#include "my_malloc.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#define NUM_THREADS 8
#define NUM_SLOTS 128
#define RANDOM_ITERATIONS 20000
#define MAX_ALLOCATION_SIZE 8192
#define ALIGNMENT 16

typedef struct {
    unsigned char *ptr;
    size_t size;
    unsigned char pattern;
} Slot;

typedef struct {
    size_t thread_id;
    uint32_t rng_state;
} ThreadArgs;

static uint32_t random_u32(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static size_t random_size(uint32_t *state) {
    return (size_t)(random_u32(state) % MAX_ALLOCATION_SIZE) + 1;
}

static unsigned char random_pattern(uint32_t *state) {
    return (unsigned char)(random_u32(state) & 0xFF);
}

static void fail(size_t thread_id, const char *message) {
    fprintf(stderr, "FAIL: thread %zu: %s\n", thread_id, message);
    exit(EXIT_FAILURE);
}

static void check_alignment(
    const void *ptr,
    size_t thread_id
) {
    if (ptr == NULL || (uintptr_t)ptr % ALIGNMENT != 0) {
        fprintf(
            stderr,
            "FAIL: thread %zu: pointer %p is not %d-byte aligned\n",
            thread_id,
            ptr,
            ALIGNMENT
        );
        exit(EXIT_FAILURE);
    }
}

static void check_pattern(
    const unsigned char *ptr,
    size_t size,
    unsigned char pattern,
    size_t thread_id,
    size_t slot_id
) {
    for (size_t i = 0; i < size; i++) {
        if (ptr[i] != pattern) {
            fprintf(
                stderr,
                "FAIL: thread %zu, slot %zu, offset %zu: "
                "expected 0x%02x, got 0x%02x (size %zu)\n",
                thread_id,
                slot_id,
                i,
                (unsigned int)pattern,
                (unsigned int)ptr[i],
                size
            );
            exit(EXIT_FAILURE);
        }
    }
}

static void check_slot(
    const Slot *slot,
    size_t thread_id,
    size_t slot_id
) {
    if (slot->ptr == NULL) {
        fail(thread_id, "attempted to verify an empty slot");
    }

    check_alignment(slot->ptr, thread_id);
    check_pattern(
        slot->ptr,
        slot->size,
        slot->pattern,
        thread_id,
        slot_id
    );
}

static void test_calloc(size_t thread_id, uint32_t *state) {
    size_t size = random_size(state);
    unsigned char *ptr = calloc(size, 1);

    if (ptr == NULL) {
        fail(thread_id, "calloc returned NULL");
    }

    check_alignment(ptr, thread_id);

    for (size_t i = 0; i < size; i++) {
        if (ptr[i] != 0) {
            fprintf(
                stderr,
                "FAIL: thread %zu: calloc returned nonzero byte "
                "at offset %zu (size %zu)\n",
                thread_id,
                i,
                size
            );
            exit(EXIT_FAILURE);
        }
    }

    memset(ptr, 0xA5, size);
    check_pattern(ptr, size, 0xA5, thread_id, SIZE_MAX);
    free(ptr);
}

static void *thread_worker(void *arg) {
    ThreadArgs *args = arg;
    size_t thread_id = args->thread_id;
    uint32_t state = args->rng_state;
    Slot slots[NUM_SLOTS] = {0};

    /*
     * Phase 1: keep many allocations alive simultaneously.
     */
    for (size_t i = 0; i < NUM_SLOTS; i++) {
        slots[i].size = random_size(&state);
        slots[i].pattern = random_pattern(&state);
        slots[i].ptr = malloc(slots[i].size);

        if (slots[i].ptr == NULL) {
            fail(thread_id, "malloc failed in initial allocation phase");
        }

        check_alignment(slots[i].ptr, thread_id);
        memset(slots[i].ptr, slots[i].pattern, slots[i].size);
    }

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        check_slot(&slots[i], thread_id, i);
    }

    /*
     * Phase 2: realloc alternating slots while other allocations
     * remain live. Verify that realloc preserves existing contents.
     */
    for (size_t i = 0; i < NUM_SLOTS; i += 2) {
        size_t old_size = slots[i].size;
        size_t new_size = random_size(&state);
        unsigned char *new_ptr = realloc(slots[i].ptr, new_size);

        if (new_ptr == NULL) {
            fail(thread_id, "realloc failed in initial realloc phase");
        }

        check_alignment(new_ptr, thread_id);

        size_t preserved_size =
            old_size < new_size ? old_size : new_size;

        check_pattern(
            new_ptr,
            preserved_size,
            slots[i].pattern,
            thread_id,
            i
        );

        if (new_size > old_size) {
            memset(
                new_ptr + old_size,
                slots[i].pattern,
                new_size - old_size
            );
        }

        slots[i].ptr = new_ptr;
        slots[i].size = new_size;
    }

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        check_slot(&slots[i], thread_id, i);
    }

    /*
     * Phase 3: randomized allocations, calloc, realloc, verification,
     * and frees. Every thread owns its slots exclusively.
     */
    for (size_t iteration = 0; iteration < RANDOM_ITERATIONS; iteration++) {
        size_t index = (size_t)(random_u32(&state) % NUM_SLOTS);
        Slot *slot = &slots[index];
        uint32_t operation = random_u32(&state) % 4;

        if (slot->ptr == NULL) {
            size_t size = random_size(&state);
            unsigned char pattern = random_pattern(&state);
            unsigned char *ptr;

            if (operation == 0) {
                ptr = calloc(size, 1);

                if (ptr == NULL) {
                    fail(thread_id, "calloc failed in randomized phase");
                }

                check_alignment(ptr, thread_id);

                for (size_t i = 0; i < size; i++) {
                    if (ptr[i] != 0) {
                        fail(thread_id, "calloc did not zero memory");
                    }
                }
            } else {
                ptr = malloc(size);

                if (ptr == NULL) {
                    fail(thread_id, "malloc failed in randomized phase");
                }

                check_alignment(ptr, thread_id);
            }

            memset(ptr, pattern, size);

            slot->ptr = ptr;
            slot->size = size;
            slot->pattern = pattern;
            continue;
        }

        check_slot(slot, thread_id, index);

        if (operation == 0) {
            free(slot->ptr);
            slot->ptr = NULL;
            slot->size = 0;
        } else if (operation == 1) {
            size_t old_size = slot->size;
            size_t new_size = random_size(&state);
            unsigned char *new_ptr = realloc(slot->ptr, new_size);

            if (new_ptr == NULL) {
                fail(thread_id, "realloc failed in randomized phase");
            }

            check_alignment(new_ptr, thread_id);

            size_t preserved_size =
                old_size < new_size ? old_size : new_size;

            check_pattern(
                new_ptr,
                preserved_size,
                slot->pattern,
                thread_id,
                index
            );

            if (new_size > old_size) {
                memset(
                    new_ptr + old_size,
                    slot->pattern,
                    new_size - old_size
                );
            }

            slot->ptr = new_ptr;
            slot->size = new_size;
        } else if (operation == 2) {
            test_calloc(thread_id, &state);
        } else {
            check_slot(slot, thread_id, index);
        }
    }

    /*
     * Phase 4: final verification and cleanup.
     */
    for (size_t i = 0; i < NUM_SLOTS; i++) {
        if (slots[i].ptr != NULL) {
            check_slot(&slots[i], thread_id, i);
            free(slots[i].ptr);
            slots[i].ptr = NULL;
        }
    }

    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    ThreadArgs args[NUM_THREADS];
    size_t created = 0;

    puts("test_threads: starting strict multithreaded allocator tests");
    printf("Configuration: %d threads, %d slots/thread, "
           "%d randomized iterations/thread\n",
           NUM_THREADS, NUM_SLOTS, RANDOM_ITERATIONS);

    for (size_t i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].rng_state = (uint32_t)(0x12345678u + i * 0x9E3779B9u);

        if (args[i].rng_state == 0) {
            args[i].rng_state = 1;
        }

        int error = pthread_create(
            &threads[i],
            NULL,
            thread_worker,
            &args[i]
        );

        if (error != 0) {
            fprintf(
                stderr,
                "FAIL: pthread_create failed for thread %zu: error %d\n",
                i,
                error
            );

            for (size_t j = 0; j < created; j++) {
                (void)pthread_join(threads[j], NULL);
            }

            return EXIT_FAILURE;
        }

        created++;
    }

    for (size_t i = 0; i < created; i++) {
        int error = pthread_join(threads[i], NULL);

        if (error != 0) {
            fprintf(
                stderr,
                "FAIL: pthread_join failed for thread %zu: error %d\n",
                i,
                error
            );
            return EXIT_FAILURE;
        }
    }

    puts("test_threads: all tests passed");
    return EXIT_SUCCESS;
}