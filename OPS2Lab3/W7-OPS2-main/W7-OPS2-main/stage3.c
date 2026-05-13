#include "w7-common.h"

#define BACKLOG 5
#define MAX_EVENTS 16

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

void add_to_epoll(int epoll_fd, int fd)
{
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event))
        ERR("epoll_ctl");
}

void remove_from_epoll(int epoll_fd, int fd)
{
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL))
        ERR("epoll_ctl");
}

void handle_new_connection(int epoll_fd, int listen_fd)
{
    int client_fd = add_new_client(listen_fd);
    if (client_fd < 0)
        return;

    char msg[] = "Welcome, elector!\n";

    if (bulk_write(client_fd, msg, strlen(msg)) < 0)
    {
        if (errno != EPIPE)
            ERR("write");

        if (close(client_fd))
            ERR("close");

        return;
    }

    add_to_epoll(epoll_fd, client_fd);
}

void disconnect_client(int epoll_fd, int fd)
{
    remove_from_epoll(epoll_fd, fd);

    if (close(fd))
        ERR("close");
}

void handle_client(int epoll_fd, int fd)
{
    char buf[256];

    ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf) - 1));

    if (bytes_read < 0)
        ERR("read");

    if (bytes_read == 0)
    {
        disconnect_client(epoll_fd, fd);
        return;
    }

    buf[bytes_read] = '\0';
    printf("%s", buf);
    fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc != 2)
        usage(argv[0]);

    uint16_t port = atoi(argv[1]);
    if (port <= 0)
        usage(argv[0]);

    sethandler(SIG_IGN, SIGPIPE);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        ERR("epoll_create1");

    int listen_fd = bind_tcp_socket(port, BACKLOG);

    int flags = fcntl(listen_fd, F_GETFL) | O_NONBLOCK;
    if (fcntl(listen_fd, F_SETFL, flags))
        ERR("fcntl");

    add_to_epoll(epoll_fd, listen_fd);

    while (1)
    {
        struct epoll_event event;

        if (epoll_wait(epoll_fd, &event, 1, -1) < 0)
            ERR("epoll_wait");

        int fd = event.data.fd;

        if (fd == listen_fd)
            handle_new_connection(epoll_fd, listen_fd);
        else
            handle_client(epoll_fd, fd);
    }

    return 0;
}