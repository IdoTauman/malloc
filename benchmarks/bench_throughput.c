#include "my_malloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_THREADS 8
#define NUM_SLOTS 128
#define ITERATIONS_PER_THREAD 100000000
#define MAX_ALLOCATION_SIZE 4096

typedef struct {
    void *ptr;
    size_t size;
    unsigned char pattern;
} Slot;

typedef struct {
    int id;
    size_t iterations;
    uint32_t rng_state;
    pthread_barrier_t *start_barrier;
    size_t operations;
    int failed;
} ThreadArgs;

typedef struct {
    int threads;
    double seconds;
    size_t operations;
    double throughput;
    double speedup;
    double efficiency;
} BenchmarkResult;

static uint32_t random_u32(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;
    return x;
}

static size_t random_size(uint32_t *state)
{
    return (size_t)(random_u32(state) % MAX_ALLOCATION_SIZE) + 1;
}

static unsigned char random_pattern(uint32_t *state)
{
    return (unsigned char)(random_u32(state) & 0xFFu);
}

static void cleanup_slots(Slot slots[NUM_SLOTS])
{
    for (size_t i = 0; i < NUM_SLOTS; ++i) {
        if (slots[i].ptr != NULL) {
            free(slots[i].ptr);
            slots[i].ptr = NULL;
        }
    }
}

static void *worker(void *arg)
{
    ThreadArgs *args = arg;
    Slot slots[NUM_SLOTS] = {0};

    uint32_t rng = args->rng_state;

    int barrier_result = pthread_barrier_wait(args->start_barrier);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        args->failed = 1;
        return NULL;
    }

    for (size_t i = 0; i < args->iterations; ++i) {
        size_t index = (size_t)(random_u32(&rng) % NUM_SLOTS);
        Slot *slot = &slots[index];
        uint32_t choice = random_u32(&rng) % 100;

        if (slot->ptr == NULL) {
            size_t size = random_size(&rng);
            unsigned char pattern = random_pattern(&rng);
            void *ptr;

            if (choice < 20) {
                ptr = calloc(size, 1);
                pattern = 0;
            } else {
                ptr = malloc(size);
            }

            if (ptr == NULL) {
                fprintf(stderr,
                        "ERROR: thread %d: allocation failed\n",
                        args->id);
                args->failed = 1;
                goto cleanup;
            }

            slot->ptr = ptr;
            slot->size = size;
            slot->pattern = pattern;

            ((unsigned char *)slot->ptr)[0] = pattern;
            ((unsigned char *)slot->ptr)[size - 1] = pattern;
            ++args->operations;
        } else if (choice < 35) {
            free(slot->ptr);
            slot->ptr = NULL;
            slot->size = 0;
            ++args->operations;
        } else if (choice < 75) {
            size_t new_size = random_size(&rng);
            unsigned char *new_ptr = realloc(slot->ptr, new_size);

            if (new_ptr == NULL) {
                fprintf(stderr,
                        "ERROR: thread %d: realloc failed\n",
                        args->id);
                args->failed = 1;
                goto cleanup;
            }

            /*
             * realloc must preserve the first byte when both the old
             * and new allocation sizes are nonzero. The last byte is
             * not necessarily a valid sentinel after shrinking.
             */
            if (new_ptr[0] != slot->pattern) {
                fprintf(stderr,
                        "ERROR: thread %d: realloc corrupted first byte\n",
                        args->id);
                slot->ptr = new_ptr;
                slot->size = new_size;
                args->failed = 1;
                goto cleanup;
            }

            /*
             * Touch the new last byte to exercise the resized region.
             * Do not check this byte on the next realloc: after a shrink,
             * the new last byte may not have been initialized previously.
             */
            new_ptr[new_size - 1] = slot->pattern;

            slot->ptr = new_ptr;
            slot->size = new_size;
            ++args->operations;
        } else {
            /*
             * Read the sentinel to keep the allocation in active use.
             */
            if (((unsigned char *)slot->ptr)[0] != slot->pattern) {
                fprintf(stderr,
                        "ERROR: thread %d: allocation corrupted first byte\n",
                        args->id);
                args->failed = 1;
                goto cleanup;
            }
        }
    }

cleanup:
    cleanup_slots(slots);
    return NULL;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static int run_benchmark(int num_threads, BenchmarkResult *result)
{
    pthread_t threads[MAX_THREADS];
    ThreadArgs args[MAX_THREADS];
    pthread_barrier_t barrier;
    struct timespec start;
    struct timespec end;
    size_t total_operations = 0;
    int created = 0;

    if (pthread_barrier_init(&barrier, NULL,
                             (unsigned int)num_threads + 1) != 0) {
        fprintf(stderr, "ERROR: could not initialize start barrier\n");
        return -1;
    }

    for (int i = 0; i < num_threads; ++i) {
        args[i].id = i;
        args[i].iterations = ITERATIONS_PER_THREAD;
        args[i].rng_state = 0x9E3779B9u ^ (uint32_t)(i + 1);
        args[i].start_barrier = &barrier;
        args[i].operations = 0;
        args[i].failed = 0;

        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "ERROR: could not create thread %d\n", i);

            /*
             * Workers already created are waiting at the barrier.
             * Terminate rather than deadlocking on a partial startup.
             */
            exit(EXIT_FAILURE);
        }

        ++created;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        fprintf(stderr, "ERROR: clock_gettime failed\n");
        exit(EXIT_FAILURE);
    }

    int barrier_result = pthread_barrier_wait(&barrier);
    if (barrier_result != 0 &&
        barrier_result != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "ERROR: main thread could not release barrier\n");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < created; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "ERROR: could not join thread %d\n", i);
            exit(EXIT_FAILURE);
        }

        total_operations += args[i].operations;

        if (args[i].failed) {
            fprintf(stderr, "ERROR: benchmark worker failed\n");
            pthread_barrier_destroy(&barrier);
            return -1;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        fprintf(stderr, "ERROR: clock_gettime failed\n");
        exit(EXIT_FAILURE);
    }

    if (pthread_barrier_destroy(&barrier) != 0) {
        fprintf(stderr, "ERROR: could not destroy start barrier\n");
        return -1;
    }

    result->threads = num_threads;
    result->seconds = elapsed_seconds(&start, &end);
    result->operations = total_operations;
    result->throughput = result->seconds > 0.0
                             ? (double)total_operations / result->seconds
                             : 0.0;

    return 0;
}

int main(void)
{
    static const int thread_counts[] = {1, 2, 4, 8};
    const size_t num_results =
        sizeof(thread_counts) / sizeof(thread_counts[0]);
    BenchmarkResult results[sizeof(thread_counts) /
                            sizeof(thread_counts[0])];

    printf("Allocator throughput benchmark\n");
    printf("Workload: %d iterations/thread, %d slots/thread\n",
           ITERATIONS_PER_THREAD, NUM_SLOTS);
    printf("Operations include malloc, calloc, realloc and free.\n");
    printf("Run without Valgrind for meaningful timing.\n\n");

    for (size_t i = 0; i < num_results; ++i) {
        if (run_benchmark(thread_counts[i], &results[i]) != 0) {
            return EXIT_FAILURE;
        }
    }

    double baseline_throughput = results[0].throughput;

    printf("%-8s %12s %14s %16s %12s %12s\n",
           "Threads", "Runtime (s)", "Operations",
           "Ops/sec", "Speedup", "Efficiency");
    printf("--------------------------------------------------------------------------\n");

    for (size_t i = 0; i < num_results; ++i) {
        results[i].speedup =
            baseline_throughput > 0.0
                ? results[i].throughput / baseline_throughput
                : 0.0;

        results[i].efficiency =
            results[i].speedup / (double)results[i].threads;

        printf("%-8d %12.4f %14zu %16.0f %11.2fx %11.1f%%\n",
               results[i].threads,
               results[i].seconds,
               results[i].operations,
               results[i].throughput,
               results[i].speedup,
               results[i].efficiency * 100.0);
    }

    printf("\nSpeedup = throughput with N threads / throughput with 1 thread.\n");
    printf("Efficiency = speedup / N threads.\n");

    return EXIT_SUCCESS;
}