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

1. Style and structure inspired by:
   - prog22a.c
   - prog22b.c
   These are the tutorial examples about many children + many pipes.

2. Specific ideas reused:
   - SIGCHLD handler with waitpid(..., WNOHANG)     -> like prog22a.c / prog22b.c
   - SIGPIPE ignored and EPIPE checked explicitly   -> like prog22b.c
   - TEMP_FAILURE_RETRY for interrupted calls       -> like prog22b.c
   - explicit closing of all unused descriptors     -> like prog22a.c / prog22b.c

3. Additional structure reused from the previously provided ring code:
   - send_msg / recv_msg helpers
   - close_fd helper
   - stage-separated functions
*/

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

#define MSG_SIZE 16
#define MIN_N 2
#define MAX_N 5
#define MIN_M 5
#define MAX_M 10

typedef struct
{
    pid_t pid;
    int to_child[2];     /* server writes [1], child reads [0] */
    int from_child[2];   /* child writes [1], server reads [0] */
    int alive;
    int score;
    int last_card;
} Player;

/* ---------- common helpers ---------- */

static void set_handler(void (*handler)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (sigaction(sig, &act, NULL) < 0)
        ERR("sigaction");
}

/* Based on prog22a.c / prog22b.c */
static void sigchld_handler(int sig)
{
    pid_t pid;
    (void)sig;

    for (;;)
    {
        pid = waitpid(0, NULL, WNOHANG);
        if (pid == 0)
            return;
        if (pid < 0)
        {
            if (errno == ECHILD)
                return;
            ERR("waitpid");
        }
    }
}

static void close_fd(int *fd, const char *role)
{
    if (*fd >= 0)
    {
        fprintf(stderr, "[%s %d] closing fd %d\n", role, getpid(), *fd);
        if (TEMP_FAILURE_RETRY(close(*fd)) < 0)
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
            return (done == 0) ? 0 : -2;
        done += (size_t)ret;
    }
    return (ssize_t)done;
}

/* Fixed 16-byte messages, as required in the task */
static int send_msg(int fd, const char *text)
{
    char msg[MSG_SIZE];
    memset(msg, 0, sizeof(msg));
    strncpy(msg, text, MSG_SIZE - 1);

    if (write_all(fd, msg, sizeof(msg)) != sizeof(msg))
        return -1;
    return 0;
}

static int recv_msg(int fd, char *out)
{
    char msg[MSG_SIZE];
    ssize_t ret = read_all(fd, msg, sizeof(msg));

    if (ret == 0)
        return 0; /* EOF */
    if (ret < 0)
        return -1;

    memcpy(out, msg, MSG_SIZE);
    out[MSG_SIZE - 1] = '\0';
    return 1;
}

static int active_players(Player *players, int N)
{
    int cnt = 0;
    for (int i = 0; i < N; i++)
        if (players[i].alive)
            cnt++;
    return cnt;
}

static void mark_player_dead(Player *p, int index, const char *why)
{
    if (!p->alive)
        return;

    fprintf(stdout, "[SERVER %d] player %d disconnected (%s)\n", getpid(), index, why);
    fflush(stdout);

    p->alive = 0;
    close_fd(&p->to_child[1], "SERVER");
    close_fd(&p->from_child[0], "SERVER");
}

static int choose_random_card(int *cards, int *count)
{
    int idx = rand() % (*count);
    int chosen = cards[idx];
    cards[idx] = cards[*count - 1];
    (*count)--;
    return chosen;
}

/* ---------- player code ---------- */

/*
Stage 2/3/4/5 player work.
Structure similar to tutorial children in prog22a.c / prog22b.c:
- child keeps only needed descriptors
- waits for server messages
- reacts and writes back through its dedicated pipe
*/
static void player_work(int read_fd, int write_fd, int M, int index)
{
    char msg[MSG_SIZE];
    int *cards = NULL;
    int cards_left = M;

    srand((unsigned int)getpid());

    cards = (int *)malloc(sizeof(int) * M);
    if (cards == NULL)
        ERR("malloc");

    for (int i = 0; i < M; i++)
        cards[i] = i + 1;

    for (;;)
    {
        int rr = recv_msg(read_fd, msg);
        if (rr <= 0)
            break;

        if (strcmp(msg, "new_round") == 0)
        {
            /* Stage 5: 5% chance of failure */
            if ((rand() % 100) < 5)
            {
                fprintf(stdout, "[PLAYER %d | pid=%d] FAILURE\n", index, getpid());
                fflush(stdout);
                break;
            }

            if (cards_left <= 0)
                break;

            int card = choose_random_card(cards, &cards_left);
            char out[MSG_SIZE];
            snprintf(out, sizeof(out), "%d", card);

            if (send_msg(write_fd, out) < 0)
                break;

            rr = recv_msg(read_fd, msg);
            if (rr <= 0)
                break;

            if (strcmp(msg, "game_over") == 0)
                break;

            fprintf(stdout, "[PLAYER %d | pid=%d] got %s points\n", index, getpid(), msg);
            fflush(stdout);
        }
        else if (strcmp(msg, "game_over") == 0)
        {
            break;
        }
    }

    free(cards);
    close_fd(&read_fd, "PLAYER");
    close_fd(&write_fd, "PLAYER");
    _exit(EXIT_SUCCESS);
}

/* ---------- stages ---------- */

/*
Stage 1:
Correct initialization, creation of player processes and required pipes.
This is the part most directly modeled after prog22a.c / prog22b.c:
create pipes, fork children, close unused ends on both sides.
*/
static void stage1_create_players(Player *players, int N, int M)
{
    for (int i = 0; i < N; i++)
    {
        players[i].to_child[0] = players[i].to_child[1] = -1;
        players[i].from_child[0] = players[i].from_child[1] = -1;
        players[i].alive = 1;
        players[i].score = 0;
        players[i].last_card = -1;

        if (pipe(players[i].to_child) < 0)
            ERR("pipe to_child");
        if (pipe(players[i].from_child) < 0)
            ERR("pipe from_child");

        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            /* Child closes all descriptors that are not its own.
               This follows the same rule as prog22a.c / prog22b.c:
               close everything not used immediately after fork. */
            for (int j = 0; j <= i; j++)
            {
                if (j != i)
                {
                    close_fd(&players[j].to_child[0], "PLAYER");
                    close_fd(&players[j].to_child[1], "PLAYER");
                    close_fd(&players[j].from_child[0], "PLAYER");
                    close_fd(&players[j].from_child[1], "PLAYER");
                }
            }

            close_fd(&players[i].to_child[1], "PLAYER");
            close_fd(&players[i].from_child[0], "PLAYER");

            player_work(players[i].to_child[0], players[i].from_child[1], M, i);
        }

        players[i].pid = pid;

        close_fd(&players[i].to_child[0], "SERVER");
        close_fd(&players[i].from_child[1], "SERVER");
    }
}

/*
Stage 2:
Every player sends a random nonnegative integer <= M to server,
server prints: Got number <X> from player <index>

This stage is mostly for checking communication works.
Not used in final game loop, but included because you asked for stage split.
*/
static void stage2_test_random_numbers(Player *players, int N, int M)
{
    char msg[MSG_SIZE];

    fprintf(stdout, "\n--- STAGE 2 TEST ---\n");
    fflush(stdout);

    for (int i = 0; i < N; i++)
    {
        if (!players[i].alive)
            continue;

        if (send_msg(players[i].to_child[1], "new_round") < 0)
        {
            mark_player_dead(&players[i], i, "send failed in stage2");
            continue;
        }
    }

    for (int i = 0; i < N; i++)
    {
        if (!players[i].alive)
            continue;

        int rr = recv_msg(players[i].from_child[0], msg);
        if (rr <= 0)
        {
            mark_player_dead(&players[i], i, "recv failed in stage2");
            continue;
        }

        char *endptr = NULL;
        long value = strtol(msg, &endptr, 10);
        if (*msg == '\0' || *endptr != '\0' || value < 0 || value > M)
        {
            mark_player_dead(&players[i], i, "invalid number in stage2");
            continue;
        }

        fprintf(stdout, "Got number %ld from player %d\n", value, i);
        fflush(stdout);

        if (send_msg(players[i].to_child[1], "0") < 0)
            mark_player_dead(&players[i], i, "send points failed in stage2");
    }
}

/*
Stage 3:
Implementation of rounds. Server cyclically sends "new_round".
Players respond with a random card.
This function performs one logical round and stores received cards.
*/
static int stage3_round_collect_cards(Player *players, int N, int M, int round,
                                      int participants[], int cards[])
{
    char msg[MSG_SIZE];
    int participant_count = 0;

    if (active_players(players, N) == 0)
        return 0;

    fprintf(stdout, "\nNEW ROUND %d\n", round);
    fflush(stdout);

    for (int i = 0; i < N; i++)
    {
        if (!players[i].alive)
            continue;

        if (send_msg(players[i].to_child[1], "new_round") < 0)
        {
            mark_player_dead(&players[i], i, "send new_round failed");
            continue;
        }
    }

    for (int i = 0; i < N; i++)
    {
        if (!players[i].alive)
            continue;

        int rr = recv_msg(players[i].from_child[0], msg);
        if (rr == 0)
        {
            mark_player_dead(&players[i], i, "EOF on card read");
            continue;
        }
        if (rr < 0)
        {
            mark_player_dead(&players[i], i, "error on card read");
            continue;
        }

        char *endptr = NULL;
        long card = strtol(msg, &endptr, 10);
        if (*msg == '\0' || *endptr != '\0' || card < 1 || card > M)
        {
            mark_player_dead(&players[i], i, "invalid card");
            continue;
        }

        players[i].last_card = (int)card;
        participants[participant_count] = i;
        cards[participant_count] = (int)card;
        participant_count++;

        fprintf(stdout, "[SERVER %d] player %d played %d\n",
                getpid(), i, (int)card);
        fflush(stdout);
    }

    return participant_count;
}

/*
Stage 4:
Full implementation of game rules:
- highest card wins
- ties divide N points equally, rounded down
- after M rounds print result table
*/
static void stage4_play_full_game(Player *players, int N, int M)
{
    for (int round = 1; round <= M; round++)
    {
        int participants[N];
        int cards[N];

        int participant_count = stage3_round_collect_cards(players, N, M, round,
                                                           participants, cards);
        if (participant_count == 0)
            continue;

        int max_card = cards[0];
        for (int i = 1; i < participant_count; i++)
            if (cards[i] > max_card)
                max_card = cards[i];

        int winners = 0;
        for (int i = 0; i < participant_count; i++)
            if (cards[i] == max_card)
                winners++;

        int gain = N / winners;

        for (int i = 0; i < participant_count; i++)
        {
            int idx = participants[i];
            int points = (cards[i] == max_card) ? gain : 0;
            char out[MSG_SIZE];

            players[idx].score += points;
            snprintf(out, sizeof(out), "%d", points);

            if (send_msg(players[idx].to_child[1], out) < 0)
                mark_player_dead(&players[idx], idx, "send points failed");
        }
    }
}

/*
Stage 5:
Players may fail with 5% chance after round start.
That logic is already inside player_work().
This function just finalizes and prints results.
*/
static void stage5_finish_game(Player *players, int N)
{
    fprintf(stdout, "\n=== RESULTS ===\n");
    for (int i = 0; i < N; i++)
    {
        fprintf(stdout, "Player %d (pid=%d): %d points%s\n",
                i,
                (int)players[i].pid,
                players[i].score,
                players[i].alive ? "" : " [disconnected]");
    }

    int best = -1;
    for (int i = 0; i < N; i++)
        if (players[i].score > best)
            best = players[i].score;

    fprintf(stdout, "Winner(s):");
    for (int i = 0; i < N; i++)
        if (players[i].score == best)
            fprintf(stdout, " %d", i);
    fprintf(stdout, "\n");
    fflush(stdout);

    for (int i = 0; i < N; i++)
    {
        if (players[i].alive)
        {
            if (send_msg(players[i].to_child[1], "game_over") < 0)
                mark_player_dead(&players[i], i, "send game_over failed");
        }
    }
}

static void cleanup_server(Player *players, int N)
{
    for (int i = 0; i < N; i++)
    {
        close_fd(&players[i].to_child[1], "SERVER");
        close_fd(&players[i].from_child[0], "SERVER");
    }

    while (TEMP_FAILURE_RETRY(wait(NULL)) > 0)
        ;

    if (errno != ECHILD)
        ERR("wait");
}

static void usage(char *name)
{
    fprintf(stderr, "USAGE: %s N M\n", name);
    fprintf(stderr, "%d <= N <= %d\n", MIN_N, MAX_N);
    fprintf(stderr, "%d <= M <= %d\n", MIN_M, MAX_M);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int N, M;
    Player *players = NULL;

    if (argc != 3)
        usage(argv[0]);

    N = atoi(argv[1]);
    M = atoi(argv[2]);

    if (N < MIN_N || N > MAX_N || M < MIN_M || M > MAX_M)
        usage(argv[0]);

    /* Like prog22b.c: ignore SIGPIPE, install SIGCHLD handler */
    set_handler(SIG_IGN, SIGPIPE);
    set_handler(sigchld_handler, SIGCHLD);

    players = (Player *)calloc((size_t)N, sizeof(Player));
    if (players == NULL)
        ERR("calloc");

    /* Stage 1 */
    stage1_create_players(players, N, M);

    /*
    Stage 2 is optional diagnostic stage from task statement.
    Uncomment if you want to test the communication step separately.
    */
    /* stage2_test_random_numbers(players, N, M); */

    /* Stage 3 + 4 + 5 */
    stage4_play_full_game(players, N, M);
    stage5_finish_game(players, N);

    cleanup_server(players, N);
    free(players);
    return EXIT_SUCCESS;
}