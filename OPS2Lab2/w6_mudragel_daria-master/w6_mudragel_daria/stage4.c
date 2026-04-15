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

#define NO_WORKER (-1)

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)

/*
    Shared memory used by all processes.

    Why all of this is here:
    - Stage 2/3 already required shared anonymous memory for mutexes
      and other shared variables.
    - Stage 4 adds:
        * alive/dead worker counting
        * finding dead bodies
        * manager printing alive count
    - robust mutexes are the correct tool here, because a worker
      may die while holding shelf locks.
*/
typedef struct
{
    int stop_work;                            /* manager sets this to 1 when shift ends */
    int alive_workers;                        /* current number of living workers */
    int worker_dead[MAX_WORKERS];             /* 0 = alive/not yet confirmed dead, 1 = dead */
    int shelf_owner[MAX_SHELVES];             /* which worker currently owns given shelf mutex */
    pthread_mutex_t info_mutex;               /* protects alive_workers and worker_dead[] */
    pthread_mutex_t shelf_mutex[MAX_SHELVES]; /* one mutex per shelf */
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
        printf("%3d ", array[i]);
    printf("\n");
}

/*
    Robust mutex lock for the info mutex.

    Robust mutexes may return EOWNERDEAD when the previous owner died.
    In that case the current process still gets the lock, but must call
    pthread_mutex_consistent() before unlocking it.
*/
void lock_info_mutex(shared_data_t *shared)
{
    int err = pthread_mutex_lock(&shared->info_mutex);
    if (err == EOWNERDEAD)
    {
        if (pthread_mutex_consistent(&shared->info_mutex) != 0)
            ERR("pthread_mutex_consistent");
        return;
    }
    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_lock info_mutex");
    }
}

void unlock_info_mutex(shared_data_t *shared)
{
    int err = pthread_mutex_unlock(&shared->info_mutex);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_unlock info_mutex");
    }
}

/*
    Called when someone finds a dead worker through a robust shelf mutex.

    Important detail:
    the same dead worker can be "found" more than once, because if the worker
    died holding two shelf mutexes, one process may discover one shelf and
    another process may discover the second shelf.

    Therefore:
    - we PRINT every discovery message
    - but we DECREMENT alive_workers only once
*/
void handle_dead_worker(shared_data_t *shared, int shelf_idx, pid_t finder_pid)
{
    int dead_worker = shared->shelf_owner[shelf_idx];

    printf("[%d] Found a dead body in aisle %d.\n", finder_pid, shelf_idx + 1);
    fflush(stdout);

    lock_info_mutex(shared);

    if (dead_worker >= 0 && dead_worker < MAX_WORKERS && shared->worker_dead[dead_worker] == 0)
    {
        shared->worker_dead[dead_worker] = 1;
        shared->alive_workers--;
    }

    unlock_info_mutex(shared);
}

/*
    Robust lock of one shelf mutex.

    If the previous owner died while holding this shelf mutex,
    pthread_mutex_lock returns EOWNERDEAD.
    That is exactly how we discover dead workers in Stage 4.

    After EOWNERDEAD:
    - the current process owns the mutex
    - we report the dead worker
    - we make the mutex consistent
*/
void lock_shelf(shared_data_t *shared, int shelf_idx, pid_t finder_pid)
{
    int err = pthread_mutex_lock(&shared->shelf_mutex[shelf_idx]);

    if (err == EOWNERDEAD)
    {
        handle_dead_worker(shared, shelf_idx, finder_pid);

        if (pthread_mutex_consistent(&shared->shelf_mutex[shelf_idx]) != 0)
            ERR("pthread_mutex_consistent shelf_mutex");
        return;
    }

    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_lock shelf_mutex");
    }
}

void unlock_shelf(shared_data_t *shared, int shelf_idx)
{
    int err = pthread_mutex_unlock(&shared->shelf_mutex[shelf_idx]);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_mutex_unlock shelf_mutex");
    }
}

int get_alive_workers(shared_data_t *shared)
{
    int alive;
    lock_info_mutex(shared);
    alive = shared->alive_workers;
    unlock_info_mutex(shared);
    return alive;
}

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
    Stage 4 worker.

    New behavior compared to Stage 3:
    - worker has an assigned worker_id
    - just before swap there is 1% chance of sudden death
    - before aborting, it prints only the required death message

    Very important trick:
    We store worker_id in shelf_owner[first/second] AFTER locking both shelves.
    That way, if this worker aborts while holding them, another process can see
    which worker died and decrease alive count only once.
*/
void worker(int *shop, int n, shared_data_t *shared, int worker_id)
{
    srand((unsigned int)(time(NULL) ^ getpid() ^ (worker_id << 16)));

    printf("[%d] Worker reports for a night shift.\n", getpid());
    fflush(stdout);

    while (!shared->stop_work)
    {
        int a = rand() % n;
        int b = rand() % n;

        while (a == b)
            b = rand() % n;

        /*
            We always lock in increasing index order.
            This avoids deadlock when two workers try to lock the same pair
            in opposite order.
        */
        int first = (a < b) ? a : b;
        int second = (a < b) ? b : a;

        lock_shelf(shared, first, getpid());
        lock_shelf(shared, second, getpid());

        /*
            Now this worker owns both shelves.
            We record that in shared memory.
            If this worker dies now, others can discover its body later.
        */
        shared->shelf_owner[first] = worker_id;
        shared->shelf_owner[second] = worker_id;

        /*
            Manager may have already ended the shift while this worker
            was waiting for mutexes.
        */
        if (shared->stop_work)
        {
            shared->shelf_owner[second] = NO_WORKER;
            shared->shelf_owner[first] = NO_WORKER;
            unlock_shelf(shared, second);
            unlock_shelf(shared, first);
            break;
        }

        /*
            Stage 4:
            1% chance of sudden death JUST BEFORE SWAPPING.
            We do it only in the branch where a swap would happen,
            because "just before swapping" means right before the real swap.
        */
        if (shop[a] > shop[b])
        {
            if ((rand() % 100) == 0)
            {
                printf("[%d] Trips over pallet and dies.\n", getpid());
                fflush(stdout);

                /*
                    abort() terminates the process immediately.
                    We intentionally DO NOT unlock the mutexes here.
                    That is the whole point of using robust mutexes:
                    other processes will detect the dead owner.
                */
                abort();
            }

            swap(&shop[a], &shop[b]);
            ms_sleep(100);
        }

        shared->shelf_owner[second] = NO_WORKER;
        shared->shelf_owner[first] = NO_WORKER;

        unlock_shelf(shared, second);
        unlock_shelf(shared, first);
    }

    exit(EXIT_SUCCESS);
}

/*
    Stage 4 manager.

    Compared to Stage 3:
    - after printing array state, manager prints current alive count
    - if alive count becomes 0, manager prints:
        [PID] All workers died, I hate my job
      and terminates
*/
void manager(int *shop, int n, shared_data_t *shared)
{
    printf("[%d] Manager reports for a night shift.\n", getpid());
    fflush(stdout);

    while (1)
    {
        ms_sleep(500);

        /*
            Lock whole shop to get a stable snapshot.
            If a dead worker is found while taking the snapshot,
            robust mutex handling will detect it here.
        */
        for (int i = 0; i < n; i++)
            lock_shelf(shared, i, getpid());

        print_array(shop, n);
        fflush(stdout);

        if (msync(shop, n * sizeof(int), MS_SYNC) == -1)
            ERR("msync");

        int alive = get_alive_workers(shared);
        printf("[%d] Workers alive: %d.\n", getpid(), alive);
        fflush(stdout);

        if (alive == 0)
        {
            printf("[%d] All workers died, I hate my job.\n", getpid());
            fflush(stdout);

            shared->stop_work = 1;

            for (int i = n - 1; i >= 0; i--)
            {
                shared->shelf_owner[i] = NO_WORKER;
                unlock_shelf(shared, i);
            }

            exit(EXIT_SUCCESS);
        }

        if (is_sorted(shop, n))
        {
            printf("[%d] The shop shelves are sorted.\n", getpid());
            fflush(stdout);

            shared->stop_work = 1;

            for (int i = n - 1; i >= 0; i--)
            {
                shared->shelf_owner[i] = NO_WORKER;
                unlock_shelf(shared, i);
            }

            exit(EXIT_SUCCESS);
        }

        for (int i = n - 1; i >= 0; i--)
        {
            shared->shelf_owner[i] = NO_WORKER;
            unlock_shelf(shared, i);
        }
    }
}

void init_shared(shared_data_t *shared, int n, int m)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0)
        ERR("pthread_mutexattr_init");

    /*
        Process-shared:
        needed because different PROCESSES use these mutexes.
    */
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
        ERR("pthread_mutexattr_setpshared");

    /*
        Robust:
        needed because Stage 4 explicitly kills workers while they may
        still hold shelf mutexes.
    */
    if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
        ERR("pthread_mutexattr_setrobust");

    if (pthread_mutex_init(&shared->info_mutex, &attr) != 0)
        ERR("pthread_mutex_init info_mutex");

    for (int i = 0; i < n; i++)
    {
        if (pthread_mutex_init(&shared->shelf_mutex[i], &attr) != 0)
            ERR("pthread_mutex_init shelf_mutex");
        shared->shelf_owner[i] = NO_WORKER;
    }

    if (pthread_mutexattr_destroy(&attr) != 0)
        ERR("pthread_mutexattr_destroy");

    shared->stop_work = 0;
    shared->alive_workers = m;

    for (int i = 0; i < MAX_WORKERS; i++)
        shared->worker_dead[i] = 0;
}

void destroy_shared(shared_data_t *shared, int n)
{
    if (pthread_mutex_destroy(&shared->info_mutex) != 0)
        ERR("pthread_mutex_destroy info_mutex");

    for (int i = 0; i < n; i++)
    {
        if (pthread_mutex_destroy(&shared->shelf_mutex[i]) != 0)
            ERR("pthread_mutex_destroy shelf_mutex");
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
        File-backed mmap for the shop shelves.
        This is the same core idea from previous stages.
    */
    int fd = open(SHOP_FILENAME, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd == -1)
        ERR("open");

    if (ftruncate(fd, N * (int)sizeof(int)) == -1)
        ERR("ftruncate");

    int *shop = mmap(NULL, N * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shop == MAP_FAILED)
        ERR("mmap shop");

    if (close(fd) == -1)
        ERR("close");

    for (int i = 0; i < N; i++)
        shop[i] = i + 1;

    shuffle(shop, N);

    /*
        Anonymous shared memory for mutexes and Stage 4 metadata.
    */
    shared_data_t *shared = mmap(NULL,
                                 sizeof(shared_data_t),
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS,
                                 -1,
                                 0);
    if (shared == MAP_FAILED)
        ERR("mmap shared");

    init_shared(shared, N, M);

    /*
        Keep Stage 2 behavior:
        print initial shop state before workers start.
    */
    print_array(shop, N);
    fflush(stdout);

    /*
        Create workers.
        Each gets a unique worker_id from 0 to M-1.
    */
    for (int i = 0; i < M; i++)
    {
        pid_t pid = fork();

        if (pid == 0)
            worker(shop, N, shared, i);
        else if (pid < 0)
            ERR("fork worker");
    }

    /*
        Create manager after workers.
    */
    pid_t manager_pid = fork();
    if (manager_pid == 0)
        manager(shop, N, shared);
    else if (manager_pid < 0)
        ERR("fork manager");

    /*
        Parent waits for all workers and the manager.
        Some workers may exit normally, some may abort.
        wait() handles both.
    */
    for (int i = 0; i < M + 1; i++)
    {
        if (wait(NULL) == -1)
            ERR("wait");
    }

    /*
        Final print from parent, as in previous stages.
    */
    print_array(shop, N);
    printf("Night shift in Bitronka is over\n");

    if (msync(shop, N * sizeof(int), MS_SYNC) == -1)
        ERR("msync final");

    destroy_shared(shared, N);

    if (munmap(shared, sizeof(shared_data_t)) == -1)
        ERR("munmap shared");

    if (munmap(shop, N * sizeof(int)) == -1)
        ERR("munmap shop");

    return EXIT_SUCCESS;
}