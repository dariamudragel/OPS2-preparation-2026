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
   - many pipes
   - closing unused descriptors after fork

2. prog22b.c
   - signal handling style
   - ignoring SIGPIPE
   - shared pipe used to report events to parent
   - robust read/write patterns around broken pipes

3. prog21_c.c / prog21b_s.c
   - fixed-size binary messages through pipes

4. Earlier ring solution style
   - players connected in ring topology
   - each child keeps exactly one read end and one write end
*/

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

#define MIN_N 4
#define MAX_N 7
#define MIN_M 4
#define DECK_SIZE 52

enum
{
    MSG_HAND_CARD = 1,
    MSG_START     = 2,
    MSG_CARD      = 3
};

typedef struct
{
    int type;   /* message type */
    int value;  /* card value */
    int extra;  /* optional */
    int pad;    /* keep fixed-size 16 bytes */
} Message;

typedef struct
{
    pid_t pid;
    int server_to_player[2]; /* server writes [1], player reads [0] */
    int alive;
} PlayerInfo;

volatile sig_atomic_t stop_flag = 0;

/* ---------- common helpers ---------- */

static void set_handler(void (*handler)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (sigaction(sig, &act, NULL) < 0)
        ERR("sigaction");
}

static void sigint_handler(int sig)
{
    (void)sig;
    stop_flag = 1;
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
            {
                if (stop_flag)
                    return -2;
                continue;
            }
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
    if (ret == -2)
        return -2;  /* interrupted due to SIGINT */
    if (ret < 0)
        return -1;  /* error */
    return 1;
}

static void shuffle_deck(int *deck, int n)
{
    for (int i = 0; i < n; i++)
        deck[i] = i;

    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        int tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

static int has_same_suit(int *hand, int count)
{
    if (count <= 0)
        return 0;

    int suit = hand[0] % 4;
    for (int i = 1; i < count; i++)
        if (hand[i] % 4 != suit)
            return 0;
    return 1;
}

static int choose_card_to_pass(int *hand, int count)
{
    return rand() % count;
}

static void remove_card_at(int *hand, int *count, int idx)
{
    hand[idx] = hand[*count - 1];
    (*count)--;
}

/* ---------- player process ---------- */

/*
Based on:
- prog22a.c / prog22b.c for child pipe setup and cleanup
- ring solution style for one read end + one write end per child
- shared winner pipe to notify parent
*/
static void player_work(int read_from_server_fd,
                        int read_from_left_fd,
                        int write_to_right_fd,
                        int winner_write_fd,
                        int M)
{
    int hand[DECK_SIZE];
    int hand_count = 0;
    Message msg;
    pid_t mypid = getpid();

    srand((unsigned int)mypid);

    /* Stage 1: receive hand from server */
    while (hand_count < M)
    {
        int rr = recv_message(read_from_server_fd, &msg);
        if (rr <= 0)
            goto cleanup;

        if (msg.type == MSG_HAND_CARD)
            hand[hand_count++] = msg.value;
    }

    printf("%d:", (int)mypid);
    for (int i = 0; i < hand_count; i++)
        printf(" %d", hand[i]);
    printf("\n");
    fflush(stdout);

    /* Wait for start signal */
    for (;;)
    {
        int rr = recv_message(read_from_server_fd, &msg);
        if (rr <= 0)
            goto cleanup;
        if (msg.type == MSG_START)
            break;
    }

    /* Stages 2 + 3: endless gameplay until someone wins or Ctrl-C */
    for (;;)
    {
        if (stop_flag)
            goto cleanup;

        if (has_same_suit(hand, hand_count))
        {
            if (write_all(winner_write_fd, &mypid, sizeof(mypid)) == (ssize_t)sizeof(mypid))
            {
                printf("%d: My ship sails!\n", (int)mypid);
                fflush(stdout);
            }
            goto cleanup;
        }

        int idx = choose_card_to_pass(hand, hand_count);
        int card_to_pass = hand[idx];
        Message out;
        out.type = MSG_CARD;
        out.value = card_to_pass;
        out.extra = 0;
        out.pad = 0;

        if (send_message(write_to_right_fd, &out) < 0)
        {
            if (errno == EPIPE)
                goto cleanup;
            ERR("player write ring");
        }

        remove_card_at(hand, &hand_count, idx);

        int rr = recv_message(read_from_left_fd, &msg);
        if (rr == 0 || rr == -2)
            goto cleanup;
        if (rr < 0)
            ERR("player read ring");

        if (msg.type == MSG_CARD)
        {
            hand[hand_count++] = msg.value;
        }
    }

cleanup:
    close_fd(&read_from_server_fd, "PLAYER");
    close_fd(&read_from_left_fd, "PLAYER");
    close_fd(&write_to_right_fd, "PLAYER");
    close_fd(&winner_write_fd, "PLAYER");
    _exit(EXIT_SUCCESS);
}

/* ---------- stages ---------- */

/*
Stage 1:
Server creates N players.
Server shuffles deck and deals M cards to each player.
Each player prints hand with PID.

This part is modeled mainly after prog22a.c:
many children, many descriptors, close unused ends carefully.
*/
static void stage1_create_players_and_deal(PlayerInfo *players,
                                           int N,
                                           int M,
                                           int ring_pipes[][2],
                                           int winner_pipe[2])
{
    int deck[DECK_SIZE];
    shuffle_deck(deck, DECK_SIZE);

    for (int i = 0; i < N; i++)
    {
        players[i].server_to_player[0] = players[i].server_to_player[1] = -1;
        players[i].alive = 1;

        if (pipe(players[i].server_to_player) < 0)
            ERR("pipe server_to_player");
    }

    for (int i = 0; i < N; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            /* child: keep own server read end, one ring read end, one ring write end, winner write end */
            for (int j = 0; j < N; j++)
            {
                if (j != i)
                {
                    close_fd(&players[j].server_to_player[0], "PLAYER");
                    close_fd(&players[j].server_to_player[1], "PLAYER");
                }
            }

            /* ring: player i reads from ring_pipes[i][0], writes to ring_pipes[(i+1)%N][1] */
            for (int j = 0; j < N; j++)
            {
                if (j != i)
                    close_fd(&ring_pipes[j][0], "PLAYER");
                if (j != (i + 1) % N)
                    close_fd(&ring_pipes[j][1], "PLAYER");
            }

            close_fd(&players[i].server_to_player[1], "PLAYER");
            close_fd(&winner_pipe[0], "PLAYER");

            player_work(players[i].server_to_player[0],
                        ring_pipes[i][0],
                        ring_pipes[(i + 1) % N][1],
                        winner_pipe[1],
                        M);
        }

        players[i].pid = pid;
        close_fd(&players[i].server_to_player[0], "SERVER");
    }

    /* server does not use ring pipes except for cleanup */
    for (int i = 0; i < N; i++)
    {
        close_fd(&ring_pipes[i][0], "SERVER");
        close_fd(&ring_pipes[i][1], "SERVER");
    }

    close_fd(&winner_pipe[1], "SERVER");

    /* deal cards */
    int pos = 0;
    for (int i = 0; i < N; i++)
    {
        for (int j = 0; j < M; j++)
        {
            Message msg;
            msg.type = MSG_HAND_CARD;
            msg.value = deck[pos++];
            msg.extra = 0;
            msg.pad = 0;

            if (send_message(players[i].server_to_player[1], &msg) < 0)
                ERR("deal card");
        }
    }
}

/*
Stage 2:
Players form a ring and pass cards endlessly.
Server only starts the game by sending MSG_START.
*/
static void stage2_start_game(PlayerInfo *players, int N)
{
    for (int i = 0; i < N; i++)
    {
        Message start;
        start.type = MSG_START;
        start.value = 0;
        start.extra = 0;
        start.pad = 0;

        if (send_message(players[i].server_to_player[1], &start) < 0)
        {
            if (errno == EPIPE)
                continue;
            ERR("send start");
        }
    }
}

/*
Stage 3:
Winner announces via shared pipe.
Server reads PID, prints:
Server: [PID] won!
and exits.
*/
static void stage3_wait_for_winner(int winner_read_fd)
{
    pid_t winner_pid;
    ssize_t ret;

    for (;;)
    {
        ret = read(winner_read_fd, &winner_pid, sizeof(winner_pid));
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                if (stop_flag)
                    return;
                continue;
            }
            ERR("read winner");
        }
        if (ret == 0)
            return;
        if (ret == (ssize_t)sizeof(winner_pid))
        {
            printf("Server: %d won!\n", (int)winner_pid);
            fflush(stdout);
            return;
        }
    }
}

/*
Stage 4:
Ctrl-C stops all processes and cleans resources.
Server handles SIGINT, kills process group, closes resources, waits.
This follows prog22b.c signal-handling spirit.
*/
static void stage4_cleanup(PlayerInfo *players,
                           int N,
                           int winner_pipe[2])
{
    close_fd(&winner_pipe[0], "SERVER");

    for (int i = 0; i < N; i++)
        close_fd(&players[i].server_to_player[1], "SERVER");

    /* kill whole process group on Ctrl-C or after winner */
    kill(0, SIGTERM);

    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");
}

static void usage(char *name)
{
    fprintf(stderr, "USAGE: %s N M\n", name);
    fprintf(stderr, "%d <= N <= %d\n", MIN_N, MAX_N);
    fprintf(stderr, "%d <= M and M*N <= 52\n", MIN_M);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int N, M;
    PlayerInfo *players = NULL;
    int (*ring_pipes)[2] = NULL;
    int winner_pipe[2];

    if (argc != 3)
        usage(argv[0]);

    N = atoi(argv[1]);
    M = atoi(argv[2]);

    if (N < MIN_N || N > MAX_N || M < MIN_M || M * N > DECK_SIZE)
        usage(argv[0]);

    srand((unsigned int)getpid());

    /* Like prog22b.c: ignore SIGPIPE so broken ring writes don't kill process immediately */
    set_handler(SIG_IGN, SIGPIPE);
    set_handler(sigint_handler, SIGINT);

    players = (PlayerInfo *)calloc((size_t)N, sizeof(PlayerInfo));
    if (players == NULL)
        ERR("calloc");

    ring_pipes = malloc((size_t)N * sizeof(*ring_pipes));
    if (ring_pipes == NULL)
        ERR("malloc ring_pipes");

    for (int i = 0; i < N; i++)
    {
        if (pipe(ring_pipes[i]) < 0)
            ERR("pipe ring");
    }

    if (pipe(winner_pipe) < 0)
        ERR("pipe winner");

    /* Stage 1 */
    stage1_create_players_and_deal(players, N, M, ring_pipes, winner_pipe);

    /* Stage 2 */
    stage2_start_game(players, N);

    /* Stage 3 */
    stage3_wait_for_winner(winner_pipe[0]);

    /* Stage 4 */
    stage4_cleanup(players, N, winner_pipe);

    free(ring_pipes);
    free(players);
    return EXIT_SUCCESS;
}