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

typedef struct
{
    int stop_work;
    pthread_mutex_t shelf_mutex[MAX_SHELVES];
} shared_data_t;

void usage(char *program_name)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "\t%s n m\n", program_name);
    fprintf(stderr, "\t  n - number of items (shelves), %d <= n <= %d\n", MIN_SHELVES, MAX_SHELVES);
    fprintf(stderr, "\t  m - number of workers, %d <= m <= %d\n", MIN_WORKERS, MAX_WORKERS);
    exit(EXIT_FAILURE);
}

void ms_sleep(unsigned int milli)
{
    time_t sec = (int)(milli / 1000);
    milli = milli - (sec * 1000);

    struct timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = milli * 1000000L;

    if (nanosleep(&ts, &ts) == -1)
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

void worker(int *shop, int n, shared_data_t *shared)
{
    srand((unsigned int)(time(NULL) ^ getpid()));

    printf("[%d] Worker reports for a night shift.\n", getpid());
    fflush(stdout);

    for (int i = 0; i < 10; i++)
    {
        int a = rand() % n;
        int b = rand() % n;

        while (a == b)
            b = rand() % n;

        int first = (a < b) ? a : b;
        int second = (a < b) ? b : a;

        lock_mutex(&shared->shelf_mutex[first]);
        lock_mutex(&shared->shelf_mutex[second]);

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

    for (int i = 0; i < N; i++)
        shop[i] = i + 1;

    shuffle(shop, N);

    shared_data_t *shared = mmap(NULL,
                                 sizeof(shared_data_t),
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS,
                                 -1,
                                 0);
    if (shared == MAP_FAILED)
        ERR("mmap shared");

    init_shared_mutexes(shared, N);

    print_array(shop, N);
    fflush(stdout);

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

    for (int i = 0; i < M; i++)
    {
        if (wait(NULL) == -1)
            ERR("wait");
    }

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