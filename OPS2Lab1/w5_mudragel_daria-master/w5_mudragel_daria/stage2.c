#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_KNIGHT_NAME_LENGHT 20
#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

typedef struct
{
    char name[MAX_KNIGHT_NAME_LENGHT + 1];
    int hp;
    int attack;
} knight;

/*
REFERENCES USED:

1. sop-roncevaux.c
   - knight struct
   - stage1 loading

2. prog22a.c
   - fork many children
   - create many pipes
   - close unused descriptors in parent/child
*/

int stage1(knight** fr, knight** sa, int* nf, int* ns)
{
    FILE* franci = fopen("franci.txt", "r");
    if (!franci)
    {
        printf("Franks have not arrived on the battlefield\n");
        exit(EXIT_FAILURE);
    }

    FILE* saraceni = fopen("saraceni.txt", "r");
    if (!saraceni)
    {
        printf("Saracens have not arrived on the battlefield\n");
        fclose(franci);
        exit(EXIT_FAILURE);
    }

    if (fscanf(franci, "%d", nf) != 1) ERR("fscanf");
    *fr = malloc(sizeof(knight) * (*nf));
    if (!(*fr)) ERR("malloc");
    for (int i = 0; i < *nf; i++)
        if (fscanf(franci, "%20s %d %d", (*fr)[i].name, &(*fr)[i].hp, &(*fr)[i].attack) != 3)
            ERR("fscanf");

    if (fscanf(saraceni, "%d", ns) != 1) ERR("fscanf");
    *sa = malloc(sizeof(knight) * (*ns));
    if (!(*sa)) ERR("malloc");
    for (int i = 0; i < *ns; i++)
        if (fscanf(saraceni, "%20s %d %d", (*sa)[i].name, &(*sa)[i].hp, &(*sa)[i].attack) != 3)
            ERR("fscanf");

    fclose(franci);
    fclose(saraceni);
    return 0;
}

static void close_fd(int* fd)
{
    if (*fd >= 0)
    {
        if (close(*fd) < 0)
            ERR("close");
        *fd = -1;
    }
}

static void knight_intro(const knight* k, int is_frank)
{
    printf("I am %s knight %s. I will serve my king with my %d HP and %d attack.\n",
           is_frank ? "Frankish" : "Spanish",
           k->name,
           k->hp,
           k->attack);
    fflush(stdout);
}

int main(void)
{
    knight *fr = NULL, *sa = NULL;
    int nf = 0, ns = 0;
    int total;
    int (*pipes)[2];
    pid_t pid;

    stage1(&fr, &sa, &nf, &ns);
    total = nf + ns;

    pipes = calloc((size_t)total, sizeof(int[2]));
    if (!pipes) ERR("calloc");

    for (int i = 0; i < total; i++)
        if (pipe(pipes[i]) < 0)
            ERR("pipe");

    for (int i = 0; i < total; i++)
    {
        pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            for (int j = 0; j < total; j++)
            {
                if (j != i)
                    close_fd(&pipes[j][0]);
                close_fd(&pipes[j][1]);
            }

            if (i < nf)
                knight_intro(&fr[i], 1);
            else
                knight_intro(&sa[i - nf], 0);

            close_fd(&pipes[i][0]);
            free(pipes);
            free(fr);
            free(sa);
            _exit(EXIT_SUCCESS);
        }
    }

    for (int i = 0; i < total; i++)
    {
        close_fd(&pipes[i][0]);
        close_fd(&pipes[i][1]);
    }

    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");

    free(pipes);
    free(fr);
    free(sa);
    return 0;
}