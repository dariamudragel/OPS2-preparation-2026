#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
REFERENCES USED IN THIS CODE:

1. Starter file: sop-roncevaux.c
   - knight struct
   - count_descriptors()
   - stage1 file loading pattern
   - msleep()

2. prog22a.c
   - many child processes
   - many pipes
   - closing unused descriptors after fork

3. prog22b.c
   - SIGPIPE ignored
   - EPIPE handling instead of fatal termination
   - robust signal / error-aware style

4. prog21_c.c / prog21b_s.c
   - sending fixed-size / byte-size data through pipes
   - treating one byte as one message / blow

5. Earlier ring-style solutions
   - helper layout
   - per-process descriptor filtering
   - enemy list compression by swap-with-last
*/

#define MAX_KNIGHT_NAME_LENGHT 20

#define ERR(source)                                                                                 \
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

static volatile sig_atomic_t last_signal = 0;

int set_handler(void (*f)(int), int sig)
{
    struct sigaction act = {0};
    act.sa_handler = f;
    if (sigaction(sig, &act, NULL) == -1)
        return -1;
    return 0;
}

void sig_handler(int sig) { last_signal = sig; }

void msleep(int millisec)
{
    struct timespec tt;
    tt.tv_sec = millisec / 1000;
    tt.tv_nsec = (millisec % 1000) * 1000000;
    while (nanosleep(&tt, &tt) == -1)
    {
        if (errno != EINTR)
            ERR("nanosleep");
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
    if (getcwd(path, PATH_MAX) == NULL)
        ERR("getcwd");
    if (chdir("/proc/self/fd"))
        ERR("chdir");
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
    return count - 1;
}

static void close_fd(int* fd, const char* role)
{
    if (*fd >= 0)
    {
        if (TEMP_FAILURE_RETRY(close(*fd)) < 0)
            ERR("close");
        *fd = -1;
    }
}

static int set_nonblock_flag(int desc, int value)
{
    int oldflags = fcntl(desc, F_GETFL, 0);
    if (oldflags == -1)
        return -1;
    if (value != 0)
        oldflags |= O_NONBLOCK;
    else
        oldflags &= ~O_NONBLOCK;
    return fcntl(desc, F_SETFL, oldflags);
}

/* ---------- stage 1 ---------- */
/* Based on starter file sop-roncevaux.c */
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
    }

    fclose(franci);
    fclose(saraceni);
    return 0;
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

static int rand_range(int a, int b)
{
    return a + rand() % (b - a + 1);
}

static void remove_enemy(enemy_fd* enemies, int* last, int idx)
{
    close_fd(&enemies[idx].fd, "KNIGHT");
    enemies[idx] = enemies[*last];
    (*last)--;
}

/* ---------- stage 2 / 3 / 4 child work ---------- */
/*
Each knight keeps:
- read end of its own pipe
- write ends to all enemy knights
All other descriptors are closed.
*/
static void knight_work(const knight* me, int is_frank, int my_read_fd, enemy_fd* enemies, int enemy_count)
{
    int hp = me->hp;
    int last = enemy_count - 1;
    unsigned char buf[256];

    srand((unsigned int)getpid());

    if (set_nonblock_flag(my_read_fd, 1) < 0)
        ERR("fcntl O_NONBLOCK");
    if (set_handler(SIG_IGN, SIGPIPE))
        ERR("set SIGPIPE");

    print_knight_intro(me, is_frank);

    /* Uncomment for descriptor debugging in stage 2 */
    /* printf("%s (%d) descriptors: %d\n", me->name, getpid(), count_descriptors()); */

    for (;;)
    {
        ssize_t r;

        /* Stage 3: read all currently available blows */
        for (;;)
        {
            r = read(my_read_fd, buf, sizeof(buf));
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

        /* Stage 4: death */
        if (hp < 0)
        {
            printf("%s dies glorious death\n", me->name);
            fflush(stdout);
            break;
        }

        /* Stage 4: no more living enemies */
        if (last < 0)
            break;

        /* Choose a living enemy and try to hit him.
           If he is already dead, detect EPIPE and compress enemy array. */
        for (;;)
        {
            if (last < 0)
                goto knight_cleanup;

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

        int t = rand_range(1, 10);
        msleep(t);
    }

knight_cleanup:
    close_fd(&my_read_fd, "KNIGHT");
    for (int i = 0; i <= last; i++)
        close_fd(&enemies[i].fd, "KNIGHT");
    free(enemies);
    _exit(EXIT_SUCCESS);
}

/* ---------- stage 2 ---------- */
/*
For each knight:
- one unnamed pipe
- one child process
- child gets its own read end and all write ends to enemies
Structure based on prog22a.c / prog22b.c.
*/
static void stage2_3_4(knight* fr, knight* sa, int nf, int ns)
{
    int total = nf + ns;
    int(*pipes)[2] = calloc((size_t)total, sizeof(int[2]));
    pid_t* pids = calloc((size_t)total, sizeof(pid_t));
    if (!pipes || !pids)
        ERR("calloc");

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
            if (!enemies)
                ERR("calloc enemies");

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

            /* Close all read ends except my own, and all write ends except enemies'. */
            for (int j = 0; j < total; j++)
            {
                if (j != i)
                    close_fd(&pipes[j][0], "KNIGHT");
            }

            for (int j = 0; j < total; j++)
            {
                int keep = 0;
                for (int k = 0; k < enemy_count; k++)
                {
                    if (enemies[k].idx == j)
                    {
                        keep = 1;
                        break;
                    }
                }
                if (!keep)
                    close_fd(&pipes[j][1], "KNIGHT");
            }

            knight_work(me, is_frank, pipes[i][0], enemies, enemy_count);
        }

        pids[i] = pid;
    }

    /* Parent closes everything and waits */
    for (int i = 0; i < total; i++)
    {
        close_fd(&pipes[i][0], "PARENT");
        close_fd(&pipes[i][1], "PARENT");
    }

    while (TEMP_FAILURE_RETRY(wait(NULL)) > 0)
        ;

    if (errno != ECHILD)
        ERR("wait");

    free(pipes);
    free(pids);
}

int main(void)
{
    srand((unsigned int)time(NULL));

    knight* fr = NULL;
    knight* sa = NULL;
    int nf = 0;
    int ns = 0;

    stage1(&fr, &sa, &nf, &ns);
    stage2_3_4(fr, sa, nf, ns);

    free(fr);
    free(sa);

    printf("Opened descriptors: %d\n", count_descriptors());
    return 0;
}