#include "w7-common.h"

#define BACKLOG 5
#define ELECTORS 7

// WITHOUT STAGE 4!

typedef struct 
{
    int fd;
    int vote;
} elector_t;

char* COUNTRY_NAMES[] = {"Mainz", "Trier", "Cologne", "Bohemia", "Palatinate", "Saxony", "Brandenburg"};

volatile sig_atomic_t do_work = 1;
void sigint_handler(int sig)
{
    do_work = 0;
}

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
        ERR("cpoll_ctl");
}

void handle_new_connection(int epoll_fd, int listen_fd)
{
    int nfd = add_new_client(listen_fd);

    // char msg[] = "Welcome, elector!";
    // if (bulk_write(nfd, msg, strlen(msg)) < 0)
    // {
    //     if (errno != EPIPE)
    //         ERR("write");
    //     if (close(nfd))
    //         ERR("close");
    //     return;
    // }
    add_to_epoll(epoll_fd, nfd);
}

int get_idx(int fd, elector_t electors[ELECTORS])
{
    for (int i=0;i<ELECTORS;++i)
    {
        if (electors[i].fd == fd)
            return i;
    }
    return -1;
}

void disconnect_client(int epoll_fd, int fd, elector_t electors[ELECTORS])
{
    remove_from_epoll(epoll_fd, fd);
    if (close(fd))
        ERR("close");
    int idx = get_idx(fd, electors);
    if (idx >= 0)
        electors[idx].fd = -1;
}

void handle_client(int epoll_fd, int fd, elector_t electors[ELECTORS])
{
    char c;
    ssize_t bytes_read;
    if ((bytes_read = bulk_read(fd, &c, 1)) < 0)
        ERR("read");

    if (bytes_read == 0)
    {
        // client disconnected
        disconnect_client(epoll_fd, fd, electors);
        return;
    }

    int elector_idx = get_idx(fd, electors);
    
    if (elector_idx == -1)
    {
        if (c < '1' || c > '7')
        {
            disconnect_client(epoll_fd, fd, electors);
            return;
        }
        int idx = c - '1';
        if (electors[idx].fd >= 0) // elector is connected
        {
            disconnect_client(epoll_fd, fd, electors);
            return;
        }
        char buf[256];
        snprintf(buf, 256, "Welcome, elector of %s!\n", COUNTRY_NAMES[idx]);

        if (bulk_write(fd, buf, strlen(buf)) < 0)
        {
            if (errno != EPIPE)
                ERR("write");
            disconnect_client(epoll_fd, fd, electors);
            return;
        }

        electors[idx].fd = fd;
    }
    else 
    {
        if (c < '1' || c > '3')
            return;

        electors[elector_idx].vote = c - '1' + 1;
    }
}

void server(uint16_t port)
{
    sethandler(SIG_IGN, SIGPIPE);
    sethandler(sigint_handler, SIGINT);

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        ERR("epoll_create1");

    int listen_fd = bind_tcp_socket(port, BACKLOG);
    int flags = fcntl(listen_fd, F_GETFL) | O_NONBLOCK;
    fcntl(listen_fd, F_SETFL, flags);
    
    add_to_epoll(epoll_fd, listen_fd);

    elector_t electors[ELECTORS];
    for (int i=0;i<ELECTORS;++i)
    {
        electors[i].fd = -1;
        electors[i].vote = 0;
    }

    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    while (do_work)
    {
        struct epoll_event current_event;
        if (epoll_pwait(epoll_fd, &current_event, 1, -1, &oldmask) <= 0)
        {
            if (errno == EINTR)
            {
                printf("SIGINT caught\n");
                break;
            }
            else {
                ERR("epoll_pwait");
            }
        }

        int current_fd = current_event.data.fd;

        if (current_fd == listen_fd)
        {
            // accept a new connection
            handle_new_connection(epoll_fd, listen_fd);
        }
        else 
        {
            handle_client(epoll_fd, current_fd, electors);
        }
    }

    if (close(listen_fd)) // stop listening
        ERR("close");

    for (int i=0;i<ELECTORS;++i)
    {
        if (electors[i].fd >= 0)
        {
            if (close(electors[i].fd))
                ERR("close");
        }
    }

    if (close(epoll_fd))
        ERR("close");
}

int main(int argc, char **argv) 
{ 
    if (argc != 2)
    {
        usage(argv[0]);
    }
    uint16_t port = atoi(argv[1]);
    if (port <= 0)
    {
        usage(argv[0]);
    }

    server(port);

    return 0; 
}
