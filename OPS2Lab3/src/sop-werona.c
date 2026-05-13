#include "l7-common.h"

void usage(char *name)
{
    printf("%s <timeout>\n", name);
    printf("  timeout - max waiting time after receiving the last message/connection (in seconds)\n");
    exit(EXIT_FAILURE);
}

#define SWAP(a, b)                      \
    do                                  \
    {                                   \
        __typeof__(a) __a = (a);        \
        __typeof__(b) __b = (b);        \
        __typeof__(*__a) __tmp = *__a;  \
        *__a = *__b;                    \
        *__b = __tmp;                   \
    } while (0)

#define MAX_CLIENTS 10
#define MAX_PAIRS 3
#define UNIX_SK_NAME "Laurenty"
#define MAX_MSG_LEN 63

typedef enum
{
    CLIENT_EMPTY,
    CLIENT_WAIT_NAME,
    CLIENT_WAIT_BELOVED,
    CLIENT_WAIT_PAIR,
    CLIENT_PAIRED
} client_state_t;

typedef struct
{
    int fd;
    client_state_t state;

    char name[MAX_MSG_LEN + 1];
    char beloved[MAX_MSG_LEN + 1];

    char buffer[MAX_MSG_LEN + 1];
    size_t used;

    int partner;
} client_t;

/* Small helper: make socket non-blocking, because we use epoll. */
void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        ERR("fcntl");

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        ERR("fcntl");
}

/* Add descriptor to epoll. */
void epoll_add(int epoll_fd, int fd)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));

    event.events = EPOLLIN | EPOLLRDHUP;
    event.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
        ERR("epoll_ctl ADD");
}

/* Remove descriptor from epoll. */
void epoll_del(int epoll_fd, int fd)
{
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0)
    {
        if (errno != EBADF && errno != ENOENT)
            ERR("epoll_ctl DEL");
    }
}

void init_clients(client_t clients[])
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].fd = -1;
        clients[i].state = CLIENT_EMPTY;
        clients[i].name[0] = '\0';
        clients[i].beloved[0] = '\0';
        clients[i].buffer[0] = '\0';
        clients[i].used = 0;
        clients[i].partner = -1;
    }
}

int find_free_client(client_t clients[])
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].state == CLIENT_EMPTY)
            return i;
    }

    return -1;
}

int find_client_by_fd(client_t clients[], int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].state != CLIENT_EMPTY && clients[i].fd == fd)
            return i;
    }

    return -1;
}

int find_client_by_name(client_t clients[], const char *name)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].state != CLIENT_EMPTY && strcmp(clients[i].name, name) == 0)
            return i;
    }

    return -1;
}

/* Send one complete line to client. */
void send_line(int fd, const char *line)
{
    char msg[MAX_MSG_LEN * 2 + 128];

    snprintf(msg, sizeof(msg), "%s\n", line);

    if (bulk_write(fd, msg, strlen(msg)) < 0)
    {
        /*
            We ignore SIGPIPE in main.
            If client disconnected, write gives EPIPE instead of killing server.
            This follows the pipe/socket broken-link practice from your L5 material.
        */
        if (errno != EPIPE)
            ERR("bulk_write");
    }
}

/* Close one client and clear its structure. */
void close_one_client(client_t clients[], int idx, int epoll_fd)
{
    if (clients[idx].fd >= 0)
    {
        epoll_del(epoll_fd, clients[idx].fd);
        close(clients[idx].fd);
    }

    clients[idx].fd = -1;
    clients[idx].state = CLIENT_EMPTY;
    clients[idx].name[0] = '\0';
    clients[idx].beloved[0] = '\0';
    clients[idx].buffer[0] = '\0';
    clients[idx].used = 0;
    clients[idx].partner = -1;
}

/* Disconnect one client. If they were paired, also disconnect the partner. */
void close_client_or_pair(client_t clients[], int idx, int epoll_fd, int *pairs)
{
    int partner = clients[idx].partner;

    if (clients[idx].state == CLIENT_PAIRED &&
        partner >= 0 &&
        clients[partner].state == CLIENT_PAIRED)
    {
        clients[partner].partner = -1;
        clients[idx].partner = -1;

        if (*pairs > 0)
            (*pairs)--;

        close_one_client(clients, partner, epoll_fd);
        close_one_client(clients, idx, epoll_fd);
    }
    else
    {
        close_one_client(clients, idx, epoll_fd);
    }
}

void strip_cr(char *line)
{
    size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\r')
        line[len - 1] = '\0';
}

/*
    STAGE 3:
    After obtaining both names, try to find a beloved person among clients.
    Pair is valid only if:
        A says they love B
        B says they love A
*/
void try_make_pair(client_t clients[], int idx, int *pairs)
{
    if (clients[idx].state != CLIENT_WAIT_PAIR)
        return;

    if (*pairs >= MAX_PAIRS)
        return;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (i == idx)
            continue;

        if (clients[i].state != CLIENT_WAIT_PAIR)
            continue;

        if (strcmp(clients[idx].name, clients[i].beloved) == 0 &&
            strcmp(clients[idx].beloved, clients[i].name) == 0)
        {
            clients[idx].state = CLIENT_PAIRED;
            clients[i].state = CLIENT_PAIRED;

            clients[idx].partner = i;
            clients[i].partner = idx;

            (*pairs)++;

            printf("%s and %s got married!\n", clients[idx].name, clients[i].name);

            char msg[MAX_MSG_LEN * 2 + 64];
            snprintf(msg, sizeof(msg), "Congratulations, %s and %s!", clients[idx].name, clients[i].name);

            /*
                Stage 3 says to send congratulations to both.
                Stage 4 later needs the connection to stay alive for messages,
                so in the final version we do NOT close them here.
            */
            send_line(clients[idx].fd, msg);
            send_line(clients[i].fd, msg);

            return;
        }
    }
}

/*
    STAGE 2 + STAGE 4:
    Handle one complete line from client.
    The first complete line is the client's name.
    The second complete line is beloved's name.
    Later lines are love messages forwarded to partner.
*/
void handle_client_line(client_t clients[], int idx, char *line, int epoll_fd, int *pairs)
{
    strip_cr(line);

    if (clients[idx].state == CLIENT_WAIT_NAME)
    {
        if (strlen(line) == 0)
        {
            close_client_or_pair(clients, idx, epoll_fd, pairs);
            return;
        }

        strncpy(clients[idx].name, line, MAX_MSG_LEN);
        clients[idx].name[MAX_MSG_LEN] = '\0';

        clients[idx].state = CLIENT_WAIT_BELOVED;
        return;
    }

    if (clients[idx].state == CLIENT_WAIT_BELOVED)
    {
        if (strlen(line) == 0)
        {
            printf("I lost contact with %s!\n",
                   strlen(clients[idx].name) > 0 ? clients[idx].name : "???");

            close_client_or_pair(clients, idx, epoll_fd, pairs);
            return;
        }

        strncpy(clients[idx].beloved, line, MAX_MSG_LEN);
        clients[idx].beloved[MAX_MSG_LEN] = '\0';

        printf("%s wants to marry %s\n", clients[idx].name, clients[idx].beloved);

        clients[idx].state = CLIENT_WAIT_PAIR;

        try_make_pair(clients, idx, pairs);
        return;
    }

    if (clients[idx].state == CLIENT_WAIT_PAIR)
    {
        /*
            Stage 4 says: if there is no partner, message is discarded.
            So messages before pairing are ignored.
        */
        return;
    }

    if (clients[idx].state == CLIENT_PAIRED)
    {
        int partner = clients[idx].partner;

        if (partner < 0 || clients[partner].state != CLIENT_PAIRED)
            return;

        /*
            STAGE 4:
            complete client messages are forwarded to the partner.
        */
        send_line(clients[partner].fd, line);
    }
}

/*
    STAGE 2:
    If client disconnects before sending both names, print lost-contact message.
*/
void handle_client_eof(client_t clients[], int idx, int epoll_fd, int *pairs)
{
    if (clients[idx].state == CLIENT_WAIT_NAME || clients[idx].state == CLIENT_WAIT_BELOVED)
    {
        printf("I lost contact with %s!\n",
               strlen(clients[idx].name) > 0 ? clients[idx].name : "???");
    }

    close_client_or_pair(clients, idx, epoll_fd, pairs);
}

/*
    Read data from client and split it into newline-terminated messages.
    Important task rule: every complete message ends with '\n'
    and no complete message exceeds MAX_MSG_LEN.
*/
void handle_client_data(client_t clients[], int idx, int epoll_fd, int *pairs)
{
    char tmp[128];

    for (;;)
    {
        ssize_t count = TEMP_FAILURE_RETRY(read(clients[idx].fd, tmp, sizeof(tmp)));

        if (count < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            handle_client_eof(clients, idx, epoll_fd, pairs);
            return;
        }

        if (count == 0)
        {
            handle_client_eof(clients, idx, epoll_fd, pairs);
            return;
        }

        for (ssize_t i = 0; i < count; i++)
        {
            char c = tmp[i];

            if (c == '\n')
            {
                clients[idx].buffer[clients[idx].used] = '\0';
                handle_client_line(clients, idx, clients[idx].buffer, epoll_fd, pairs);

                if (clients[idx].state == CLIENT_EMPTY)
                    return;

                clients[idx].used = 0;
                clients[idx].buffer[0] = '\0';
            }
            else
            {
                if (clients[idx].used >= MAX_MSG_LEN)
                {
                    /*
                        Wrong/incomplete too-long line.
                        We cannot store it safely, so close client.
                    */
                    handle_client_eof(clients, idx, epoll_fd, pairs);
                    return;
                }

                clients[idx].buffer[clients[idx].used++] = c;
            }
        }
    }
}

/*
    STAGE 1:
    Accept incoming clients on UNIX socket.
*/
void accept_clients(int server_fd, int epoll_fd, client_t clients[])
{
    for (;;)
    {
        int client_fd = add_new_client(server_fd);

        if (client_fd < 0)
            return;

        printf("Another young person (%d) needs my help!\n", client_fd);

        int idx = find_free_client(clients);

        if (idx < 0)
        {
            close(client_fd);
            continue;
        }

        set_nonblock(client_fd);

        clients[idx].fd = client_fd;
        clients[idx].state = CLIENT_WAIT_NAME;
        clients[idx].name[0] = '\0';
        clients[idx].beloved[0] = '\0';
        clients[idx].buffer[0] = '\0';
        clients[idx].used = 0;
        clients[idx].partner = -1;

        epoll_add(epoll_fd, client_fd);
    }
}

/*
    STAGE 4:
    Read one complete line from stdin.
    Format:
        <addressee>:<message>

    If addressee exists, send message to that client.
    Otherwise print error.
*/
void handle_stdin_line(client_t clients[], char *line)
{
    strip_cr(line);

    char *colon = strchr(line, ':');

    if (colon == NULL)
    {
        printf("Wrong stdin message format: %s\n", line);
        return;
    }

    *colon = '\0';

    char *addressee = line;
    char *message = colon + 1;

    if (strlen(addressee) == 0 || strlen(message) == 0)
    {
        printf("Wrong stdin message format\n");
        return;
    }

    int idx = find_client_by_name(clients, addressee);

    if (idx < 0)
    {
        printf("Incorrect addressee: %s\n", addressee);
        return;
    }

    send_line(clients[idx].fd, message);
}

/*
    STAGE 4:
    Standard input is also handled with epoll.
*/
void handle_stdin_data(client_t clients[])
{
    static char buffer[MAX_MSG_LEN + 1];
    static size_t used = 0;

    char tmp[128];

    for (;;)
    {
        ssize_t count = TEMP_FAILURE_RETRY(read(STDIN_FILENO, tmp, sizeof(tmp)));

        if (count < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;

            ERR("read stdin");
        }

        if (count == 0)
            return;

        for (ssize_t i = 0; i < count; i++)
        {
            char c = tmp[i];

            if (c == '\n')
            {
                buffer[used] = '\0';
                handle_stdin_line(clients, buffer);
                used = 0;
                buffer[0] = '\0';
            }
            else
            {
                if (used >= MAX_MSG_LEN)
                {
                    printf("Wrong stdin message format: line too long\n");
                    used = 0;
                    buffer[0] = '\0';
                    return;
                }

                buffer[used++] = c;
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
        usage(argv[0]);

    int timeout = atoi(argv[1]);

    if (timeout < 1)
        usage(argv[0]);

    /*
        Referenced from uploaded starter code:
        SIGPIPE must not kill the server when writing to a disconnected socket.
    */
    if (sethandler(SIG_IGN, SIGPIPE) < 0)
        ERR("sethandler");

    /*
        STAGE 1:
        Create UNIX stream socket named Laurenty.
        This matches base_client.sh:
            nc ... -U Laurenty
    */
    int server_fd = bind_local_socket(UNIX_SK_NAME, MAX_CLIENTS);
    set_nonblock(server_fd);

    /*
        We use epoll because the task explicitly asks for epoll/select/poll,
        and later stages need many clients plus stdin.
    */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        ERR("epoll_create1");

    epoll_add(epoll_fd, server_fd);

    /*
        STAGE 4:
        Also listen on stdin.
    */
    set_nonblock(STDIN_FILENO);
    epoll_add(epoll_fd, STDIN_FILENO);

    client_t clients[MAX_CLIENTS];
    init_clients(clients);

    struct epoll_event events[MAX_CLIENTS + 2];

    int pairs = 0;

    for (;;)
    {
        /*
            Stage 1/2:
            timeout means no new connection/message for this many seconds.
        */
        int ready = epoll_wait(epoll_fd, events, MAX_CLIENTS + 2, timeout * 1000);

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            ERR("epoll_wait");
        }

        if (ready == 0)
        {
            printf("No one needs my help anymore!\n");
            break;
        }

        for (int i = 0; i < ready; i++)
        {
            int fd = events[i].data.fd;

            if (fd == server_fd)
            {
                accept_clients(server_fd, epoll_fd, clients);
                continue;
            }

            if (fd == STDIN_FILENO)
            {
                handle_stdin_data(clients);
                continue;
            }

            int idx = find_client_by_fd(clients, fd);

            if (idx < 0)
                continue;

            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
            {
                handle_client_eof(clients, idx, epoll_fd, &pairs);
                continue;
            }

            if (events[i].events & EPOLLIN)
            {
                handle_client_data(clients, idx, epoll_fd, &pairs);
            }
        }
    }

    /*
        Cleanup:
        remove all client descriptors, server descriptor, epoll descriptor,
        and unlink UNIX socket file.
    */
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].state != CLIENT_EMPTY)
            close_one_client(clients, i, epoll_fd);
    }

    epoll_del(epoll_fd, STDIN_FILENO);
    epoll_del(epoll_fd, server_fd);

    close(server_fd);
    close(epoll_fd);

    unlink(UNIX_SK_NAME);

    return EXIT_SUCCESS;
}