#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <assert.h>

typedef struct {
    atomic_flag lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t *sl)
{
    atomic_flag_clear_explicit(&sl->lock, memory_order_relaxed);
}

static inline void spin_lock(spinlock_t *sl)
{
    while (atomic_flag_test_and_set_explicit(
        &sl->lock, memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}

static inline void spin_unlock(spinlock_t *sl)
{
    atomic_flag_clear_explicit(&sl->lock, memory_order_release);
}

typedef struct BlockHeader {
    union {
        size_t actual_size;
        size_t mapping_size;
    };
    uint32_t class_index;
    uint32_t is_free;
} BlockHeader;

static_assert(sizeof(BlockHeader) == 16,
              "Header must be 16 bytes");

/*
 * Store the freelist pointer inside the unused payload while a block is free.
 * Every size class has a capacity of at least 16 bytes (>= sizeof(void *)).
 */
static inline BlockHeader **block_next(BlockHeader *header)
{
    return (BlockHeader **)(header + 1);
}

#define MMAP_CHUNK_SIZE (64 * 1024)