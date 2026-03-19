#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_KNIGHT_NAME_LENGHT 20
#define ERR(source) (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

typedef struct
{
    char name[MAX_KNIGHT_NAME_LENGHT + 1];
    int hp;
    int attack;
} knight;

typedef struct
{
    int fd;
    int idx;
} enemy_fd;

/*
REFERENCES USED:

1. sop-roncevaux.c
   - knight struct
   - msleep idea
   - file loading

2. prog22a.c
   - one process per entity
   - many pipes
   - close unused descriptors

3. pipe rules from materials
   - one-byte writes are simple and safe here
*/

int stage1(knight** fr, knight** sa, int* nf, int* ns)
{
    FILE* franci = fopen("franci.txt", "r");
    if (!franci) { printf("Franks have not arrived on the battlefield\n"); exit(EXIT_FAILURE); }
    FILE* saraceni = fopen("saraceni.txt", "r");
    if (!saraceni) { printf("Saracens have not arrived on the battlefield\n"); fclose(franci); exit(EXIT_FAILURE); }

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

static void msleep(int millisec)
{
    struct timespec tt;
    tt.tv_sec = millisec / 1000;
    tt.tv_nsec = (millisec % 1000) * 1000000;
    while (nanosleep(&tt, &tt) == -1)
        if (errno != EINTR)
            ERR("nanosleep");
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

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int rand_range(int a, int b)
{
    return a + rand() % (b - a + 1);
}

static void print_intro(const knight* k, int is_frank)
{
    printf("I am %s knight %s. I will serve my king with my %d HP and %d attack.\n",
           is_frank ? "Frankish" : "Spanish", k->name, k->hp, k->attack);
    fflush(stdout);
}

static void print_hit(const char* name, unsigned char s)
{
    if (s == 0)
        printf("%s attacks his enemy, however he deflected\n", name);
    else if (s <= 5)
        printf("%s goes to strike, he hit right and well\n", name);
    else
        printf("%s strikes powerful blow, the shield he breaks and inflicts a big wound\n", name);
    fflush(stdout);
}

static void knight_work(const knight* me, int is_frank, int my_read_fd, enemy_fd* enemies, int enemy_count)
{
    int hp = me->hp;
    unsigned char buf[256];

    srand((unsigned int)getpid());
    if (set_nonblock(my_read_fd) < 0)
        ERR("fcntl");

    print_intro(me, is_frank);

    for (;;)
    {
        for (;;)
        {
            ssize_t r = read(my_read_fd, buf, sizeof(buf));
            if (r > 0)
            {
                for (ssize_t i = 0; i < r; i++)
                    hp -= (int)buf[i];
            }
            else if (r == 0)
                break;
            else
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                ERR("read");
            }
        }

        if (hp < 0)
        {
            printf("%s dies glorious death\n", me->name);
            fflush(stdout);
            break;
        }

        int chosen = rand_range(0, enemy_count - 1);
        unsigned char s = (unsigned char)rand_range(0, me->attack);

        if (write(enemies[chosen].fd, &s, 1) != 1)
            ERR("write");

        print_hit(me->name, s);
        msleep(rand_range(1, 10));
    }

    close_fd(&my_read_fd);
    for (int i = 0; i < enemy_count; i++)
        close_fd(&enemies[i].fd);
    free(enemies);
    _exit(EXIT_SUCCESS);
}

int main(void)
{
    knight *fr = NULL, *sa = NULL;
    int nf = 0, ns = 0, total;
    int (*pipes)[2];
    stage1(&fr, &sa, &nf, &ns);
    total = nf + ns;

    pipes = calloc((size_t)total, sizeof(int[2]));
    if (!pipes) ERR("calloc");
    for (int i = 0; i < total; i++)
        if (pipe(pipes[i]) < 0)
            ERR("pipe");

    for (int i = 0; i < total; i++)
    {
        pid_t pid = fork();
        if (pid < 0) ERR("fork");

        if (pid == 0)
        {
            int is_frank = (i < nf);
            knight* me = is_frank ? &fr[i] : &sa[i - nf];
            int enemy_count = is_frank ? ns : nf;
            enemy_fd* enemies = calloc((size_t)enemy_count, sizeof(enemy_fd));
            if (!enemies) ERR("calloc");

            int e = 0;
            if (is_frank)
                for (int j = nf; j < total; j++) { enemies[e].fd = pipes[j][1]; enemies[e].idx = j; e++; }
            else
                for (int j = 0; j < nf; j++) { enemies[e].fd = pipes[j][1]; enemies[e].idx = j; e++; }

            for (int j = 0; j < total; j++)
            {
                if (j != i) close_fd(&pipes[j][0]);

                int keep = 0;
                for (int k = 0; k < enemy_count; k++)
                    if (enemies[k].idx == j) keep = 1;
                if (!keep) close_fd(&pipes[j][1]);
            }

            knight_work(me, is_frank, pipes[i][0], enemies, enemy_count);
        }
    }

    for (int i = 0; i < total; i++)
    {
        close_fd(&pipes[i][0]);
        close_fd(&pipes[i][1]);
    }

    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD) ERR("wait");

    free(pipes);
    free(fr);
    free(sa);
    return 0;
}