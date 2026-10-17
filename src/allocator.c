#include "internal.h"

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

typedef struct {
    BlockHeader *free_list;
    spinlock_t lock;
} SizeClass;

static SizeClass size_classes[CLASS_COUNT];

/*
 * initialize class locks exactly once
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
 * classes:
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
 * returns NO_CLASS if the requested size cannot be represented by a class
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
 * map a dedicated allocation, large requests do not enter a size class
 * freelist, so freeing them can release their mapping immediately
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
 * add blocks from a new mmap chunk to one class's freelist
 * the caller must hold that class's lock
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

static void *malloc_impl(size_t size) {
    if (size == 0 || size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }

    ensure_classes_initialized();

    size_t index = size_to_class(size);

    /*
     * large blocks bypass the class freelists
     */
    if (index == NO_CLASS ||
        class_capacity(index) >= DIRECT_MMAP_THRESHOLD) {
        BlockHeader *header = request_dedicated_mapping(size);
        return header ? (void *)(header + 1) : NULL;
    }

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

static void free_impl(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    BlockHeader *header = (BlockHeader *)ptr - 1;

    /*
     * dedicated mappings can be returned directly to the os
     */
    if (header->class_index == NO_CLASS) {
        size_t mapping_size = header->mapping_size;
        munmap(header, mapping_size);
        return;
    }

    size_t index = header->class_index;
    SizeClass *class = &size_classes[index];

    spin_lock(&class->lock);

    /*
     * avoid inserting the same block twice if a block is accidentally
     * freed twice. passing an invalid pointer to free remains undefined
     */
    if (header->is_free) {
        spin_unlock(&class->lock);
        return;
    }

    header->actual_size = 0;
    header->is_free = true;
    header->next = class->free_list;
    class->free_list = header;

    spin_unlock(&class->lock);
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
     * keep the allocation if its existing capacity is sufficient
     * update the requested size for future realloc calls.
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