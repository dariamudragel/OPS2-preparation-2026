#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
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

#define BYTE_VALUES 256

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)

/*
    We store one histogram per child process in shared memory.

    Why this design?
    Instead of making all children update one common array directly,
    each child writes only to its own row:
        shared_results[child_id][byte_value]

    This is simpler and safer for a beginner:
    - no race conditions between children
    - no mutexes needed
    - parent combines results after all children finish

    This follows the same good idea from the shared-memory materials:
    if each process writes to its own dedicated memory area,
    synchronization may be unnecessary. 
*/
typedef struct
{
    int failed; /* not strictly required, but useful if we want shared failure info */
    uint64_t counts[]; /* flexible array: n_children * 256 counters */
} shared_data_t;

void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s file n\n", progname);
    fprintf(stderr, "  file - input file to analyze\n");
    fprintf(stderr, "  n    - number of child processes, n >= 1\n");
    exit(EXIT_FAILURE);
}

/*
    Helper: return pointer to the row belonging to one child.
*/
uint64_t *child_row(shared_data_t *shared, int child_id)
{
    return shared->counts + ((size_t)child_id * BYTE_VALUES);
}

/*
    Prints file contents to standard output.

    Stage 1 explicitly forbids streams and read() for file processing.
    So here we use write() to stdout, and the file itself is processed through mmap().

    We do NOT use fopen/fread/fgets for reading the file.
*/
void print_file_contents(const unsigned char *data, size_t size)
{
    size_t written_total = 0;

    while (written_total < size)
    {
        ssize_t written = write(STDOUT_FILENO, data + written_total, size - written_total);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            ERR("write");
        }
        written_total += (size_t)written;
    }

    if (size > 0 && data[size - 1] != '\n')
    {
        if (write(STDOUT_FILENO, "\n", 1) < 0)
            ERR("write");
    }
}

/*
    Counts characters in [begin, end) range of the mapped file.
    Results are written into local_counts[256].
*/
void count_range(const unsigned char *data, size_t begin, size_t end, uint64_t local_counts[BYTE_VALUES])
{
    for (size_t i = begin; i < end; i++)
    {
        local_counts[data[i]]++;
    }
}

/*
    Prints summary for all bytes that appeared at least once.

    I print:
    - printable ASCII characters in readable form
    - others as hex byte values

    Example:
      'a' : 12
      0x0A : 3
*/
void print_summary(const uint64_t total_counts[BYTE_VALUES])
{
    printf("\nSummary of character occurrences:\n");

    for (int i = 0; i < BYTE_VALUES; i++)
    {
        if (total_counts[i] == 0)
            continue;

        if (i >= 32 && i <= 126)
        {
            printf("'%c' : %llu\n", i, (unsigned long long)total_counts[i]);
        }
        else
        {
            printf("0x%02X : %llu\n", i, (unsigned long long)total_counts[i]);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 3)
        usage(argv[0]);

    const char *filename = argv[1];
    int n_children = atoi(argv[2]);

    if (n_children < 1)
        usage(argv[0]);

    srand((unsigned int)(time(NULL) ^ getpid()));

    /*
        ============================================================
        STAGE 1
        ------------------------------------------------------------
        Open the file using mmap in the parent process.
        Print its contents to standard output.
        Streams and read() are forbidden.
        ============================================================
    */

    int fd = open(filename, O_RDONLY);
    if (fd < 0)
        ERR("open");

    struct stat st;
    if (fstat(fd, &st) < 0)
        ERR("fstat");

    if (st.st_size < 0)
    {
        fprintf(stderr, "Negative file size is invalid.\n");
        exit(EXIT_FAILURE);
    }

    size_t file_size = (size_t)st.st_size;

    /*
        mmap() of size 0 is invalid on many systems,
        so for empty file we handle printing/counting carefully.
    */
    unsigned char *mapped_parent = NULL;

    if (file_size > 0)
    {
        mapped_parent = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped_parent == MAP_FAILED)
            ERR("mmap");
    }

    /*
        Print file contents exactly as required in Stage 1.
    */
    if (file_size > 0)
        print_file_contents(mapped_parent, file_size);
    else
        printf("(empty file)\n");

    if (close(fd) < 0)
        ERR("close");

    /*
        ============================================================
        STAGE 2
        ------------------------------------------------------------
        Implement character counting logic.
        At the end, print how many times each character appeared.
        ============================================================

        We already implement the counting function above.
        In the final solution, the parent prints the summary
        after child processes finish (because later stages require that).
    */

    /*
        ============================================================
        STAGE 3
        ------------------------------------------------------------
        Distribute work across N child processes.
        Move file opening to the child process.
        Each child counts independently.
        Use shared memory to send results to the parent.
        Parent prints summary after all children finish.
        ============================================================
    */

    /*
        Shared memory layout:
        - 1 int-like field for failure info
        - n_children * 256 counters

        We use anonymous shared memory because parent and children
        are related by fork() and only need a shared region for results.
    */
    size_t shm_size = sizeof(shared_data_t) + (size_t)n_children * BYTE_VALUES * sizeof(uint64_t);

    shared_data_t *shared = mmap(NULL,
                                 shm_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS,
                                 -1,
                                 0);
    if (shared == MAP_FAILED)
        ERR("mmap shared");

    shared->failed = 0;
    memset(shared->counts, 0, (size_t)n_children * BYTE_VALUES * sizeof(uint64_t));

    /*
        Divide the file into N ranges.
        Child i processes:
            [ i*size/N , (i+1)*size/N )
        This covers the whole file without overlaps and without gaps.
    */
    for (int i = 0; i < n_children; i++)
    {
        pid_t pid = fork();

        if (pid < 0)
            ERR("fork");

        if (pid == 0)
        {
            /*
                ---------------- CHILD PROCESS ----------------

                Stage 3 says file opening should be moved to the child process.
                So each child opens and mmaps the file independently.
            */

            int child_fd = open(filename, O_RDONLY);
            if (child_fd < 0)
                ERR("open in child");

            struct stat child_st;
            if (fstat(child_fd, &child_st) < 0)
                ERR("fstat in child");

            size_t child_file_size = (size_t)child_st.st_size;
            unsigned char *child_map = NULL;

            if (child_file_size > 0)
            {
                child_map = mmap(NULL, child_file_size, PROT_READ, MAP_PRIVATE, child_fd, 0);
                if (child_map == MAP_FAILED)
                    ERR("mmap in child");
            }

            if (close(child_fd) < 0)
                ERR("close in child");

            size_t begin = ((size_t)i * child_file_size) / (size_t)n_children;
            size_t end = ((size_t)(i + 1) * child_file_size) / (size_t)n_children;

            uint64_t local_counts[BYTE_VALUES];
            memset(local_counts, 0, sizeof(local_counts));

            count_range(child_map, begin, end, local_counts);

            /*
                ============================================================
                STAGE 4
                ------------------------------------------------------------
                Handle unexpected child termination.
                Each child has 3% chance to abort when reporting results.
                If any child dies unexpectedly, parent skips summary and
                prints that computation failed.
                ============================================================
            */

            /*
                The task says: 3% chance of sudden termination
                WHEN REPORTING RESULTS TO THE PARENT.

                So we place the random abort right before copying local_counts
                into shared memory.
            */
            if ((rand() % 100) < 3)
            {
                abort();
            }

            /*
                "Reporting results" = copying local results to shared memory,
                in the row reserved for this child.
            */
            uint64_t *row = child_row(shared, i);
            memcpy(row, local_counts, sizeof(local_counts));

            if (child_file_size > 0)
            {
                if (munmap(child_map, child_file_size) < 0)
                    ERR("munmap in child");
            }

            if (file_size > 0 && mapped_parent != NULL)
            {
                /*
                    Child inherited parent's mapping through fork,
                    but does not need it.
                    Not required to unmap here for correctness,
                    so we just ignore it to keep the child logic simple.
                */
            }

            exit(EXIT_SUCCESS);
        }
    }

    /*
        Parent waits for all children.

        If ANY child:
        - exits بسبب signal
        - aborts
        - or exits with non-zero code

        then we treat the whole computation as failed.
    */
    int computation_failed = 0;

    for (int i = 0; i < n_children; i++)
    {
        int status;
        pid_t pid = wait(&status);

        if (pid < 0)
            ERR("wait");

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            computation_failed = 1;
        }
    }

    if (computation_failed)
    {
        printf("\nComputation failed.\n");
    }
    else
    {
        /*
            Parent combines all child histograms into one final summary.
        */
        uint64_t total_counts[BYTE_VALUES];
        memset(total_counts, 0, sizeof(total_counts));

        for (int child = 0; child < n_children; child++)
        {
            uint64_t *row = child_row(shared, child);
            for (int b = 0; b < BYTE_VALUES; b++)
            {
                total_counts[b] += row[b];
            }
        }

        print_summary(total_counts);
    }

    /*
        Cleanup.
    */
    if (file_size > 0)
    {
        if (munmap(mapped_parent, file_size) < 0)
            ERR("munmap parent");
    }

    if (munmap(shared, shm_size) < 0)
        ERR("munmap shared");

    return EXIT_SUCCESS;
}