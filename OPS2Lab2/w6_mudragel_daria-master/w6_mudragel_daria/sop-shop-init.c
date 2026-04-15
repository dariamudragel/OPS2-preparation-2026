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

void usage(char* program_name)
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

void swap(int* x, int* y)
{
    int tmp = *y;
    *y = *x;
    *x = tmp;
}

void shuffle(int* array, int n)
{
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap(&array[i], &array[j]);
    }
}

void print_array(int* array, int n)
{
    for (int i = 0; i < n; ++i)
    {
        printf("%3d ", array[i]);
    }
    printf("\n");
}

void worker(int* shop, int n)
{
    printf("[%d] Worker reports for a night shift.\n", getpid());

    for (int i = 0; i < 10; i++)
    {
        int a = rand() % n;
        int b = rand() % n;

        if (a == b)
        {
            continue;
        }

        if (shop[a] > shop[b])
        {
            swap(&shop[a], &shop[b]);
            ms_sleep(100);
        }
    }

    exit(0);
}

int main(int argc, char** argv)
{
    srand(time(NULL));

    if (argc != 3)
    {
        usage(argv[0]);
        exit(1);
    }
    int N = atoi(argv[1]);
    int M = atoi(argv[2]);

    if (N < MIN_SHELVES || N > MAX_SHELVES || M < MIN_WORKERS || M > MAX_WORKERS)
    {
        usage(argv[0]);
        exit(1);
    }

    int fd = open(SHOP_FILENAME, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd == -1)
    {
        ERR("open");
    }

    if (ftruncate(fd, N * sizeof(int)) == -1)
    {
        ERR("ftruncate");
    }

    int* shop = mmap(NULL, N * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shop == MAP_FAILED)
    {
        ERR("mmap");
    }

    close(fd);

    for (int i = 0; i < N; i++)
    {
        shop[i] = i + 1;
    }

    shuffle(shop, N);

    for (int i = 0; i < M; i++)
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            worker(shop, N);
        }
        else if (pid < 0)
        {
            ERR("fork");
        }
    }

    for (int i = 0; i < M; i++)
    {
        wait(NULL);
    }

    printf("After:\n");
    print_array(shop, N);

    printf("Night shift in Bitronka is over\n");

    if (msync(shop, N * sizeof(int), MS_SYNC))
        ERR("msync");
    if (munmap(shop, N * sizeof(int)))
        ERR("munmap");

    return EXIT_SUCCESS;
}
