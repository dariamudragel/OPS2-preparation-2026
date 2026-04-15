#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHOP_FILENAME "./shop"
#define MIN_SHELVES 8
#define MAX_SHELVES 256
#define MIN_WORKERS 1
#define MAX_WORKERS 64

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)

/*
    Shared memory structure for Stage 3.

    Why is this needed?
    Stage 3 says:
    - mutexes should be in anonymous shared memory,
    - other shared variables should be there too,
    - manager announces end of work through a shared variable.

    So here we store:
    - stop_work: manager sets it to 1 when shelves are sorted
    - shelf_mutex: one mutex per shelf
*/
typedef struct
{
    int stop_work;
    pthread_mutex_t shelf_mutex[MAX_SHELVES];
} shared_data_t;

void usage(char *program_name)
{
    fprintf(stderr, "Usage: \n");
    fprintf(stderr, "\t%s n m\n", program_name);
    fprintf(stderr, "\t  n - number of items (shelves), %d <= n <= %d\n", MIN_SHELVES, MAX_SHELVES);
    fprintf(stderr, "\t  m - number of workers, %d <= m <= %d\n", MIN_WORKERS, MAX_WORKERS);
    exit(EXIT_FAILURE);
}

void ms_sleep(unsigned int milli)
{
    time_t sec = (int)(milli / 1000);
    milli = milli - (sec * 1000);

    struct timespec ts = {0};
    ts.tv_sec = sec;
    ts.tv_nsec = milli * 1000000L;

    if (nanosleep(&ts, &ts))
        ERR("nanosleep");
}

void swap(int *x, int *y)
{
    int tmp = *y;
    *y = *x;
    *x = tmp;
}

void shuffle(int *array, int n)
{
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap(&array[i], &array[j]);
    }
}

void print_array(int *array, int n)
{
    for (int i = 0; i < n; ++i)
    {
        printf("%3d ", array[i]);
    }
    printf("\n");
}

/*
    Lock helper.

    pthread functions return error codes directly,
    so we convert them into errno-like behavior for ERR().
*/
void lock_mutex(pthread_mutex_t *mtx)
{
    int err = pthread_mutex_lock(mtx);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_lock");
    }
}

void unlock_mutex(pthread_mutex_t *mtx)
{
    int err = pthread_mutex_unlock(mtx);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_unlock");
    }
}

/*
    Stage 3 workers continue working until manager announces the end.
    So unlike Stage 2, there is no "10 times and stop" rule anymore.

    Important detail:
    We still use one mutex per shelf.
    And we always lock lower index first, then higher index.
    This avoids deadlock.
*/
void worker(int *shop, int n, shared_data_t *shared)
{
    srand((unsigned int)(time(NULL) ^ getpid()));

    printf("[%d] Worker reports for a night shift.\n", getpid());
    fflush(stdout);

    while (!shared->stop_work)
    {
        int a = rand() % n;
        int b = rand() % n;

        while (a == b)
            b = rand() % n;

        int first = (a < b) ? a : b;
        int second = (a < b) ? b : a;

        lock_mutex(&shared->shelf_mutex[first]);
        lock_mutex(&shared->shelf_mutex[second]);

        /*
            Check again after locking.
            Maybe manager already announced the end while
            this worker was waiting for mutexes.
        */
        if (shared->stop_work)
        {
            unlock_mutex(&shared->shelf_mutex[second]);
            unlock_mutex(&shared->shelf_mutex[first]);
            break;
        }

        /*
            The task says:
            if value at first chosen index > value at second chosen index,
            swap them and sleep 100ms during the swap.

            "first chosen index" means original a and b, not lock order.
        */
        if (shop[a] > shop[b])
        {
            swap(&shop[a], &shop[b]);
            ms_sleep(100);
        }

        unlock_mutex(&shared->shelf_mutex[second]);
        unlock_mutex(&shared->shelf_mutex[first]);
    }

    exit(EXIT_SUCCESS);
}

/*
    Checks whether the array is sorted in nondecreasing order.

    Manager uses this every 0.5 second.
*/
int is_sorted(int *shop, int n)
{
    for (int i = 0; i < n - 1; i++)
    {
        if (shop[i] > shop[i + 1])
            return 0;
    }
    return 1;
}

/*
    Manager process for Stage 3.

    It must:
    - report in,
    - every 0.5 second print the array,
    - synchronize the mapped file,
    - check if sorted,
    - if sorted:
        * print that shelves are sorted
        * set shared stop variable
        * terminate
*/
void manager(int *shop, int n, shared_data_t *shared)
{
    printf("[%d] Manager reports for a night shift.\n", getpid());
    fflush(stdout);

    while (1)
    {
        ms_sleep(500);

        /*
            To print a stable snapshot of the array, manager locks all shelf mutexes.

            Why?
            Without this, it could print half-old / half-new state while
            workers are in the middle of swaps.
        */
        for (int i = 0; i < n; i++)
            lock_mutex(&shared->shelf_mutex[i]);

        print_array(shop, n);
        fflush(stdout);

        /*
            Stage 3 says manager synchronizes the file with current state.
            Since shop is a file-backed mmap, msync() is the correct tool.
        */
        if (msync(shop, n * sizeof(int), MS_SYNC) == -1)
            ERR("msync");

        if (is_sorted(shop, n))
        {
            printf("[%d] The shop shelves are sorted\n", getpid());
            fflush(stdout);

            /*
                Tell workers to stop.
                This shared variable lives in anonymous shared memory.
            */
            shared->stop_work = 1;

            for (int i = n - 1; i >= 0; i--)
                unlock_mutex(&shared->shelf_mutex[i]);

            exit(EXIT_SUCCESS);
        }

        for (int i = n - 1; i >= 0; i--)
            unlock_mutex(&shared->shelf_mutex[i]);
    }
}

/*
    Initializes process-shared mutexes.

    This is required because mutexes must be usable by different processes,
    not just by threads in one process.
*/
void init_shared_mutexes(shared_data_t *shared, int n)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0)
        ERR("pthread_mutexattr_init");

    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
        ERR("pthread_mutexattr_setpshared");

    for (int i = 0; i < n; i++)
    {
        if (pthread_mutex_init(&shared->shelf_mutex[i], &attr) != 0)
            ERR("pthread_mutex_init");
    }

    if (pthread_mutexattr_destroy(&attr) != 0)
        ERR("pthread_mutexattr_destroy");

    shared->stop_work = 0;
}

void destroy_shared_mutexes(shared_data_t *shared, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (pthread_mutex_destroy(&shared->shelf_mutex[i]) != 0)
            ERR("pthread_mutex_destroy");
    }
}

int main(int argc, char **argv)
{
    srand((unsigned int)time(NULL));

    if (argc != 3)
        usage(argv[0]);

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);

    if (N < MIN_SHELVES || N > MAX_SHELVES || M < MIN_WORKERS || M > MAX_WORKERS)
        usage(argv[0]);

    /*
        Stage 1/2 base:
        create file and map shop array into memory.
    */
    int fd = open(SHOP_FILENAME, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd == -1)
        ERR("open");

    if (ftruncate(fd, N * (int)sizeof(int)) == -1)
        ERR("ftruncate");

    int *shop = mmap(NULL, N * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shop == MAP_FAILED)
        ERR("mmap");

    if (close(fd) == -1)
        ERR("close");

    /*
        Initialize shelves and shuffle them.
    */
    for (int i = 0; i < N; i++)
        shop[i] = i + 1;

    shuffle(shop, N);

    /*
        Anonymous shared memory for mutexes and shared variables.
    */
    shared_data_t *shared = mmap(NULL,
                                 sizeof(shared_data_t),
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS,
                                 -1,
                                 0);
    if (shared == MAP_FAILED)
        ERR("mmap shared");

    init_shared_mutexes(shared, N);

    /*
        Stage 2 requires printing array before workers start.
        We keep it here too, because Stage 3 builds on Stage 2.
    */
    print_array(shop, N);
    fflush(stdout);

    /*
        Create worker processes.
    */
    for (int i = 0; i < M; i++)
    {
        pid_t pid = fork();

        if (pid == 0)
        {
            worker(shop, N, shared);
        }
        else if (pid < 0)
        {
            ERR("fork");
        }
    }

    /*
        After workers are created, Stage 3 says to create manager.
    */
    pid_t manager_pid = fork();
    if (manager_pid == 0)
    {
        manager(shop, N, shared);
    }
    else if (manager_pid < 0)
    {
        ERR("fork");
    }

    /*
        Parent waits for all workers and manager.
        That is M workers + 1 manager.
    */
    for (int i = 0; i < M + 1; i++)
    {
        if (wait(NULL) == -1)
            ERR("wait");
    }

    /*
        After all child processes finish, parent prints final state
        and the end message, just like in Stage 2.
    */
    print_array(shop, N);
    printf("Night shift in Bitronka is over\n");

    if (msync(shop, N * sizeof(int), MS_SYNC) == -1)
        ERR("msync");

    destroy_shared_mutexes(shared, N);

    if (munmap(shared, sizeof(shared_data_t)) == -1)
        ERR("munmap shared");

    if (munmap(shop, N * sizeof(int)) == -1)
        ERR("munmap shop");

    return EXIT_SUCCESS;
}