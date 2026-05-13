#include "w7-common.h"

#define BACKLOG 5

void usage(char* name)
{
    fprintf(stderr, "USAGE: %s port\n", name);
    exit(EXIT_FAILURE);
}

int main(int argc, char** argv)
{
    if (argc != 2)
        usage(argv[0]);

    uint16_t port = atoi(argv[1]);
    if (port <= 0)
        usage(argv[0]);

    int listen_fd = bind_tcp_socket(port, BACKLOG);

    int client_fd = add_new_client(listen_fd);

    printf("Client connected\n");

    if (close(client_fd))
        ERR("close");

    if (close(listen_fd))
        ERR("close");

    return 0;
}