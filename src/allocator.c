#include "internal.h"

#include <pthread.h>
#include <sys/mman.h>
#include <string.h>

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))
#define HEADER_SIZE sizeof(BlockHeader)

#define SMALL_CLASS_COUNT 32
#define CLASS_COUNT (SMALL_CLASS_COUNT + sizeof(size_t) * 8 - 10)
#define DIRECT_MMAP_THRESHOLD (MMAP_CHUNK_SIZE / 2)
#define PAGE_SIZE 4096

#define NO_CLASS SIZE_MAX

#define CACHE_BATCH_SIZE 16
#define CACHE_MAX_BLOCKS 32

typedef struct {
    BlockHeader *free_list;
    spinlock_t lock;
} SizeClass;

typedef struct {
    BlockHeader *free_list;
    size_t count;
} ThreadCache;

typedef struct {
    ThreadCache classes[CLASS_COUNT];
} ThreadCacheState;

static SizeClass size_classes[CLASS_COUNT];

/*
 * Thread-local pointer to the current thread's cache state.
 * The state itself is mmap-allocated so its lifetime can be managed
 * by the pthread-specific-data destructor.
 */
static _Thread_local ThreadCacheState *thread_cache_state;

static pthread_key_t cache_key;
static pthread_once_t cache_key_once = PTHREAD_ONCE_INIT;
static int cache_key_ready;

/*
 * Initialize class locks exactly once.
 */
static spinlock_t init_lock = { .lock = ATOMIC_FLAG_INIT };
static atomic_bool classes_initialized = ATOMIC_VAR_INIT(false);

static void ensure_classes_initialized(void) {
    if (atomic_load_explicit(&classes_initialized, memory_order_acquire)) {
        return;
    }

    spin_lock(&init_lock);

    if (!atomic_load_explicit(&classes_initialized, memory_order_relaxed)) {
        for (size_t i = 0; i < CLASS_COUNT; ++i) {
            size_classes[i].free_list = NULL;
            spinlock_init(&size_classes[i].lock);
        }

        atomic_store_explicit(
            &classes_initialized, true, memory_order_release);
    }

    spin_unlock(&init_lock);
}

/*
 * Classes:
 *   16, 32, 48, ..., 512 bytes
 *   1024, 2048, 4096, ... bytes
 */
static size_t class_capacity(size_t index) {
    if (index < SMALL_CLASS_COUNT) {
        return (index + 1) * ALIGNMENT;
    }

    return (size_t)1 << (index - SMALL_CLASS_COUNT + 10);
}

/*
 * Return NO_CLASS if the requested size cannot be represented by a class.
 */
static size_t size_to_class(size_t size) {
    if (size == 0) {
        return NO_CLASS;
    }

    if (size <= 512) {
        return (size - 1) / ALIGNMENT;
    }

    size_t index = SMALL_CLASS_COUNT;
    size_t capacity = 1024;

    while (capacity < size) {
        if (capacity > SIZE_MAX / 2) {
            return NO_CLASS;
        }

        capacity *= 2;
        ++index;
    }

    if (index >= CLASS_COUNT) {
        return NO_CLASS;
    }

    return index;
}

/*
 * Map a dedicated allocation. Large requests bypass size-class freelists
 * so that freeing them can release their mapping immediately.
 */
static BlockHeader *request_dedicated_mapping(size_t size) {
    if (size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }

    size_t aligned_size = ALIGN(size);

    if (aligned_size > SIZE_MAX - HEADER_SIZE) {
        return NULL;
    }

    size_t total_size = HEADER_SIZE + aligned_size;

    if (total_size > SIZE_MAX - (PAGE_SIZE - 1)) {
        return NULL;
    }

    size_t mapping_size =
        (total_size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    void *memory = mmap(NULL, mapping_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (memory == MAP_FAILED) {
        return NULL;
    }

    BlockHeader *header = memory;

    header->next = NULL;
    header->size = aligned_size;
    header->actual_size = size;
    header->mapping_size = mapping_size;
    header->class_index = NO_CLASS;
    header->is_free = false;

    return header;
}

/*
 * Add blocks from a new mmap chunk to one class's shared freelist.
 * The caller must hold that class's lock.
 */
static int refill_class(size_t index) {
    size_t capacity = class_capacity(index);
    size_t stride = HEADER_SIZE + capacity;

    if (stride > MMAP_CHUNK_SIZE) {
        return 0;
    }

    void *memory = mmap(NULL, MMAP_CHUNK_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (memory == MAP_FAILED) {
        return 0;
    }

    size_t block_count = MMAP_CHUNK_SIZE / stride;
    uint8_t *cursor = memory;

    for (size_t i = 0; i < block_count; ++i) {
        BlockHeader *header = (BlockHeader *)cursor;

        header->next = size_classes[index].free_list;
        header->size = capacity;
        header->actual_size = 0;
        header->mapping_size = 0;
        header->class_index = index;
        header->is_free = true;

        size_classes[index].free_list = header;
        cursor += stride;
    }

    return 1;
}

/*
 * Return all blocks in a thread's caches to the shared freelists
 * when that thread exits.
 */
static void thread_cache_destructor(void *arg) {
    ThreadCacheState *state = arg;

    if (state == NULL) {
        return;
    }

    /*
     * Clear the TLS pointer before releasing the state.
     */
    thread_cache_state = NULL;

    for (size_t i = 0; i < CLASS_COUNT; ++i) {
        ThreadCache *cache = &state->classes[i];

        if (cache->free_list == NULL) {
            continue;
        }

        SizeClass *class = &size_classes[i];

        spin_lock(&class->lock);

        BlockHeader *tail = cache->free_list;

        while (tail->next != NULL) {
            tail = tail->next;
        }

        tail->next = class->free_list;
        class->free_list = cache->free_list;

        spin_unlock(&class->lock);
    }

    munmap(state, sizeof(*state));
}

static void create_cache_key(void) {
    cache_key_ready =
        pthread_key_create(&cache_key, thread_cache_destructor) == 0;
}

/*
 * Lazily create per-thread cache state.
 */
static ThreadCacheState *get_thread_cache_state(void) {
    if (thread_cache_state != NULL) {
        return thread_cache_state;
    }

    pthread_once(&cache_key_once, create_cache_key);

    if (!cache_key_ready) {
        return NULL;
    }

    void *memory = mmap(NULL, sizeof(ThreadCacheState),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (memory == MAP_FAILED) {
        return NULL;
    }

    ThreadCacheState *state = memory;

    if (pthread_setspecific(cache_key, state) != 0) {
        munmap(state, sizeof(*state));
        return NULL;
    }

    thread_cache_state = state;
    return state;
}

/*
 * Refill the calling thread's cache with up to CACHE_BATCH_SIZE blocks
 * from the shared freelist.
 */
static int refill_thread_cache(size_t index) {
    ThreadCacheState *state = get_thread_cache_state();

    if (state == NULL) {
        return 0;
    }

    ThreadCache *cache = &state->classes[index];
    SizeClass *class = &size_classes[index];

    spin_lock(&class->lock);

    if (class->free_list == NULL && !refill_class(index)) {
        spin_unlock(&class->lock);
        return 0;
    }

    size_t moved = 0;

    while (class->free_list != NULL && moved < CACHE_BATCH_SIZE) {
        BlockHeader *header = class->free_list;
        class->free_list = header->next;

        header->next = cache->free_list;
        cache->free_list = header;

        ++cache->count;
        ++moved;
    }

    spin_unlock(&class->lock);

    return moved != 0;
}

/*
 * Return CACHE_BATCH_SIZE blocks from the calling thread's cache
 * to the shared freelist. The caller should have more than
 * CACHE_MAX_BLOCKS cached blocks.
 */
static void flush_thread_cache(size_t index) {
    ThreadCacheState *state = get_thread_cache_state();

    if (state == NULL) {
        return;
    }

    ThreadCache *cache = &state->classes[index];
    SizeClass *class = &size_classes[index];

    BlockHeader *batch = NULL;
    size_t moved = 0;

    while (cache->free_list != NULL && moved < CACHE_BATCH_SIZE) {
        BlockHeader *header = cache->free_list;
        cache->free_list = header->next;

        header->next = batch;
        batch = header;

        --cache->count;
        ++moved;
    }

    if (batch == NULL) {
        return;
    }

    spin_lock(&class->lock);

    BlockHeader *tail = batch;

    while (tail->next != NULL) {
        tail = tail->next;
    }

    tail->next = class->free_list;
    class->free_list = batch;

    spin_unlock(&class->lock);
}

static void *malloc_impl(size_t size) {
    if (size == 0 || size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }

    ensure_classes_initialized();

    size_t index = size_to_class(size);

    /*
     * Large blocks bypass the size-class caches.
     */
    if (index == NO_CLASS ||
        class_capacity(index) >= DIRECT_MMAP_THRESHOLD) {
        BlockHeader *header = request_dedicated_mapping(size);
        return header ? (void *)(header + 1) : NULL;
    }

    ThreadCacheState *state = get_thread_cache_state();

    if (state == NULL) {
        /*
         * If thread-cache setup fails, fall back to the shared freelist.
         */
        SizeClass *class = &size_classes[index];

        spin_lock(&class->lock);

        if (class->free_list == NULL && !refill_class(index)) {
            spin_unlock(&class->lock);
            return NULL;
        }

        BlockHeader *header = class->free_list;
        class->free_list = header->next;

        header->next = NULL;
        header->actual_size = size;
        header->is_free = false;

        spin_unlock(&class->lock);

        return (void *)(header + 1);
    }

    ThreadCache *cache = &state->classes[index];

    /*
     * Fast path: allocate from the local cache without a shared lock.
     */
    if (cache->free_list == NULL && !refill_thread_cache(index)) {
        return NULL;
    }

    BlockHeader *header = cache->free_list;
    cache->free_list = header->next;
    --cache->count;

    header->next = NULL;
    header->actual_size = size;
    header->is_free = false;

    return (void *)(header + 1);
}

static void free_impl(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    BlockHeader *header = (BlockHeader *)ptr - 1;

    /*
     * Dedicated mappings can be returned directly to the OS.
     */
    if (header->class_index == NO_CLASS) {
        size_t mapping_size = header->mapping_size;
        munmap(header, mapping_size);
        return;
    }

    size_t index = header->class_index;

    /*
     * Double-free protection for ordinary sequential misuse.
     * Concurrent or invalid frees remain undefined behavior.
     */
    if (header->is_free) {
        return;
    }

    ensure_classes_initialized();

    header->actual_size = 0;
    header->is_free = true;

    ThreadCacheState *state = get_thread_cache_state();

    if (state == NULL) {
        /*
         * If cache setup fails, return the block directly to the
         * shared freelist instead of losing it.
         */
        SizeClass *class = &size_classes[index];

        spin_lock(&class->lock);

        header->next = class->free_list;
        class->free_list = header;

        spin_unlock(&class->lock);
        return;
    }

    ThreadCache *cache = &state->classes[index];

    /*
     * Fast path: return the block to the freeing thread's local cache.
     */
    header->next = cache->free_list;
    cache->free_list = header;
    ++cache->count;

    /*
     * Keep local caches bounded by returning a batch to the shared list.
     */
    if (cache->count > CACHE_MAX_BLOCKS) {
        flush_thread_cache(index);
    }
}

static void *calloc_impl(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) {
        return NULL;
    }

    size_t total_size = nmemb * size;
    void *ptr = malloc_impl(total_size);

    if (ptr != NULL) {
        memset(ptr, 0, total_size);
    }

    return ptr;
}

static void *realloc_impl(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc_impl(size);
    }

    if (size == 0) {
        free_impl(ptr);
        return NULL;
    }

    if (size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }

    BlockHeader *old_header = (BlockHeader *)ptr - 1;
    size_t old_size = old_header->actual_size;

    /*
     * Keep the allocation if its existing capacity is sufficient.
     */
    if (old_header->size >= size) {
        old_header->actual_size = size;
        return ptr;
    }

    void *new_ptr = malloc_impl(size);

    if (new_ptr == NULL) {
        return NULL;
    }

    size_t copy_size = old_size < size ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);

    free_impl(ptr);

    return new_ptr;
}

void *malloc(size_t size) {
    return malloc_impl(size);
}

void *calloc(size_t nmemb, size_t size) {
    return calloc_impl(nmemb, size);
}

void *realloc(void *ptr, size_t size) {
    return realloc_impl(ptr, size);
}

void free(void *ptr) {
    free_impl(ptr);
}
