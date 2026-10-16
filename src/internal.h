#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <assert.h>

typedef struct {
    atomic_flag lock;
} spinlock_t;

static inline void spinlock_init(spinlock_t *sl) {
    atomic_flag_clear_explicit(&sl->lock, memory_order_relaxed);
}

static inline void spin_lock(spinlock_t *sl) {
    while (atomic_flag_test_and_set_explicit(&sl->lock, memory_order_acquire)) {
        #if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
        #endif
    }
}

static inline void spin_unlock(spinlock_t *sl) {
    atomic_flag_clear_explicit(&sl->lock, memory_order_release);
}

typedef struct BlockHeader {
    struct BlockHeader *next; // 8 bytes
    size_t size; // 8 bytes
    size_t actual_size; // 8 bytes
    spinlock_t lock; // 1 byte
    bool is_free; // 1 byte
    uint8_t padding[6]; // padding to 32 bytes (16 byte alignment)
} BlockHeader;

static_assert(sizeof(BlockHeader) % 16 == 0, "Header must be 16 byte aligned");
#define MMAP_CHUNK_SIZE (64 * 1024)