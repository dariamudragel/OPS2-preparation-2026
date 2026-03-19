#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_GRAPH_NODES 128
#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

typedef struct
{
    int neighbors[MAX_GRAPH_NODES];
    int count;
} Node;

/*
REFERENCES USED:

1. General process creation pattern similar to prog22a.c
   - fork many children
   - parent waits for all

2. This stage does not use pipes yet.
*/

static void load_graph(const char* path, Node graph[], int* V)
{
    FILE* f = fopen(path, "r");
    if (!f)
        ERR("fopen");

    if (fscanf(f, "%d", V) != 1)
        ERR("fscanf");

    if (*V < 1 || *V > MAX_GRAPH_NODES)
    {
        fprintf(stderr, "Invalid number of vertices\n");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < *V; i++)
        graph[i].count = 0;

    int u, v;
    while (fscanf(f, "%d %d", &u, &v) == 2)
    {
        if (u < 0 || u >= *V || v < 0 || v >= *V)
        {
            fprintf(stderr, "Invalid edge %d -> %d\n", u, v);
            exit(EXIT_FAILURE);
        }
        graph[u].neighbors[graph[u].count++] = v;
    }

    fclose(f);
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "USAGE: %s graph_file\n", argv[0]);
        return EXIT_FAILURE;
    }

    Node graph[MAX_GRAPH_NODES];
    int V = 0;

    load_graph(argv[1], graph, &V);

    for (int i = 0; i < V; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            printf("%d:", i);
            for (int j = 0; j < graph[i].count; j++)
                printf(" %d", graph[i].neighbors[j]);
            printf("\n");
            fflush(stdout);
            _exit(EXIT_SUCCESS);
        }
    }

    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");

    return EXIT_SUCCESS;
}