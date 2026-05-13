#define _GNU_SOURCE
#include "l7-common.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define UNIX_SK_NAME "Laurenty"
#define MAX_CLIENTS 10

void usage(char *name)
{
    fprintf(stderr, "Usage: %s <timeout>\n", name);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        usage(argv[0]);

    int timeout = atoi(argv[1]);
    if (timeout <= 0)
        usage(argv[0]);

    sethandler(SIG_IGN, SIGPIPE);

    int server_fd = bind_local_socket(UNIX_SK_NAME, MAX_CLIENTS);

    while (1)
    {
        struct pollfd pfd;

        pfd.fd = server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = TEMP_FAILURE_RETRY(poll(&pfd, 1, timeout * 1000));

        if (ret < 0)
            ERR("poll");

        if (ret == 0)
        {
            printf("No one needs my help anymore!\n");
            break;
        }

        if (pfd.revents & POLLIN)
        {
            int client_fd = add_new_client(server_fd);

            if (client_fd >= 0)
            {
                printf("Another young person (%d) needs my help!\n", client_fd);
                close(client_fd); // Stage 1: close immediately
            }
        }
    }

    close(server_fd);
    unlink(UNIX_SK_NAME);

    return EXIT_SUCCESS;
}