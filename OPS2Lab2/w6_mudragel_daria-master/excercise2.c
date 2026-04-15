#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHM_NAME "/shared_integration_shm"
#define SEM_NAME "/shared_integration_init_sem"

#define ERR(source) \
    (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), kill(0, SIGKILL), exit(EXIT_FAILURE))

volatile sig_atomic_t keep_running = 1;

/* Values of this function are in range (0,1] */
double func(double x)
{
    usleep(2000);
    return exp(-x * x);
}

/**
 * It counts hit points by Monte Carlo method.
 * Use it to process one batch of computation.
 * @param N Number of points to randomize
 * @param a Lower bound of integration
 * @param b Upper bound of integration
 * @return Number of points which was hit.
 */
int randomize_points(int N, float a, float b)
{
    int result = 0;
    for (int i = 0; i < N; ++i)
    {
        double rand_x = ((double)rand() / RAND_MAX) * (b - a) + a;
        double rand_y = ((double)rand() / RAND_MAX);
        double real_y = func(rand_x);

        if (rand_y <= real_y)
            result++;
    }
    return result;
}

/**
 * This function calculates approximation of integral from counters of hit and total points.
 * @param total_randomized_points Number of total randomized points.
 * @param hit_points Number of hit points.
 * @param a Lower bound of integration
 * @param b Upper bound of integration
 * @return The approximation of integral
 */
double summarize_calculations(uint64_t total_randomized_points, uint64_t hit_points, float a, float b)
{
    return (b - a) * ((double)hit_points / (double)total_randomized_points);
}

/**
 * This function locks mutex and can sometime die (it has 2% chance to die).
 * It cannot die if lock would return an error.
 * It doesn't handle any errors. It's users responsibility.
 * Use it only in STAGE 4.
 *
 * @param mtx Mutex to lock
 * @return Value returned from pthread_mutex_lock.
 */
int random_death_lock(pthread_mutex_t* mtx)
{
    int ret = pthread_mutex_lock(mtx);
    if (ret)
        return ret;

    /* 2% chance to die */
    if (rand() % 50 == 0)
        abort();
    return ret;
}

typedef struct
{
    pthread_mutex_t mtx;
    uint64_t process_count;
    uint64_t total_points;
    uint64_t hit_points;
    float a;
    float b;
    int N;
    int initialized;
} shared_data_t;

void usage(char* argv[])
{
    printf("%s a b N - calculating integral with multiple processes\n", argv[0]);
    printf("a - Start of segment for integral (default: -1)\n");
    printf("b - End of segment for integral (default: 1)\n");
    printf("N - Size of batch to calculate before reporting to shared memory (default: 1000)\n");
}

/* SIGINT handler for Stage 3 */
void sigint_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

/*
    Helper used in Stage 4:
    lock robust mutex and recover if owner died.

    If previous owner died while holding the mutex:
    - pthread_mutex_lock returns EOWNERDEAD
    - we assume one process disappeared and decrement process_count
    - then mark mutex consistent
*/
void robust_lock_with_recovery(shared_data_t* shm)
{
    int ret = random_death_lock(&shm->mtx);

    if (ret == EOWNERDEAD)
    {
        if (shm->process_count > 0)
            shm->process_count--;

        if (pthread_mutex_consistent(&shm->mtx) != 0)
            ERR("pthread_mutex_consistent");
        return;
    }

    if (ret != 0)
    {
        errno = ret;
        ERR("pthread_mutex_lock");
    }
}

void robust_unlock(shared_data_t* shm)
{
    int ret = pthread_mutex_unlock(&shm->mtx);
    if (ret != 0)
    {
        errno = ret;
        ERR("pthread_mutex_unlock");
    }
}

int main(int argc, char* argv[])
{
    float a = -1.0f;
    float b = 1.0f;
    int N = 1000;

    if (argc > 1)
        a = strtof(argv[1], NULL);
    if (argc > 2)
        b = strtof(argv[2], NULL);
    if (argc > 3)
        N = atoi(argv[3]);

    usage(argv);

    if (N <= 0 || a >= b)
    {
        fprintf(stderr, "Invalid arguments.\n");
        return EXIT_FAILURE;
    }

    srand((unsigned int)(time(NULL) ^ getpid()));

    /*
        ============================================================
        STAGE 1
        ------------------------------------------------------------
        Use a named shared memory object for inter-process cooperation.
        Prepare a shared memory structure containing a process counter
        protected by a shared mutex.
        ============================================================
    */

    /*
        We also use a named semaphore to protect initialization.
        The task explicitly asks for it to avoid race conditions between
        creation and initialization.
    */
    sem_t* init_sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (init_sem == SEM_FAILED)
        ERR("sem_open");

    if (sem_wait(init_sem) < 0)
        ERR("sem_wait");

    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0)
        ERR("shm_open");

    if (ftruncate(shm_fd, sizeof(shared_data_t)) < 0)
        ERR("ftruncate");

    shared_data_t* shm = mmap(NULL, sizeof(shared_data_t),
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED)
        ERR("mmap");

    if (close(shm_fd) < 0)
        ERR("close");

    /*
        ============================================================
        STAGE 2
        ------------------------------------------------------------
        Initialize shared memory correctly.
        Use named semaphore to avoid init race.
        Increase process counter on startup.
        Print process count, sleep 2 seconds, terminate.
        Last process destroys shared memory.
        ============================================================
    */

    if (!shm->initialized)
    {
        pthread_mutexattr_t attr;
        if (pthread_mutexattr_init(&attr) != 0)
            ERR("pthread_mutexattr_init");

        /*
            Shared mutex: different processes must use it.
            This is exactly the rule from the synchronization materials.
        */
        if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
            ERR("pthread_mutexattr_setpshared");

        /*
            STAGE 4 requirement:
            robust mutex, so we can recover if a process dies
            while holding it outside initialization.
        */
        if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0)
            ERR("pthread_mutexattr_setrobust");

        if (pthread_mutex_init(&shm->mtx, &attr) != 0)
            ERR("pthread_mutex_init");

        if (pthread_mutexattr_destroy(&attr) != 0)
            ERR("pthread_mutexattr_destroy");

        shm->process_count = 0;
        shm->total_points = 0;
        shm->hit_points = 0;
        shm->a = a;
        shm->b = b;
        shm->N = N;
        shm->initialized = 1;
    }

    shm->process_count++;

    /*
        If someone joins later with different parameters, that would make
        the common approximation meaningless. The task warns about that.
        So we simply check consistency and use the shared values as truth.
    */
    if (shm->a != a || shm->b != b || shm->N != N)
    {
        fprintf(stderr, "Process joined with different parameters. Shared values are: a=%f b=%f N=%d\n",
                shm->a, shm->b, shm->N);
        if (sem_post(init_sem) < 0)
            ERR("sem_post");
        if (munmap(shm, sizeof(shared_data_t)) < 0)
            ERR("munmap");
        if (sem_close(init_sem) < 0)
            ERR("sem_close");
        return EXIT_FAILURE;
    }

    uint64_t current_count = shm->process_count;

    if (sem_post(init_sem) < 0)
        ERR("sem_post");

    printf("[%d] Collaborating processes: %llu\n",
           getpid(), (unsigned long long)current_count);

    /*
        ============================================================
        STAGE 3
        ------------------------------------------------------------
        Implement three batches of N Monte Carlo evaluations.
        Extend shared memory with total/hit counters.
        After each batch, update counters and print their status.
        After three iterations, terminate with last-process cleanup.
        ============================================================
    */

    /*
        ============================================================
        STAGE 4
        ------------------------------------------------------------
        Handle SIGINT.
        Continue computing until signal arrives.
        Finish current batch, then stop.
        If last process detaches, print final result.
        Robust mutex recovery when owner dies.
        Use random_death_lock outside initialization.
        ============================================================
    */

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigint_handler;
    if (sigaction(SIGINT, &act, NULL) < 0)
        ERR("sigaction");

    /*
        Stage 2 alone wanted sleep(2) and exit.
        But since the final program includes later stages,
        we continue into computation phase.
    */

    /*
        In final version we follow Stage 4 behavior:
        keep computing until SIGINT.
        That is the later stage, so it overrides the earlier
        "exactly three batches" behavior.
    */
    while (keep_running)
    {
        int local_hits = randomize_points(shm->N, shm->a, shm->b);

        /*
            Lock shared state.
            In Stage 4 we must use random_death_lock when locking any mutex
            outside initialization. We also must recover robustly.
        */
        robust_lock_with_recovery(shm);

        shm->total_points += (uint64_t)shm->N;
        shm->hit_points += (uint64_t)local_hits;

        printf("[%d] total=%llu hit=%llu processes=%llu\n",
               getpid(),
               (unsigned long long)shm->total_points,
               (unsigned long long)shm->hit_points,
               (unsigned long long)shm->process_count);

        robust_unlock(shm);

        /*
            If SIGINT arrived during current batch,
            we finish this batch and do not start another one.
        */
        if (!keep_running)
            break;
    }

    /*
        Detach logic:
        - decrement process count
        - if this process is last, print final result and destroy objects
    */
    robust_lock_with_recovery(shm);

    if (shm->process_count == 0)
    {
        /*
            This should not happen in normal flow, but if robust recovery
            already decremented counts due to dead owners, we still guard.
        */
        robust_unlock(shm);
        if (munmap(shm, sizeof(shared_data_t)) < 0)
            ERR("munmap");
        if (sem_close(init_sem) < 0)
            ERR("sem_close");
        return EXIT_SUCCESS;
    }

    shm->process_count--;

    int am_last = (shm->process_count == 0);

    uint64_t total_points = shm->total_points;
    uint64_t hit_points = shm->hit_points;
    float final_a = shm->a;
    float final_b = shm->b;

    robust_unlock(shm);

    if (am_last)
    {
        if (total_points > 0)
        {
            double result = summarize_calculations(total_points, hit_points, final_a, final_b);
            printf("[%d] Final approximation: %.12f\n", getpid(), result);
        }
        else
        {
            printf("[%d] No points were processed.\n", getpid());
        }

        if (shm_unlink(SHM_NAME) < 0)
            ERR("shm_unlink");

        if (sem_unlink(SEM_NAME) < 0)
            ERR("sem_unlink");
    }

    if (munmap(shm, sizeof(shared_data_t)) < 0)
        ERR("munmap");

    if (sem_close(init_sem) < 0)
        ERR("sem_close");

    return EXIT_SUCCESS;
}