#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_GRAPH_NODES 128
#define MAX_PATH_LENGTH 64
#define FIFO_NAME "ANTS_FIFO"
#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), kill(0, SIGKILL), exit(EXIT_FAILURE))

typedef struct
{
    int neighbors[MAX_GRAPH_NODES];
    int count;
} Node;

typedef struct
{
    int id;
    int path[MAX_PATH_LENGTH];
    int path_length;
} Ant;

static volatile sig_atomic_t stop_flag = 0;
static int my_read_fd = -1;

/*
REFERENCES USED:

1. Stage 3 structure
   - anonymous pipes between node processes

2. POSIX-pipes slides
   - mkfifo()
   - nonblocking FIFO open/read
   - FIFO as a named pipe shared by many processes

3. Signals slides
   - SIGINT cleanup
*/

static void msleep(int ms)
{
    struct timespec tt;
    tt.tv_sec = ms / 1000;
    tt.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&tt, &tt) < 0)
        if (errno != EINTR)
            ERR("nanosleep");
}

static void sigint_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
    if (my_read_fd >= 0)
        close(my_read_fd);
}

static void set_handler(void (*handler)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (sigaction(sig, &act, NULL) < 0)
        ERR("sigaction");
}

static void close_fd(int* fd)
{
    if (*fd >= 0)
    {
        if (TEMP_FAILURE_RETRY(close(*fd)) < 0)
            ERR("close");
        *fd = -1;
    }
}

static void load_graph(const char* path, Node graph[], int* V)
{
    FILE* f = fopen(path, "r");
    if (!f)
        ERR("fopen");

    if (fscanf(f, "%d", V) != 1)
        ERR("fscanf");

    for (int i = 0; i < *V; i++)
        graph[i].count = 0;

    int u, v;
    while (fscanf(f, "%d %d", &u, &v) == 2)
        graph[u].neighbors[graph[u].count++] = v;

    fclose(f);
}

static void node_work(int id, int destination, int read_fd, int neighbor_writes[], int neighbor_count)
{
    Ant ant;
    my_read_fd = read_fd;
    srand((unsigned int)getpid());
    set_handler(sigint_handler, SIGINT);
    set_handler(SIG_IGN, SIGPIPE);

    int fifo_fd = open(FIFO_NAME, O_WRONLY);
    if (fifo_fd < 0)
        ERR("open fifo for write");

    for (;;)
    {
        ssize_t r = read(read_fd, &ant, sizeof(ant));
        if (r <= 0)
        {
            if (r < 0 && errno == EINTR && !stop_flag)
                continue;
            break;
        }

        if (ant.path_length >= MAX_PATH_LENGTH)
        {
            printf("Ant %d: got lost\n", ant.id);
            fflush(stdout);
            continue;
        }

        ant.path[ant.path_length++] = id;

        if (id == destination)
        {
            printf("Ant %d: found food\n", ant.id);
            fflush(stdout);
            if (write(fifo_fd, &ant, sizeof(ant)) != (ssize_t)sizeof(ant))
                ;
            continue;
        }

        if (neighbor_count == 0)
        {
            printf("Ant %d: got lost\n", ant.id);
            fflush(stdout);
            continue;
        }

        msleep(100);

        int chosen = rand() % neighbor_count;
        if (write(neighbor_writes[chosen], &ant, sizeof(ant)) != (ssize_t)sizeof(ant))
        {
            printf("Ant %d: got lost\n", ant.id);
            fflush(stdout);
        }
    }

    close_fd(&fifo_fd);
    close_fd(&read_fd);
    for (int i = 0; i < neighbor_count; i++)
        close_fd(&neighbor_writes[i]);
    _exit(EXIT_SUCCESS);
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "USAGE: %s graph_file start destination\n", argv[0]);
        return EXIT_FAILURE;
    }

    Node graph[MAX_GRAPH_NODES];
    int V = 0;
    int start = atoi(argv[2]);
    int destination = atoi(argv[3]);

    load_graph(argv[1], graph, &V);

    unlink(FIFO_NAME);
    if (mkfifo(FIFO_NAME, 0666) < 0)
        ERR("mkfifo");

    int fifo_rd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK);
    if (fifo_rd < 0)
        ERR("open fifo rd");
    int fifo_dummy = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
    if (fifo_dummy < 0)
        ERR("open fifo dummy");

    int pipes[MAX_GRAPH_NODES][2];
    for (int i = 0; i < V; i++)
        if (pipe(pipes[i]) < 0)
            ERR("pipe");

    set_handler(SIG_IGN, SIGPIPE);
    set_handler(sigint_handler, SIGINT);

    for (int i = 0; i < V; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            int neighbor_writes[MAX_GRAPH_NODES];
            int nc = graph[i].count;

            for (int k = 0; k < nc; k++)
                neighbor_writes[k] = pipes[graph[i].neighbors[k]][1];

            for (int j = 0; j < V; j++)
            {
                if (j != i)
                    close_fd(&pipes[j][0]);

                int keep = 0;
                for (int k = 0; k < nc; k++)
                    if (graph[i].neighbors[k] == j)
                        keep = 1;
                if (!keep)
                    close_fd(&pipes[j][1]);
            }

            close_fd(&fifo_rd);
            close_fd(&fifo_dummy);
            node_work(i, destination, pipes[i][0], neighbor_writes, nc);
        }
    }

    for (int i = 0; i < V; i++)
    {
        if (i != start)
            close_fd(&pipes[i][1]);
        close_fd(&pipes[i][0]);
    }

    int ant_id = 1;
    while (!stop_flag)
    {
        Ant ant;
        ant.id = ant_id++;
        ant.path_length = 0;

        if (write(pipes[start][1], &ant, sizeof(ant)) != (ssize_t)sizeof(ant))
            break;

        Ant received;
        ssize_t r = read(fifo_rd, &received, sizeof(received));
        if (r == (ssize_t)sizeof(received))
        {
            printf("Ant %d path:", received.id);
            for (int i = 0; i < received.path_length; i++)
                printf(" %d", received.path[i]);
            printf("\n");
            fflush(stdout);
        }

        msleep(1000);
    }

    kill(0, SIGINT);

    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");

    close_fd(&fifo_rd);
    close_fd(&fifo_dummy);
    unlink(FIFO_NAME);
    return EXIT_SUCCESS;
}