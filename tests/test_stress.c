#include "my_malloc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALIGNMENT 16
#define NUM_SLOTS 256
#define RANDOM_ITERATIONS 50000
#define MAX_ALLOCATION_SIZE 8192
#define CALLOC_MMAP_THRESHOLD (32 * 1024)

typedef struct {
    unsigned char *ptr;
    size_t size;
    unsigned char pattern;
} Slot;

static uint32_t rng_state = 0x12345678;

static uint32_t random_u32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static size_t random_size(size_t max_size) {
    return (size_t)(random_u32() % max_size) + 1;
}

static void check_alignment(const void *ptr) {
    if (ptr != NULL && (uintptr_t)ptr % ALIGNMENT != 0) {
        fprintf(stderr, "FAIL: pointer %p is not %d-byte aligned\n",
                ptr, ALIGNMENT);
        exit(EXIT_FAILURE);
    }
}

static void check_pattern(
    const unsigned char *ptr,
    size_t size,
    unsigned char pattern,
    size_t index,
    const char *phase
) {
    for (size_t i = 0; i < size; i++) {
        if (ptr[i] != pattern) {
            fprintf(
                stderr,
                "FAIL: %s: data corruption at block %zu, offset %zu: "
                "expected 0x%02x, got 0x%02x (block size %zu)\n",
                phase,
                index,
                i,
                (unsigned int)pattern,
                (unsigned int)ptr[i],
                size
            );
            exit(EXIT_FAILURE);
        }
    }
}

static void test_size_boundaries(void) {
    static const size_t sizes[] = {
        1, 2, 7, 8, 15, 16, 17, 31, 32, 33,
        63, 64, 65, 255, 256, 257,
        4095, 4096, 4097,
        32767, 32768, 32769
    };

    puts("[START] allocation size boundaries");

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i];
        unsigned char pattern = (unsigned char)(i + 1);
        unsigned char *ptr = malloc(size);

        if (ptr == NULL) {
            fprintf(stderr, "FAIL: malloc(%zu) returned NULL\n", size);
            exit(EXIT_FAILURE);
        }

        check_alignment(ptr);
        memset(ptr, pattern, size);
        check_pattern(ptr, size, pattern, i, "size boundaries");
        free(ptr);
    }

    puts("[ OK  ] allocation size boundaries");
}

static void test_calloc_threshold(void) {
    static const size_t sizes[] = {
        CALLOC_MMAP_THRESHOLD - 1,
        CALLOC_MMAP_THRESHOLD,
        CALLOC_MMAP_THRESHOLD + 1
    };

    puts("[START] calloc threshold and zero initialization");

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i];
        unsigned char *ptr = calloc(1, size);

        if (ptr == NULL) {
            fprintf(stderr, "FAIL: calloc(1, %zu) returned NULL\n", size);
            exit(EXIT_FAILURE);
        }

        check_alignment(ptr);

        for (size_t j = 0; j < size; j++) {
            if (ptr[j] != 0) {
                fprintf(stderr,
                        "FAIL: calloc(%zu) returned nonzero byte at %zu\n",
                        size, j);
                exit(EXIT_FAILURE);
            }
        }

        memset(ptr, 0xA5, size);
        check_pattern(ptr, size, 0xA5, i, "calloc threshold");
        free(ptr);
    }

    puts("[ OK  ] calloc threshold and zero initialization");
}

static void test_overflow_and_failed_realloc(void) {
    static volatile size_t allocation_limit = SIZE_MAX;

    puts("[START] overflow and failed realloc");

    size_t huge = allocation_limit;

    if (calloc(huge, 2) != NULL) {
        fprintf(stderr, "FAIL: overflowing calloc should return NULL\n");
        exit(EXIT_FAILURE);
    }

    unsigned char *ptr = malloc(32);
    if (ptr == NULL) {
        fprintf(stderr, "FAIL: malloc(32) returned NULL\n");
        exit(EXIT_FAILURE);
    }

    memset(ptr, 0x6B, 32);

    unsigned char *resized = realloc(ptr, huge);
    if (resized != NULL) {
        fprintf(stderr, "FAIL: realloc with overflowing size should fail\n");
        free(resized);
        exit(EXIT_FAILURE);
    }

    check_pattern(ptr, 32, 0x6B, 0, "failed realloc preservation");
    free(ptr);

    puts("[ OK  ] overflow and failed realloc");
}

static void test_many_live_allocations(void) {
    Slot slots[NUM_SLOTS];

    puts("[START] many simultaneously live allocations");

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        slots[i].size = random_size(MAX_ALLOCATION_SIZE);
        slots[i].pattern = (unsigned char)(i * 37 + 11);
        slots[i].ptr = malloc(slots[i].size);

        if (slots[i].ptr == NULL) {
            fprintf(stderr, "FAIL: malloc failed for block %zu\n", i);
            exit(EXIT_FAILURE);
        }

        check_alignment(slots[i].ptr);
        memset(slots[i].ptr, slots[i].pattern, slots[i].size);
    }

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        check_pattern(slots[i].ptr, slots[i].size,
                      slots[i].pattern, i, "before realloc");
    }

    for (size_t i = 0; i < NUM_SLOTS; i += 2) {
        size_t old_size = slots[i].size;
        size_t new_size = random_size(MAX_ALLOCATION_SIZE);
        unsigned char *old_ptr = slots[i].ptr;
        unsigned char *new_ptr = realloc(old_ptr, new_size);

        if (new_ptr == NULL) {
            fprintf(stderr, "FAIL: realloc failed for block %zu\n", i);
            exit(EXIT_FAILURE);
        }

        check_alignment(new_ptr);

        size_t preserved_size = old_size < new_size ? old_size : new_size;
        check_pattern(new_ptr, preserved_size, slots[i].pattern,
                      i, "immediately after realloc");

        if (new_size > old_size) {
            memset(new_ptr + old_size, slots[i].pattern,
                   new_size - old_size);
        }

        slots[i].ptr = new_ptr;
        slots[i].size = new_size;
    }

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        check_pattern(slots[i].ptr, slots[i].size,
                      slots[i].pattern, i, "after realloc");
    }

    size_t order[NUM_SLOTS];

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        order[i] = i;
    }

    for (size_t i = NUM_SLOTS - 1; i > 0; i--) {
        size_t j = (size_t)(random_u32() % (i + 1));
        size_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        free(slots[order[i]].ptr);
    }

    puts("[ OK  ] many simultaneously live allocations");
}

static void test_randomized_operations(void) {
    Slot slots[NUM_SLOTS] = {0};

    puts("[START] randomized allocation operations");

    for (size_t iteration = 0; iteration < RANDOM_ITERATIONS; iteration++) {
        size_t index = (size_t)(random_u32() % NUM_SLOTS);
        Slot *slot = &slots[index];
        uint32_t operation = random_u32() % 3;

        if (slot->ptr == NULL) {
            size_t size = random_size(MAX_ALLOCATION_SIZE);
            unsigned char pattern = (unsigned char)(random_u32() & 0xFF);
            unsigned char *ptr = malloc(size);

            if (ptr == NULL) {
                fprintf(stderr,
                        "FAIL: malloc failed at iteration %zu\n",
                        iteration);
                exit(EXIT_FAILURE);
            }

            check_alignment(ptr);
            memset(ptr, pattern, size);

            slot->ptr = ptr;
            slot->size = size;
            slot->pattern = pattern;
        } else if (operation == 0) {
            check_pattern(slot->ptr, slot->size, slot->pattern,
                          iteration, "randomized before free");
            free(slot->ptr);
            slot->ptr = NULL;
            slot->size = 0;
        } else if (operation == 1) {
            check_pattern(slot->ptr, slot->size, slot->pattern,
                          iteration, "randomized before realloc");

            size_t old_size = slot->size;
            size_t new_size = random_size(MAX_ALLOCATION_SIZE);
            unsigned char *new_ptr = realloc(slot->ptr, new_size);

            if (new_ptr == NULL) {
                fprintf(stderr,
                        "FAIL: realloc failed at iteration %zu\n",
                        iteration);
                exit(EXIT_FAILURE);
            }

            check_alignment(new_ptr);

            size_t preserved_size =
                old_size < new_size ? old_size : new_size;

            check_pattern(new_ptr, preserved_size, slot->pattern,
                          iteration, "randomized immediately after realloc");

            if (new_size > old_size) {
                memset(new_ptr + old_size, slot->pattern,
                       new_size - old_size);
            }

            slot->ptr = new_ptr;
            slot->size = new_size;
        } else {
            check_pattern(slot->ptr, slot->size, slot->pattern,
                          iteration, "randomized verification");
        }
    }

    for (size_t i = 0; i < NUM_SLOTS; i++) {
        if (slots[i].ptr != NULL) {
            check_pattern(slots[i].ptr, slots[i].size, slots[i].pattern,
                          i, "randomized final verification");
            free(slots[i].ptr);
        }
    }

    puts("[ OK  ] randomized allocation operations");
}

int main(void) {
    puts("test_stress: starting single-threaded allocator tests");

    test_size_boundaries();
    test_calloc_threshold();
    test_overflow_and_failed_realloc();
    test_many_live_allocations();
    test_randomized_operations();

    puts("test_stress: all tests passed");
    return EXIT_SUCCESS;
}