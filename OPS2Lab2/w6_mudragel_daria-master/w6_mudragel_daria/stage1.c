#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#define SHOP_FILENAME "./shop"
#define MIN_SHELVES 8
#define MAX_SHELVES 256
#define MIN_WORKERS 1
#define MAX_WORKERS 64

/* 
   This macro prints the line of the error, prints the system error message,
   and ends the program.
   We keep it because in operating systems tasks we should always check
   system calls like open, ftruncate, mmap, msync, munmap, close, etc.
*/
#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        exit(EXIT_FAILURE);                             \
    } while (0)

/*
   Prints correct program usage.
   Stage 1 still receives BOTH parameters, even though M (workers count)
   will only become useful from Stage 2.
*/
void usage(char *program_name)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "\t%s n m\n", program_name);
    fprintf(stderr, "\t  n - number of products/shelves, %d <= n <= %d\n", MIN_SHELVES, MAX_SHELVES);
    fprintf(stderr, "\t  m - number of workers, %d <= m <= %d\n", MIN_WORKERS, MAX_WORKERS);
    exit(EXIT_FAILURE);
}

/*
   Simple helper to swap two integers.
   This is already used by shuffle().
*/
void swap(int *x, int *y)
{
    int tmp = *y;
    *y = *x;
    *x = tmp;
}

/*
   Provided shuffle logic:
   fill the array first with 1..N, then mix it randomly.
*/
void shuffle(int *array, int n)
{
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap(&array[i], &array[j]);
    }
}

/*
   Provided print method:
   Stage 1 requires printing the final shelf contents before exit.
*/
void print_array(int *array, int n)
{
    for (int i = 0; i < n; ++i)
    {
        printf("%3d ", array[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    /* 
       We initialize random generator once in the main process,
       because Stage 1 needs randomness only for shuffle().
    */
    srand((unsigned int)time(NULL));

    /* 
       The task says the program takes exactly two parameters: N and M.
       Even though M is not used yet in Stage 1, we still validate it,
       because the task definition already requires both parameters.
    */
    if (argc != 3)
        usage(argv[0]);

    int N = atoi(argv[1]); /* number of shelves/products */
    int M = atoi(argv[2]); /* number of workers, used later in Stage 2 */

    /*
       Check constraints from the task.
       N must be between 8 and 256.
       M must be between 1 and 64.
    */
    if (N < MIN_SHELVES || N > MAX_SHELVES || M < MIN_WORKERS || M > MAX_WORKERS)
        usage(argv[0]);

    /*
       STAGE 1 CORE IDEA:
       The shop array must live inside a FILE that we map with mmap().

       We open/create the file:
       - O_CREAT  -> create if it does not exist
       - O_TRUNC  -> clear old contents
       - O_RDWR   -> we need both reading and writing
       
       This follows the file-backed mmap approach from the L6 material. 
    */
    int fd = open(SHOP_FILENAME, O_CREAT | O_TRUNC | O_RDWR, 0666);
    if (fd == -1)
        ERR("open");

    /*
       Very important mmap practice from L6:
       before mapping a file for writing, set its size with ftruncate().
       
       Why?
       Because a newly created/truncated file may be size 0.
       We need enough space for N integers.
    */
    if (ftruncate(fd, N * (int)sizeof(int)) == -1)
        ERR("ftruncate");

    /*
       Now map the file into memory.

       - NULL        -> let OS choose address
       - N*sizeof(int) -> size of mapped area
       - PROT_READ | PROT_WRITE -> we will read and modify shelves
       - MAP_SHARED  -> changes in memory should affect the mapped file

       Another important L6 rule:
       mmap() must be checked against MAP_FAILED, not NULL.
    */
    int *shop = mmap(NULL, N * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shop == MAP_FAILED)
        ERR("mmap");

    /*
       After successful mmap, the descriptor itself is no longer needed
       for normal access to the mapped memory, so we can close it.
       The mapping still exists.
    */
    if (close(fd) == -1)
        ERR("close");

    /*
       Initialize shelves with consecutive values:
       shelf 0 -> product 1
       shelf 1 -> product 2
       ...
       shelf N-1 -> product N
    */
    for (int i = 0; i < N; i++)
    {
        shop[i] = i + 1;
    }

    /*
       Shuffle the products using the provided method.
       After this, the shop is disordered, which matches the task story.
    */
    shuffle(shop, N);

    /*
       Stage 1 says:
       "Before exiting the program, print the contents of the product array
       using the provided print array method."
    */
    print_array(shop, N);

    /*
       Good mmap practice from L6:
       if this mapping represents a file and we want to be sure changes
       are written back, call msync().
    */
    if (msync(shop, N * sizeof(int), MS_SYNC) == -1)
        ERR("msync");

    /*
       Free the mapped memory region when finished.
    */
    if (munmap(shop, N * sizeof(int)) == -1)
        ERR("munmap");

    return EXIT_SUCCESS;
}