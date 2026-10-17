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
    struct BlockHeader *next;
    size_t size;
    size_t actual_size;
    size_t mapping_size;
    size_t class_index;
    bool is_free;
    uint8_t padding[7];
} BlockHeader;

static_assert(sizeof(BlockHeader) % 16 == 0,
              "Header must be 16 byte aligned");

#define MMAP_CHUNK_SIZE (64 * 1024)