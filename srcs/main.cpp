#include "global.hpp"

void *StartWorker(void *args) {
    Client              *clients[FD_SETSIZE];
    bool                all_receive[FD_SETSIZE];
    bool                close_conn[FD_SETSIZE];
    char                buffer[RECV_BUFFER_SIZE];
    fd_set              master_set, working_read_set, working_write_set;
    int                 len, rc;
    int                 desc_ready;
    //int                 max_sd = FD_SETSIZE;
    int                 new_sd;
    std::set<int>       openclients;

    t_worker            *worker = static_cast<t_worker*>(args);
    char **env      =   worker->env;
    Server *server  =   worker->serv;
    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    for (int i = 0; i < FD_SETSIZE; i++)
    {
        all_receive[i] = false;
        close_conn[i] = false;
    }

    int count = 0;
    noblock(server_socket);
    while (true)
    {
        memcpy(&working_read_set, &master_set, sizeof(master_set));
        memcpy(&working_write_set, &master_set, sizeof(master_set));
        Debug::info("Waiting on select()...");
        rc = select(FD_SETSIZE, &working_read_set, &working_write_set, NULL, NULL);
        if (rc < 0)
        {
            Debug::error("select() failed [" + std::string(strerror(errno)) + "]");
            break;
        }
        Debug::info("RC ", rc);
        desc_ready = rc;
        if (FD_ISSET(server_socket, &working_read_set))
        {
            desc_ready -= 1;
            new_sd = accept(server_socket, NULL, NULL);
            if (new_sd < 0)
            {
                if (errno != EWOULDBLOCK)
                {
                    Debug::error("accept() failed [" + std::string(strerror(errno)) + "]");
                    exit(1);
                }
                break;
            }
            else {
                Debug::checkpoint("New incoming connection - ", new_sd);
                FD_SET(new_sd, &master_set);
                openclients.insert(new_sd);
                noblock(new_sd);
                clients[new_sd] = new Client(new_sd, env, server);
                count++;
            }
        }
        else
        {
            for (std::set<int>::iterator it = openclients.begin(); it != openclients.end(); it++)
            {
                int i = *it;
                Debug::error("HERE ONCE");
                if (FD_ISSET(i, &working_read_set))
                {
                    Debug::checkpoint("Descriptor is readable - Count:", count);
                    if (!all_receive[i])
                    {
                        rc = recv(i, buffer, sizeof(buffer), 0);
                        if (rc < 0)
                        {
                            if (errno != EWOULDBLOCK && errno != EAGAIN)
                            {
                                Debug::error("recv() failed [" + std::string(strerror(errno)) + "]");
                                close_conn[i] = true;
                                break;
                            }
                            else
                                Debug::error("send() failed [" + std::string(strerror(errno)) + "]");
                        }
                        else
                        {
                            len = rc;
                            //Debug::info("bytes received: [" + Debug::escapestr(std::string(buffer, len)) + "]", len);
                            clients[i]->append(std::string(buffer, len));

                            if (clients[i]->hasFinishedReading())
                            {
                                Debug::checkpoint("Descriptor is finished reading - Count:", count);
                                clients[i]->process();
                                all_receive[i] = true;
                            }
                        }
                    }
                }
                if (FD_ISSET(i, &working_write_set))
                {
                    if (all_receive[i])
                    {
                        Debug::checkpoint("Send : ", clients[i]->getResponseLength());
                        rc = send(i, clients[i]->getResponse(), clients[i]->getResponseLength(), 0);
                        Debug::error("RC : ", rc);
                        if (rc < 0)
                        {
                            if (errno != EWOULDBLOCK && errno != EAGAIN)
                            {
                                Debug::error("send() failed [" + std::string(strerror(errno)) + "]");
                                close_conn[i] = true;
                                break;
                            }
                            else
                            {
                                Debug::error("send() failed [" + std::string(strerror(errno)) + "]");
                                close_conn[i] = true;
                            }
                        }
                        else {
                            clients[i]->substract(rc);
                            if (clients[i]->hasFinishedSending())
                            {
                                Debug::checkpoint("Connection closed");
                                close_conn[i] = true;
                            }
                        }
                    }
                    if (close_conn[i])
                    {
                        close(i);
                        FD_CLR(i, &master_set);
                        delete clients[i];
                        all_receive[i] = false;
                        close_conn[i] = false;
                        openclients.erase(i);
                        Debug::checkpoint("Close FD");
                        break;
                    }
                }
            }
        }
    }
    pthread_exit(NULL);
}

void RunServer(Server server, char **env)
{
    int                     rc, on = 1;
    struct sockaddr_in      addr;
    //pthread_t               *workers = (pthread_t *)malloc(WORKERS * sizeof(pthread_t));
    t_worker                data;

    addr.sin_family = AF_INET;
	addr.sin_port = htons(server.getPort());
	addr.sin_addr.s_addr = inet_addr(server.getHost().c_str());

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        Debug::error("socket() failed [" + std::string(strerror(errno)) + "]");
        exit(-1);
    }

    signal(SIGINT, int_handler);
    rc = setsockopt(server_socket, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
    if (rc < 0)
    {
        Debug::error("setsockopt() failed [" + std::string(strerror(errno)) + "]");
        close(server_socket);
        exit(-1);
    }

    rc = noblock(server_socket);
    if (rc < 0)
    {
        Debug::error("noblock() failed [" + std::string(strerror(errno)) + "]");
        close(server_socket);
        exit(-1);
    }

    rc = bind(server_socket, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0)
    {
        Debug::error("bind() failed [" + std::string(strerror(errno)) + "]");
        close(server_socket);
        exit(-1);
    }

    rc = listen(server_socket, MAX_CONNECTIONS);
    if (rc < 0)
    {
        Debug::error("listen() failed [" + std::string(strerror(errno)) + "]");
        close(server_socket);
        exit(-1);
    }

    data.env = env;
    data.serv = &server;

    for (int i = 0; i < WORKERS; i++)
    {
        pid[i] = fork();
        if (!pid[i]) {
            StartWorker(&data);
        }
    }
    while (true);
}

int main(int ac, char **av, char **env)
{
    if (ac >= 2)
    {
        char *path;
        char buf[2048];

        getcwd(buf, 2048);
        path = ft_strjoin(buf, "/");
        path = ft_strfjoin(path, av[1], 1);
        Parser p(path);
        Debug::checkpoint("Path: " + std::string(path));
        free(path);
        Server s = p.getConfig().at(0);
        RunServer(s, env);
    }
    else
        Debug::error("You need to specified a config path.");
    return (0);
}