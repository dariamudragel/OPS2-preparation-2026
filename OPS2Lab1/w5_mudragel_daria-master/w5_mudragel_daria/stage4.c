#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_KNIGHT_NAME_LENGHT 20
#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))

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
   - file loading
   - msleep style

2. prog22a.c
   - multiple children + multiple pipes
   - close unused descriptors

3. prog22b.c
   - ignore SIGPIPE
   - handle EPIPE when enemy is gone
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
        if (TEMP_FAILURE_RETRY(close(*fd)) < 0)
            ERR("close");
        *fd = -1;
    }
}

static int rand_range(int a, int b)
{
    return a + rand() % (b - a + 1);
}

static int set_nonblock_flag(int desc)
{
    int flags = fcntl(desc, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(desc, F_SETFL, flags | O_NONBLOCK);
}

static void print_knight_intro(const knight* k, int is_frank)
{
    printf("I am %s knight %s. I will serve my king with my %d HP and %d attack.\n",
           is_frank ? "Frankish" : "Spanish",
           k->name,
           k->hp,
           k->attack);
    fflush(stdout);
}

static void print_strike_line(const char* name, unsigned char s)
{
    if (s == 0)
        printf("%s attacks his enemy, however he deflected\n", name);
    else if (s <= 5)
        printf("%s goes to strike, he hit right and well\n", name);
    else
        printf("%s strikes powerful blow, the shield he breaks and inflicts a big wound\n", name);
    fflush(stdout);
}

static void remove_enemy(enemy_fd* enemies, int* last, int idx)
{
    close_fd(&enemies[idx].fd);
    enemies[idx] = enemies[*last];
    (*last)--;
}

static void knight_work(const knight* me, int is_frank, int my_read_fd, enemy_fd* enemies, int enemy_count)
{
    int hp = me->hp;
    int last = enemy_count - 1;
    unsigned char buf[256];

    srand((unsigned int)getpid());

    if (set_nonblock_flag(my_read_fd) < 0)
        ERR("fcntl");
    signal(SIGPIPE, SIG_IGN);

    print_knight_intro(me, is_frank);

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
            {
                break;
            }
            else
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                ERR("read");
            }
        }

        if (hp < 0)
        {
            printf("%s dies glorious death\n", me->name);
            fflush(stdout);
            break;
        }

        if (last < 0)
            break;

        for (;;)
        {
            if (last < 0)
                goto cleanup;

            int chosen = rand_range(0, last);
            unsigned char s = (unsigned char)rand_range(0, me->attack);

            ssize_t w = write(enemies[chosen].fd, &s, 1);
            if (w == 1)
            {
                print_strike_line(me->name, s);
                break;
            }

            if (w < 0 && errno == EINTR)
                continue;

            if (w < 0 && errno == EPIPE)
            {
                remove_enemy(enemies, &last, chosen);
                continue;
            }

            ERR("write");
        }

        msleep(rand_range(1, 10));
    }

cleanup:
    close_fd(&my_read_fd);
    for (int i = 0; i <= last; i++)
        close_fd(&enemies[i].fd);
    free(enemies);
    _exit(EXIT_SUCCESS);
}

int main(void)
{
    knight *fr = NULL, *sa = NULL;
    int nf = 0, ns = 0;
    int total;
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
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            int is_frank = (i < nf);
            knight* me = is_frank ? &fr[i] : &sa[i - nf];
            int enemy_count = is_frank ? ns : nf;
            enemy_fd* enemies = calloc((size_t)enemy_count, sizeof(enemy_fd));
            if (!enemies) ERR("calloc");

            int e = 0;
            if (is_frank)
            {
                for (int j = nf; j < total; j++)
                {
                    enemies[e].fd = pipes[j][1];
                    enemies[e].idx = j;
                    e++;
                }
            }
            else
            {
                for (int j = 0; j < nf; j++)
                {
                    enemies[e].fd = pipes[j][1];
                    enemies[e].idx = j;
                    e++;
                }
            }

            for (int j = 0; j < total; j++)
            {
                if (j != i)
                    close_fd(&pipes[j][0]);

                int keep = 0;
                for (int k = 0; k < enemy_count; k++)
                    if (enemies[k].idx == j)
                        keep = 1;

                if (!keep)
                    close_fd(&pipes[j][1]);
            }

            knight_work(me, is_frank, pipes[i][0], enemies, enemy_count);
        }
    }

    for (int i = 0; i < total; i++)
    {
        close_fd(&pipes[i][0]);
        close_fd(&pipes[i][1]);
    }

    while (TEMP_FAILURE_RETRY(wait(NULL)) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");

    free(pipes);
    free(fr);
    free(sa);
    return 0;
}