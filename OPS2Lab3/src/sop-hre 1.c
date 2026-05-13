#include "l4-common.h"

#define ELECTOR_NUM 7
#define CANDIDATES_NUM 3

volatile sig_atomic_t do_work = 1;

void sigint_handler(int sig)
{
    do_work = 0;
}

typedef struct {
    int connected;
    int fd;
    char vote;
} elector_t;

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

void do_server(int server_socket_fd, sigset_t* old_mask)
{
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) ERR("epoll_create");

    struct epoll_event event;
    event.data.fd = server_socket_fd;
    event.events = EPOLLIN;

    int res = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket_fd, &event);
    if (res < 0) ERR("epoll_ctl");

    struct epoll_event events[20];

    elector_t electors[ELECTOR_NUM];
    for (int i = 0; i < ELECTOR_NUM; ++i) {
        electors[i].connected = 0;
        electors[i].fd = -1;
        electors[i].vote = -1;
    }

    while(1)
    {
//        int number_of_fds = epoll_wait(epoll_fd, events, 20, -1);
        int number_of_fds = epoll_pwait(epoll_fd, events, 20, -1, old_mask);
        if (number_of_fds < 0)
        {
            if (errno == EINTR)
            {
                // Handle SIGINT:
                if (!do_work) break;
            } else ERR("epoll_wait");
        }
        for (int i = 0; i < number_of_fds; i++)
        {
            event = events[i]; // currently handled event
            int fd = event.data.fd; // file descriptor of currently handled event

            if (fd == server_socket_fd) // server, we need to accept new client
            {
                int client_fd = add_new_client(fd);

                printf("New client connected!\n");
                // When we are not sure how many byte we will read from client
                // So that when we read to some buffer it will read up to the buffer capacity
                // and if there was not that many it block us.
//                int flags = fcntl(client_fd, F_GETFL) | O_NONBLOCK;
//                fcntl(client_fd, F_SETFL, flags);

                event.events = EPOLLIN;
                event.data.fd = client_fd;

                res = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event);
                if (res < 0) ERR("epoll_ctl");
            }
            else // client, we need to handle data it sends
            {
                char message;
                res = read(fd, &message, sizeof(message));
                if (res < 0) ERR("read");
                if (res == 0) // client sent everything, we read everything, client disconnected, we may close it
                {
                    printf("Client disconnected!\n");
                    close(fd);
                    for (int j = 0; j < ELECTOR_NUM; ++j) {
                        if (electors[j].fd == fd) // remove it from electors array
                        {
                            electors[j].connected = 0;
                            electors[j].fd = -1;
                        }
                    }
                    continue;
                }
                // Handling of the message from client:
                // Check whether the elector is registered (we have its fd in the electors array)
                int registered = 0;
                for (int j = 0; j < ELECTOR_NUM; ++j) {
                    if (electors[j].fd == fd) // elector registered, we are changing its vote
                    {
                        registered = 1;
                        if (message > '0' && message < '4')
                        {
                            printf("Elector %d voted %c\n", j + 1, message);
                            electors[j].vote = message;
                        }
                        break;
                    }
                }
                if (!registered) {
                    // not registered, the message is identification (elector index)
                    if (message > '0' && message < '8')
                    {
                        int index = message - '1';
                        if (!electors[index].connected) // that elector is not connecting, we initialize it
                        {
                            printf("Elector %c registered\n", message);
                            electors[index].connected = 1;
                            electors[index].fd = fd;
                        }
                    }
                }
            }
        }
    }

    for (int j = 0; j < ELECTOR_NUM; ++j)
    {
        if (electors[j].vote > 0)
        {
            printf("Elector %d voted %c\n", j + 1, electors[j].vote);
        }
        close(electors[j].fd);
    }

    close(epoll_fd);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        usage(argv[0]);

    // block the signals we want to handle:
    sigset_t mask, old_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &old_mask);
    // and set handler!
    sethandler(sigint_handler, SIGINT);

    uint16_t port = atoi(argv[1]);

    int socket_fd = bind_tcp_socket(port, 8);

    do_server(socket_fd, &old_mask);

    close(socket_fd);
    return 0;
}
