#define _GNU_SOURCE
#include "l7-common.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNIX_SK_NAME "Laurenty"
#define MAX_CLIENTS 10
#define MAX_MSG_LEN 63

typedef struct client
{
    int fd;

    char name[MAX_MSG_LEN + 1];
    char beloved[MAX_MSG_LEN + 1];

    int has_name;
    int has_beloved;

    char buf[MAX_MSG_LEN + 1];
    int buf_len;
} client_t;

void usage(char *name)
{
    fprintf(stderr, "Usage: %s <timeout>\n", name);
    exit(EXIT_FAILURE);
}

void init_clients(client_t clients[])
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].fd = -1;
        clients[i].has_name = 0;
        clients[i].has_beloved = 0;
        clients[i].buf_len = 0;
        clients[i].name[0] = '\0';
        clients[i].beloved[0] = '\0';
        clients[i].buf[0] = '\0';
    }
}

int find_free_client(client_t clients[])
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd == -1)
            return i;

    return -1;
}

void close_client(client_t clients[], int i)
{
    if (clients[i].fd != -1)
        close(clients[i].fd);

    clients[i].fd = -1;
    clients[i].has_name = 0;
    clients[i].has_beloved = 0;
    clients[i].buf_len = 0;
    clients[i].name[0] = '\0';
    clients[i].beloved[0] = '\0';
    clients[i].buf[0] = '\0';
}

void lost_contact(client_t clients[], int i)
{
    if (!clients[i].has_name)
        printf("I lost contact with ??!\n");
    else if (!clients[i].has_beloved)
        printf("I lost contact with %s!\n", clients[i].name);

    close_client(clients, i);
}

void handle_complete_line(client_t clients[], int i, char *line)
{
    if (!clients[i].has_name)
    {
        strncpy(clients[i].name, line, MAX_MSG_LEN);
        clients[i].name[MAX_MSG_LEN] = '\0';
        clients[i].has_name = 1;
        return;
    }

    if (!clients[i].has_beloved)
    {
        strncpy(clients[i].beloved, line, MAX_MSG_LEN);
        clients[i].beloved[MAX_MSG_LEN] = '\0';
        clients[i].has_beloved = 1;

        printf("%s wants to marry %s\n", clients[i].name, clients[i].beloved);

        close_client(clients, i); // Stage 2: after two names, close connection
    }
}

void read_from_client(client_t clients[], int i)
{
    char tmp[64];

    ssize_t count = TEMP_FAILURE_RETRY(read(clients[i].fd, tmp, sizeof(tmp)));

    if (count < 0)
    {
        perror("read");
        lost_contact(clients, i);
        return;
    }

    if (count == 0)
    {
        lost_contact(clients, i);
        return;
    }

    for (ssize_t k = 0; k < count; k++)
    {
        if (tmp[k] == '\n')
        {
            clients[i].buf[clients[i].buf_len] = '\0';
            handle_complete_line(clients, i, clients[i].buf);

            if (clients[i].fd == -1)
                return;

            clients[i].buf_len = 0;
            clients[i].buf[0] = '\0';
        }
        else
        {
            if (clients[i].buf_len < MAX_MSG_LEN)
                clients[i].buf[clients[i].buf_len++] = tmp[k];
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
        usage(argv[0]);

    int timeout = atoi(argv[1]);
    if (timeout <= 0)
        usage(argv[0]);

    sethandler(SIG_IGN, SIGPIPE);

    client_t clients[MAX_CLIENTS];
    init_clients(clients);

    int server_fd = bind_local_socket(UNIX_SK_NAME, MAX_CLIENTS);

    while (1)
    {
        struct pollfd fds[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS + 1];
        int nfds = 0;

        fds[nfds].fd = server_fd;
        fds[nfds].events = POLLIN;
        map[nfds] = -1;
        nfds++;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].fd != -1)
            {
                fds[nfds].fd = clients[i].fd;
                fds[nfds].events = POLLIN;
                map[nfds] = i;
                nfds++;
            }
        }

        int ret = TEMP_FAILURE_RETRY(poll(fds, nfds, timeout * 1000));

        if (ret < 0)
            ERR("poll");

        if (ret == 0)
        {
            printf("No one needs my help anymore!\n");
            break;
        }

        for (int k = 0; k < nfds; k++)
        {
            if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            if (map[k] == -1)
            {
                int client_fd = add_new_client(server_fd);

                if (client_fd >= 0)
                {
                    int pos = find_free_client(clients);

                    if (pos == -1)
                    {
                        close(client_fd);
                    }
                    else
                    {
                        clients[pos].fd = client_fd;
                        printf("Another young person (%d) needs my help!\n", client_fd);
                    }
                }
            }
            else
            {
                read_from_client(clients, map[k]);
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1)
            close_client(clients, i);

    close(server_fd);
    unlink(UNIX_SK_NAME);

    return EXIT_SUCCESS;
}