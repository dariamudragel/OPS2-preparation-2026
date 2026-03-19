#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_KNIGHT_NAME_LENGHT 20

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

typedef struct
{
    char name[MAX_KNIGHT_NAME_LENGHT + 1];
    int hp, attack;

} knight;

int set_handler(void (*f)(int), int sig)
{
    struct sigaction act = {0};
    act.sa_handler = f;
    if (sigaction(sig, &act, NULL) == -1)
        return -1;
    return 0;
}

void msleep(int millisec)
{
    struct timespec tt;
    tt.tv_sec = millisec / 1000;
    tt.tv_nsec = (millisec % 1000) * 1000000;
    while (nanosleep(&tt, &tt) == -1)
    {
    }
}

int count_descriptors()
{
    int count = 0;
    DIR* dir;
    struct dirent* entry;
    struct stat stats;
    if ((dir = opendir("/proc/self/fd")) == NULL)
        ERR("opendir");
    char path[PATH_MAX];
    getcwd(path, PATH_MAX);
    chdir("/proc/self/fd");
    do
    {
        errno = 0;
        if ((entry = readdir(dir)) != NULL)
        {
            if (lstat(entry->d_name, &stats))
                ERR("lstat");
            if (!S_ISDIR(stats.st_mode))
                count++;
        }
    } while (entry != NULL);
    if (chdir(path))
        ERR("chdir");
    if (closedir(dir))
        ERR("closedir");
    return count - 1;  // one descriptor for open directory
}

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

    if (fscanf(franci, "%d", nf) != 1)
        ERR("fscanf");
    *fr = malloc(sizeof(knight) * (*nf));
    if (!(*fr))
        ERR("malloc");

    for (int i = 0; i < *nf; i++)
    {
        if (fscanf(franci, "%20s %d %d", (*fr)[i].name, &(*fr)[i].hp, &(*fr)[i].attack) != 3)
            ERR("fscanf");

        printf("I am Frankish knight %s. I will serve my king with my %d HP and %d attack.\n", (*fr)[i].name,
               (*fr)[i].hp, (*fr)[i].attack);
    }

    if (fscanf(saraceni, "%d", ns) != 1)
        ERR("fscanf");
    *sa = malloc(sizeof(knight) * (*ns));
    if (!(*sa))
        ERR("malloc");

    for (int i = 0; i < *ns; i++)
    {
        if (fscanf(saraceni, "%20s %d %d", (*sa)[i].name, &(*sa)[i].hp, &(*sa)[i].attack) != 3)
            ERR("fscanf");

        printf("I am Spanish knight %s. I will serve my king with my %d HP and %d attack.\n", (*sa)[i].name,
               (*sa)[i].hp, (*sa)[i].attack);
    }

    fclose(franci);
    fclose(saraceni);

    return 0;
}

int main(int argc, char* argv[])
{
    srand(time(NULL));
    knight* fr = NULL;
    knight* sc = NULL;

    int nf = 0;
    int ns = 0;

    stage1(&fr, &sc, &nf, &ns);

    free(fr);
    free(sc);
    printf("Opened descriptors: %d\n", count_descriptors());
}
