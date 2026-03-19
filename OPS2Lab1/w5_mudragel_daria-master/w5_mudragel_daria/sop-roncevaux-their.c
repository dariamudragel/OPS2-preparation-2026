#include <sys/types.h>
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

typedef struct {
    char name[MAX_KNIGHT_NAME_LENGHT];
    int hp;
    int attack;
    int read_fd;
    int write_fd;
} knight_t;

knight_t* read_knights(FILE* f, int frankish, int* count)
{
    int n;
    fscanf(f, "%d", &n);
    *count = n;
    knight_t* knights = (knight_t*)malloc(n * sizeof(knight_t));
    if (!knights)
        ERR("malloc");

    for (int i=0;i<n;++i)
    {
        fscanf(f, "%20s %d %d", knights[i].name, &knights[i].hp, &knights[i].attack);
    }

    if (fclose(f))
        ERR("fclose");

    return knights;
}

void create_pipes(knight_t* knights, int n)
{
    for (int i=0;i<n;++i)
    {
        int fd[2];
        if (pipe(fd))
            ERR("pipe");
        knights[i].read_fd = fd[0];
        knights[i].write_fd = fd[1];
    }
}

void child_work(knight_t* allies, int allies_count, knight_t* enemies, int enemies_count, int i, int frankish)
{
    knight_t* me = &allies[i];

    for (int j=0;j<allies_count;++j)
    {
        if (j != i)
        {
            if (close(allies[j].read_fd))
                ERR("close");
            if (close(allies[j].write_fd))
                ERR("close");
        }
        else {
            if (close(allies[j].write_fd))
                ERR("close");
        }
    }
    for (int j=0;j<enemies_count;++j)
    {
        if (close(enemies[j].read_fd))
            ERR("close");
    }

    int flags = fcntl(me->read_fd, F_GETFL);
    flags |= O_NONBLOCK;
    if (fcntl(me->read_fd, F_SETFL, flags))
        ERR("fcntl");

    printf("I am %s knight %s. I will serve my king with my %d HP and %d attack\n", frankish ? "Frankish" : "Spanish", me->name, me->hp, me->attack);

    srand(getpid());
    while (1)
    {
        uint8_t damage;
        int ret;
        while ((ret = read(me->read_fd, &damage, 1) > 0))
        {
            me->hp -= damage;
        }
        if (ret < 0 && errno != EAGAIN)
            ERR("read");

        int rand_i = rand() % enemies_count;
        uint8_t S = (uint8_t)(rand() % (me->attack + 1));
        if (write(enemies[rand_i].write_fd, &S, 1) < 0) 
            ERR("write");

        if (S == 0)
        {
            printf("%s attacks his enemy, however he deflected\n", me->name);
        }
        else if (S <= 5)
        {
            printf("%s goes to strike, he hit right and well\n", me->name);
        }
        else
        {
            printf("%s strikes powerful blow, the shield he breaks and inflicts a big wound\n", me->name);
        }

        int t = (rand() % 10) + 1;
        msleep(t);
    }

    if (close(me->read_fd))
        ERR("close");
    for (int j=0;j<enemies_count;++j)
    {
        if (close(enemies[j].write_fd))
            ERR("close");
    }

    printf("Opened descriptors: %d\n", count_descriptors());

    free(allies);
    free(enemies);
    exit(EXIT_SUCCESS);
}

int main(int argc, char* argv[])
{
    srand(time(NULL));
    set_handler(SIG_IGN, SIGPIPE);

    FILE* franci_f, *saraceni_f;
    if ((franci_f = fopen("franci.txt", "r")) == NULL)
    {
        perror("fopen");
        printf("Franks have not arrived on the battlefield\n");
        return EXIT_FAILURE;
    }
    if ((saraceni_f = fopen("saraceni.txt", "r")) == NULL)
    {
        perror("fopen");
        printf("Saracens have not arrived on the battlefield\n");
        return EXIT_FAILURE;
    }

    int franci_n, saraceni_n;
    knight_t* franci = read_knights(franci_f, 1, &franci_n);
    knight_t* saraceni = read_knights(saraceni_f, 0, &saraceni_n);

    create_pipes(franci, franci_n);
    create_pipes(saraceni, saraceni_n);

    for (int i=0;i<franci_n;++i)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            child_work(franci, franci_n, saraceni, saraceni_n, i, 1);
        }
        else if (pid < 0)
        {
            ERR("fork");
        }
    }
    for (int i=0;i<saraceni_n;++i)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            child_work(saraceni, saraceni_n, franci, franci_n, i, 0);
        }
        else if (pid < 0)
        {
            ERR("fork");
        }
    }

    for (int i=0;i<franci_n;++i)
    {
        if (close(franci[i].read_fd))
            ERR("close");
        if (close(franci[i].write_fd))
            ERR("close");
    }
    for (int i=0;i<saraceni_n;++i)
    {
        if (close(saraceni[i].read_fd))
            ERR("close");
        if (close(saraceni[i].write_fd))
            ERR("close");
    }

    while (wait(NULL) > 0);
    
    free(franci);
    free(saraceni);

    printf("Opened descriptors: %d\n", count_descriptors());
}
