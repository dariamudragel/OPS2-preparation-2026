#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

#define MSG_SIZE 64

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
        if (TEMP_FAILURE_RETRY(close(*fd)) < 0)
            ERR("close");
        *fd = -1;
    }
}

static void keep_only_two_fds(const char *role, int keep1, int keep2, int fds[], int count)
{
    fprintf(stderr, "[%s %d] using read fd %d and write fd %d\n",
            role, getpid(), keep1, keep2);

    for (int i = 0; i < count; i++)
    {
        if (fds[i] != keep1 && fds[i] != keep2 && fds[i] >= 0)
            close_fd(&fds[i], role);
    }
}

static ssize_t write_all(int fd, const void *buf, size_t count)
{
    size_t done = 0;
    const char *p = buf;

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

static ssize_t write_cstring(int fd, const char *msg)
{
    return write_all(fd, msg, strlen(msg) + 1);
}

static ssize_t read_cstring(int fd, char *buf, size_t size)
{
    size_t pos = 0;

    for (;;)
    {
        char c;
        ssize_t ret = read(fd, &c, 1);

        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (ret == 0)
        {
            if (pos == 0)
                return 0; /* EOF before new message */
            errno = EPROTO; /* broken message */
            return -1;
        }

        if (pos + 1 >= size)
        {
            errno = EMSGSIZE;
            return -1;
        }

        buf[pos++] = c;
        if (c == '\0')
            return (ssize_t)pos;
    }
}

static void ring_work(const char *role, int read_fd, int write_fd, int is_parent)
{
    char msg[MSG_SIZE];
    char out[MSG_SIZE];

    srand((unsigned int)getpid());

    if (is_parent)
    {
        strcpy(out, "1");
        fprintf(stdout, "[PARENT %d] sending initial value: %s\n", getpid(), out);
        fflush(stdout);

        if (write_cstring(write_fd, out) < 0)
        {
            if (errno == EPIPE)
            {
                fprintf(stdout, "[PARENT %d] broken pipe on initial send\n", getpid());
                fflush(stdout);
                return;
            }
            ERR("write initial");
        }
    }

    for (;;)
    {
        ssize_t r = read_cstring(read_fd, msg, sizeof(msg));
        if (r < 0)
            ERR("read_cstring");

        if (r == 0)
        {
            fprintf(stdout, "[%s %d] EOF on read pipe, terminating\n", role, getpid());
            fflush(stdout);
            return;
        }

        char *endptr = NULL;
        long value = strtol(msg, &endptr, 10);
        if (*msg == '\0' || *endptr != '\0')
        {
            fprintf(stderr, "[%s %d] invalid message: '%s'\n", role, getpid(), msg);
            return;
        }

        fprintf(stdout, "[%s %d] received: %ld\n", role, getpid(), value);
        fflush(stdout);

        if (value == 0)
        {
            fprintf(stdout, "[%s %d] received 0 -> terminating\n", role, getpid());
            fflush(stdout);
            return;
        }

        long delta = (rand() % 21) - 10;   /* [-10, 10] */
        long next = value + delta;

        snprintf(out, sizeof(out), "%ld", next);

        fprintf(stdout, "[%s %d] delta=%ld, sending: %s\n",
                role, getpid(), delta, out);
        fflush(stdout);

        if (write_cstring(write_fd, out) < 0)
        {
            if (errno == EPIPE)
            {
                fprintf(stdout, "[%s %d] broken pipe on write, terminating\n",
                        role, getpid());
                fflush(stdout);
                return;
            }
            ERR("write_cstring");
        }
    }
}

int main(void)
{
    int p12[2], p23[2], p31[2];
    pid_t c1, c2;

    set_handler(SIG_IGN, SIGPIPE);

    if (pipe(p12) < 0)
        ERR("pipe p12");
    if (pipe(p23) < 0)
        ERR("pipe p23");
    if (pipe(p31) < 0)
        ERR("pipe p31");

    int all_fds[6] = {p12[0], p12[1], p23[0], p23[1], p31[0], p31[1]};

    c1 = fork();
    if (c1 < 0)
        ERR("fork c1");

    if (c1 == 0)
    {
        keep_only_two_fds("CHILD1", p12[0], p23[1], all_fds, 6);
        ring_work("CHILD1", p12[0], p23[1], 0);
        close_fd(&p12[0], "CHILD1");
        close_fd(&p23[1], "CHILD1");
        _exit(EXIT_SUCCESS);
    }

    c2 = fork();
    if (c2 < 0)
        ERR("fork c2");

    if (c2 == 0)
    {
        keep_only_two_fds("CHILD2", p23[0], p31[1], all_fds, 6);
        ring_work("CHILD2", p23[0], p31[1], 0);
        close_fd(&p23[0], "CHILD2");
        close_fd(&p31[1], "CHILD2");
        _exit(EXIT_SUCCESS);
    }

    keep_only_two_fds("PARENT", p31[0], p12[1], all_fds, 6);
    ring_work("PARENT", p31[0], p12[1], 1);

    close_fd(&p31[0], "PARENT");
    close_fd(&p12[1], "PARENT");

    while (TEMP_FAILURE_RETRY(wait(NULL)) > 0)
        ;

    if (errno != ECHILD)
        ERR("wait");

    return EXIT_SUCCESS;
}

/* gcc -std=gnu99 -Wall -Wextra -pedantic ring.c -o ring
./ring */