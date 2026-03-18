#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
REFERENCES USED IN THIS CODE:

1. prog22a.c
   - creating many child processes
   - many dedicated pipes
   - closing unused descriptors after fork

2. prog22b.c
   - ignoring SIGPIPE
   - handling disappearing processes / broken pipes
   - style of robust pipe communication

3. prog21_c.c / prog21b_s.c
   - fixed-size binary messages through pipes

4. Earlier ring / game solutions
   - helper functions: close_fd, read_all, write_all
   - stage-based structure
*/

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

#define MIN_MONEY 100

enum
{
    MSG_START_ROUND = 1,
    MSG_BET = 2,
    MSG_RESULT = 3
};

/* Fixed-size 16-byte message */
typedef struct
{
    int type;    /* message type */
    int pid;     /* process id */
    int a;       /* amount / result */
    int b;       /* number / extra */
} Message;

typedef struct
{
    pid_t pid;
    int to_player[2];    /* dealer writes [1], player reads [0] */
    int from_player[2];  /* player writes [1], dealer reads [0] */
    int alive;
    int balance;
    int last_bet;
    int last_number;
} PlayerInfo;

/* ---------- common helpers ---------- */

static void set_handler(void (*handler)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (sigaction(sig, &act, NULL) < 0)
        ERR("sigaction");
}

static void close_fd(int *fd, const char *role)
{
    if (*fd >= 0)
    {
        fprintf(stderr, "[%s %d] closing fd %d\n", role, getpid(), *fd);
        if (close(*fd) < 0)
            ERR("close");
        *fd = -1;
    }
}

static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t done = 0;
    const char *p = (const char *)buf;

    while (done < count)
    {
        ssize_t ret = write(fd, p + done, count - done);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)ret;
    }
    return (ssize_t)done;
}

static ssize_t read_all(int fd, void *buf, size_t count)
{
    size_t done = 0;
    char *p = (char *)buf;

    while (done < count)
    {
        ssize_t ret = read(fd, p + done, count - done);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0)
            return (done == 0) ? 0 : -1;
        done += (size_t)ret;
    }
    return (ssize_t)done;
}

static int send_message(int fd, const Message *msg)
{
    return (write_all(fd, msg, sizeof(Message)) == (ssize_t)sizeof(Message)) ? 0 : -1;
}

static int recv_message(int fd, Message *msg)
{
    ssize_t ret = read_all(fd, msg, sizeof(Message));
    if (ret == 0)
        return 0;   /* EOF */
    if (ret < 0)
        return -1;  /* error */
    return 1;
}

static int rand_range(int a, int b)
{
    return a + rand() % (b - a + 1);
}

static int active_players(PlayerInfo *players, int n)
{
    int cnt = 0;
    for (int i = 0; i < n; i++)
        if (players[i].alive)
            cnt++;
    return cnt;
}

static void mark_player_dead(PlayerInfo *p, int index, const char *why)
{
    if (!p->alive)
        return;

    fprintf(stdout, "Dealer: player %d disconnected (%s)\n", index, why);
    fflush(stdout);

    p->alive = 0;
    close_fd(&p->to_player[1], "DEALER");
    close_fd(&p->from_player[0], "DEALER");
}

/* ---------- player process ---------- */

/*
Based mainly on prog22a.c / prog22b.c style:
child keeps only its needed descriptors and communicates with parent.
*/
static void player_work(int read_fd, int write_fd, int start_money)
{
    Message msg;
    int balance = start_money;
    pid_t mypid = getpid();

    srand((unsigned int)mypid);

    printf("%d: I have %d and I'm going to play roulette.\n", (int)mypid, balance);
    fflush(stdout);

    for (;;)
    {
        int rr = recv_message(read_fd, &msg);
        if (rr == 0)
            break;
        if (rr < 0)
            ERR("player read");

        if (msg.type != MSG_START_ROUND)
            continue;

        if ((rand() % 100) < 10)
        {
            printf("%d: I saved %d and exit.\n", (int)mypid, balance);
            fflush(stdout);
            break;
        }

        if (balance <= 0)
        {
            printf("%d: I'm broke and exit.\n", (int)mypid);
            fflush(stdout);
            break;
        }

        int bet = rand_range(1, balance);
        int number = rand_range(0, 36);

        Message bet_msg;
        bet_msg.type = MSG_BET;
        bet_msg.pid = (int)mypid;
        bet_msg.a = bet;
        bet_msg.b = number;

        if (send_message(write_fd, &bet_msg) < 0)
        {
            if (errno == EPIPE)
                break;
            ERR("player write bet");
        }

        rr = recv_message(read_fd, &msg);
        if (rr == 0)
            break;
        if (rr < 0)
            ERR("player read result");

        if (msg.type == MSG_RESULT)
        {
            int won = msg.a;      /* profit */
            int played_bet = msg.b;

            balance -= played_bet;
            if (won > 0)
            {
                balance += played_bet + won;
                printf("%d: I won %d.\n", (int)mypid, won);
                fflush(stdout);
            }

            if (balance <= 0)
            {
                printf("%d: I'm broke and exit.\n", (int)mypid);
                fflush(stdout);
                break;
            }
        }
    }

    close_fd(&read_fd, "PLAYER");
    close_fd(&write_fd, "PLAYER");
    _exit(EXIT_SUCCESS);
}

/* ---------- stages ---------- */

/*
Stage 1:
Dealer creates N players.
Each player prints:
[pid]: I have [amount] and I'm going to play roulette.

Modeled after prog22a.c / prog22b.c:
fork children, create per-player pipes, close unused ends.
*/
static void stage1_create_players(PlayerInfo *players, int n, int money)
{
    for (int i = 0; i < n; i++)
    {
        players[i].to_player[0] = players[i].to_player[1] = -1;
        players[i].from_player[0] = players[i].from_player[1] = -1;
        players[i].alive = 1;
        players[i].balance = money;
        players[i].last_bet = 0;
        players[i].last_number = -1;

        if (pipe(players[i].to_player) < 0)
            ERR("pipe to_player");
        if (pipe(players[i].from_player) < 0)
            ERR("pipe from_player");

        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            for (int j = 0; j <= i; j++)
            {
                if (j != i)
                {
                    close_fd(&players[j].to_player[0], "PLAYER");
                    close_fd(&players[j].to_player[1], "PLAYER");
                    close_fd(&players[j].from_player[0], "PLAYER");
                    close_fd(&players[j].from_player[1], "PLAYER");
                }
            }

            close_fd(&players[i].to_player[1], "PLAYER");
            close_fd(&players[i].from_player[0], "PLAYER");

            player_work(players[i].to_player[0], players[i].from_player[1], money);
        }

        players[i].pid = pid;
        close_fd(&players[i].to_player[0], "DEALER");
        close_fd(&players[i].from_player[1], "DEALER");
    }
}

/*
Stage 2:
Players send bet amount and chosen number.
Dealer prints:
Dealer: [pid] placed [amount] on [number]
*/
static int stage2_collect_bets(PlayerInfo *players, int n, int participants[])
{
    int count = 0;
    Message msg;

    for (int i = 0; i < n; i++)
    {
        if (!players[i].alive)
            continue;

        Message start;
        start.type = MSG_START_ROUND;
        start.pid = 0;
        start.a = 0;
        start.b = 0;

        if (send_message(players[i].to_player[1], &start) < 0)
        {
            mark_player_dead(&players[i], i, "could not start round");
            continue;
        }
    }

    for (int i = 0; i < n; i++)
    {
        if (!players[i].alive)
            continue;

        int rr = recv_message(players[i].from_player[0], &msg);
        if (rr == 0)
        {
            mark_player_dead(&players[i], i, "player left");
            continue;
        }
        if (rr < 0)
        {
            mark_player_dead(&players[i], i, "bet read failed");
            continue;
        }

        if (msg.type != MSG_BET)
        {
            mark_player_dead(&players[i], i, "invalid message");
            continue;
        }

        if (msg.a < 1 || msg.a > players[i].balance || msg.b < 0 || msg.b > 36)
        {
            mark_player_dead(&players[i], i, "invalid bet");
            continue;
        }

        players[i].last_bet = msg.a;
        players[i].last_number = msg.b;
        participants[count++] = i;

        printf("Dealer: %d placed %d on %d\n",
               (int)players[i].pid, players[i].last_bet, players[i].last_number);
        fflush(stdout);
    }

    return count;
}

/*
Stage 3:
Dealer draws lucky number, announces it,
sends round result to all participants.
After one round everybody could exit, but in final version we keep going.
*/
static void stage3_resolve_round(PlayerInfo *players, int participants[], int count)
{
    int lucky = rand_range(0, 36);

    printf("Dealer: %d is the lucky number.\n", lucky);
    fflush(stdout);

    for (int k = 0; k < count; k++)
    {
        int idx = participants[k];
        int profit = 0;

        if (!players[idx].alive)
            continue;

        if (players[idx].last_number == lucky)
            profit = 35 * players[idx].last_bet;

        Message result;
        result.type = MSG_RESULT;
        result.pid = 0;
        result.a = profit;                /* profit only */
        result.b = players[idx].last_bet; /* original bet */

        if (send_message(players[idx].to_player[1], &result) < 0)
        {
            mark_player_dead(&players[idx], idx, "result send failed");
            continue;
        }

        players[idx].balance -= players[idx].last_bet;
        if (profit > 0)
            players[idx].balance += players[idx].last_bet + profit;
    }
}

/*
Stage 4 + 5:
The game continues while at least one player has money and stays in game.
If player is broke, they print and exit on their side.
If player leaves with 10% chance, they print and exit on their side.
Dealer ends when all players are gone.
*/
static void stage4_play_game(PlayerInfo *players, int n)
{
    while (active_players(players, n) > 0)
    {
        int participants[n];
        int count = stage2_collect_bets(players, n, participants);

        if (count == 0)
            continue;

        stage3_resolve_round(players, participants, count);

        for (int i = 0; i < n; i++)
        {
            if (players[i].alive && players[i].balance <= 0)
            {
                close_fd(&players[i].to_player[1], "DEALER");
                close_fd(&players[i].from_player[0], "DEALER");
                players[i].alive = 0;
            }
        }
    }
}

static void cleanup_dealer(PlayerInfo *players, int n)
{
    for (int i = 0; i < n; i++)
    {
        close_fd(&players[i].to_player[1], "DEALER");
        close_fd(&players[i].from_player[0], "DEALER");
    }

    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");
}

static void usage(char *name)
{
    fprintf(stderr, "USAGE: %s N M\n", name);
    fprintf(stderr, "N >= 1\n");
    fprintf(stderr, "M >= %d\n", MIN_MONEY);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int n, money;
    PlayerInfo *players = NULL;

    if (argc != 3)
        usage(argv[0]);

    n = atoi(argv[1]);
    money = atoi(argv[2]);

    if (n < 1 || money < MIN_MONEY)
        usage(argv[0]);

    srand((unsigned int)getpid());

    /* Like prog22b.c: ignore SIGPIPE so broken pipes do not kill dealer */
    set_handler(SIG_IGN, SIGPIPE);

    players = (PlayerInfo *)calloc((size_t)n, sizeof(PlayerInfo));
    if (players == NULL)
        ERR("calloc");

    /* Stage 1 */
    stage1_create_players(players, n, money);

    /* Stage 2 + 3 + 4 + 5 */
    stage4_play_game(players, n);

    cleanup_dealer(players, n);
    free(players);

    printf("Dealer: Casino always wins\n");
    fflush(stdout);

    return EXIT_SUCCESS;
}