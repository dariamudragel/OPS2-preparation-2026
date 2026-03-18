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

1. prog21_c.c / prog21b_s.c
   - binary/fixed-size messages
   - storing PID directly inside transmitted data

2. prog22a.c / prog22b.c
   - many child processes
   - one shared pipe for children -> parent
   - one dedicated pipe parent -> each child
   - closing unused pipe ends after fork
   - SIGPIPE ignored and broken-pipe handling
   - wait / child cleanup style

3. Earlier ring/game_stages style
   - helper functions: close_fd, read_all, write_all
   - stage-based program organization
*/

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), exit(EXIT_FAILURE))

#define MIN_N 3
#define MAX_N 20
#define MSG_SIZE_BYTES 16
#define LAB_TIME 5 /* seconds, adjustable */

enum
{
    MSG_ATTENDANCE_QUESTION = 1,
    MSG_ATTENDANCE_HERE     = 2,
    MSG_STAGE_RESULT        = 3,
    MSG_STAGE_ATTEMPT       = 4
};

/*
16-byte message, like in the earlier fixed-size message tasks.
This follows the spirit of prog21_c.c / prog21b_s.c:
PID and data are sent in binary, not as text.
On typical systems this is 16 bytes.
*/
typedef struct
{
    int type;      /* message type */
    pid_t pid;     /* student pid */
    int value;     /* attempt score OR result (0/1) */
    int stage;     /* current stage number */
} Message;

typedef struct
{
    pid_t pid;
    int to_student[2];     /* teacher writes [1], student reads [0] */
    int alive;
    int finished;
    int current_stage;     /* 1..4, or 5 if fully completed */
    int points;
    int skill;
} StudentInfo;

volatile sig_atomic_t alarm_fired = 0;

/* ---------- common helpers ---------- */

static void set_handler(void (*handler)(int), int sig)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    if (sigaction(sig, &act, NULL) < 0)
        ERR("sigaction");
}

static void sigalrm_handler(int sig)
{
    (void)sig;
    alarm_fired = 1;
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
                return -2; /* let caller react to EINTR/alarm */
            return -1;
        }
        if (ret == 0)
        {
            if (done == 0)
                return 0;  /* EOF */
            return -1;     /* broken partial message */
        }
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
        return -2;  /* interrupted */
    if (ret < 0)
        return -1;  /* error */

    return 1;
}

static int rand_range(int a, int b)
{
    return a + rand() % (b - a + 1);
}

static int find_student_index_by_pid(StudentInfo *students, int n, pid_t pid)
{
    for (int i = 0; i < n; i++)
        if (students[i].pid == pid)
            return i;
    return -1;
}

static int all_students_done(StudentInfo *students, int n)
{
    for (int i = 0; i < n; i++)
        if (students[i].alive && !students[i].finished)
            return 0;
    return 1;
}

static void print_summary(StudentInfo *students, int n)
{
    for (int i = 0; i < n; i++)
        printf("Teacher: %d - %d\n", (int)students[i].pid, students[i].points);
    fflush(stdout);
}

static void reap_children(void)
{
    while (wait(NULL) > 0)
        ;
    if (errno != ECHILD)
        ERR("wait");
}

/* ---------- student process ---------- */

/*
Student side:
- prints PID
- attendance reply over shared pipe + stdout
- then repeatedly works on current stage
- if teacher disappears, student prints "need more time" message and exits

Communication model is based mainly on prog22a.c / prog22b.c:
shared pipe students->teacher, dedicated teacher->student pipe.
*/
static void student_work(int teacher_to_student_fd, int shared_to_teacher_fd, int skill)
{
    Message msg;
    int current_stage = 1;
    pid_t mypid = getpid();

    srand((unsigned int)mypid);

    printf("Student: %d\n", (int)mypid);
    fflush(stdout);

    for (;;)
    {
        int rr = recv_message(teacher_to_student_fd, &msg);
        if (rr == 0)
        {
            if (current_stage <= 4)
            {
                printf("Student %d: Oh no, I haven't finished stage %d. I need more time.\n",
                       (int)mypid, current_stage);
                fflush(stdout);
            }
            break;
        }
        if (rr < 0)
            ERR("student read");

        if (msg.type == MSG_ATTENDANCE_QUESTION)
        {
            printf("Student %d: HERE!\n", (int)mypid);
            fflush(stdout);

            Message reply;
            reply.type = MSG_ATTENDANCE_HERE;
            reply.pid = mypid;
            reply.value = 0;
            reply.stage = 0;

            if (send_message(shared_to_teacher_fd, &reply) < 0)
            {
                if (errno == EPIPE)
                {
                    printf("Student %d: Oh no, I haven't finished stage %d. I need more time.\n",
                           (int)mypid, current_stage);
                    fflush(stdout);
                    break;
                }
                ERR("student attendance write");
            }

            /* attendance finished, start solving from stage 1 */
            while (current_stage <= 4)
            {
                int t_ms = rand_range(100, 500);
                usleep((useconds_t)t_ms * 1000);

                int q = rand_range(1, 20);
                int attempt_score = skill + q;

                Message attempt;
                attempt.type = MSG_STAGE_ATTEMPT;
                attempt.pid = mypid;
                attempt.value = attempt_score;
                attempt.stage = current_stage;

                if (send_message(shared_to_teacher_fd, &attempt) < 0)
                {
                    if (errno == EPIPE)
                    {
                        printf("Student %d: Oh no, I haven't finished stage %d. I need more time.\n",
                               (int)mypid, current_stage);
                        fflush(stdout);
                        goto student_cleanup;
                    }
                    ERR("student attempt write");
                }

                rr = recv_message(teacher_to_student_fd, &msg);
                if (rr == 0)
                {
                    printf("Student %d: Oh no, I haven't finished stage %d. I need more time.\n",
                           (int)mypid, current_stage);
                    fflush(stdout);
                    goto student_cleanup;
                }
                if (rr < 0)
                    ERR("student result read");

                if (msg.type == MSG_STAGE_RESULT)
                {
                    if (msg.value == 1)
                        current_stage++;
                }
            }

            printf("Student %d: I NAILED IT!\n", (int)mypid);
            fflush(stdout);
            break;
        }
    }

student_cleanup:
    close_fd(&teacher_to_student_fd, "STUDENT");
    close_fd(&shared_to_teacher_fd, "STUDENT");
    _exit(EXIT_SUCCESS);
}

/* ---------- stages ---------- */

/*
Stage 1:
Teacher creates n child processes.
Each student prints PID.
This follows the style of prog22a.c / prog22b.c:
fork children, close unused ends after fork.
*/
static void stage1_create_students(StudentInfo *students, int n, int shared_pipe[2])
{
    for (int i = 0; i < n; i++)
    {
        students[i].alive = 1;
        students[i].finished = 0;
        students[i].current_stage = 1;
        students[i].points = 0;
        students[i].skill = rand_range(3, 9);
        students[i].to_student[0] = students[i].to_student[1] = -1;

        if (pipe(students[i].to_student) < 0)
            ERR("pipe to_student");

        pid_t pid = fork();
        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            for (int j = 0; j <= i; j++)
            {
                if (j != i)
                {
                    close_fd(&students[j].to_student[0], "STUDENT");
                    close_fd(&students[j].to_student[1], "STUDENT");
                }
            }

            close_fd(&students[i].to_student[1], "STUDENT");
            close_fd(&shared_pipe[0], "STUDENT");

            student_work(students[i].to_student[0], shared_pipe[1], students[i].skill);
        }

        students[i].pid = pid;
        close_fd(&students[i].to_student[0], "TEACHER");
    }

    close_fd(&shared_pipe[1], "TEACHER");
}

/*
Stage 2:
Attendance check.
Teacher sends attendance question to each student via dedicated pipe.
Student replies via shared pipe and stdout.
*/
static void stage2_attendance(StudentInfo *students, int n, int shared_read_fd)
{
    Message msg;

    for (int i = 0; i < n; i++)
    {
        printf("Teacher: Is %d here?\n", (int)students[i].pid);
        fflush(stdout);

        Message q;
        q.type = MSG_ATTENDANCE_QUESTION;
        q.pid = students[i].pid;
        q.value = 0;
        q.stage = 0;

        if (send_message(students[i].to_student[1], &q) < 0)
        {
            students[i].alive = 0;
            close_fd(&students[i].to_student[1], "TEACHER");
            continue;
        }

        int rr = recv_message(shared_read_fd, &msg);
        if (rr <= 0)
        {
            students[i].alive = 0;
            continue;
        }

        if (msg.type == MSG_ATTENDANCE_HERE)
        {
            /* student already printed HERE on stdout */
        }
    }
}

/*
Stage 3 + 4:
Teacher grades stage attempts.
Student sends {pid, attempt_score, stage}.
Teacher creates difficulty = base_points + rand[1,20].
If attempt_score >= difficulty -> success, else failure.
Teacher prints the required message and sends result back.
*/
static void stage3_and_4_grade(StudentInfo *students, int n, int shared_read_fd)
{
    const int base_points[4] = {3, 6, 7, 5};
    Message msg;

    alarm(LAB_TIME);

    for (;;)
    {
        if (alarm_fired)
            break;

        if (all_students_done(students, n))
            break;

        int rr = recv_message(shared_read_fd, &msg);

        if (rr == -2)
        {
            if (alarm_fired)
                break;
            continue;
        }
        if (rr == 0)
            break;
        if (rr < 0)
            ERR("teacher read shared");

        if (msg.type != MSG_STAGE_ATTEMPT)
            continue;

        int idx = find_student_index_by_pid(students, n, msg.pid);
        if (idx < 0)
            continue;
        if (!students[idx].alive || students[idx].finished)
            continue;

        int st = msg.stage;
        if (st < 1 || st > 4)
            continue;

        int difficulty = base_points[st - 1] + rand_range(1, 20);
        int success = (msg.value >= difficulty) ? 1 : 0;

        Message result;
        result.type = MSG_STAGE_RESULT;
        result.pid = students[idx].pid;
        result.value = success;
        result.stage = st;

        if (success)
        {
            students[idx].points += base_points[st - 1];
            students[idx].current_stage++;

            printf("Teacher: Student %d finished stage %d\n",
                   (int)students[idx].pid, st);
            fflush(stdout);

            if (students[idx].current_stage == 5)
                students[idx].finished = 1;
        }
        else
        {
            printf("Teacher: Student %d needs to fix stage %d\n",
                   (int)students[idx].pid, st);
            fflush(stdout);
        }

        if (send_message(students[idx].to_student[1], &result) < 0)
        {
            students[idx].alive = 0;
            close_fd(&students[idx].to_student[1], "TEACHER");
        }
    }
}

/*
Stage 5:
Time limit.
When alarm fires:
- teacher prints END OF TIME
- stops grading
- prints summary
- closes pipes so students detect teacher has gone
- waits for students
- prints final message
*/
static void stage5_finish(StudentInfo *students, int n, int shared_read_fd)
{
    if (alarm_fired)
    {
        printf("Teacher: END OF TIME!\n");
        fflush(stdout);
    }

    print_summary(students, n);

    close_fd(&shared_read_fd, "TEACHER");
    for (int i = 0; i < n; i++)
        close_fd(&students[i].to_student[1], "TEACHER");

    reap_children();

    printf("Teacher: IT'S FINALLY OVER!\n");
    fflush(stdout);
}

static void usage(char *name)
{
    fprintf(stderr, "USAGE: %s n\n", name);
    fprintf(stderr, "%d <= n <= %d\n", MIN_N, MAX_N);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    int n;
    int shared_pipe[2];
    StudentInfo *students = NULL;

    if (argc != 2)
        usage(argv[0]);

    n = atoi(argv[1]);
    if (n < MIN_N || n > MAX_N)
        usage(argv[0]);

    srand((unsigned int)getpid());

    /* Like prog22b.c: teacher ignores SIGPIPE */
    set_handler(SIG_IGN, SIGPIPE);
    set_handler(sigalrm_handler, SIGALRM);

    if (pipe(shared_pipe) < 0)
        ERR("pipe shared");

    students = (StudentInfo *)calloc((size_t)n, sizeof(StudentInfo));
    if (students == NULL)
        ERR("calloc");

    printf("Teacher: %d\n", (int)getpid());
    fflush(stdout);

    /* Stage 1 */
    stage1_create_students(students, n, shared_pipe);

    /* Stage 2 */
    stage2_attendance(students, n, shared_pipe[0]);

    /* Stage 3 + 4 */
    stage3_and_4_grade(students, n, shared_pipe[0]);

    /* Stage 5 */
    stage5_finish(students, n, shared_pipe[0]);

    free(students);
    return EXIT_SUCCESS;
}