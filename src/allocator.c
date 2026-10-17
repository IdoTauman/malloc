#include "internal.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define HEADER_SIZE sizeof(BlockHeader)

#define CALLOC_MMAP_THRESHOLD (32 * 1024)

static BlockHeader *heap_head = NULL;
static spinlock_t head_lock = { .lock = ATOMIC_FLAG_INIT };

/**
 * requests new memory using mmap/sbrk and returns a pointer to the new block, inits header.
 */
static BlockHeader *request_from_os(size_t aligned_size, size_t actual_size);

/**
 * splits an existing block into two if there is enough space for a header and the payload.
 * assumes the block curr is already locked by the caller.
 */
static void split_block(BlockHeader *curr, size_t requested_size);


static BlockHeader *request_from_os(size_t aligned_size, size_t actual_size) {
    if (aligned_size == 0 ||
        aligned_size > SIZE_MAX - HEADER_SIZE ||
        aligned_size + HEADER_SIZE > SIZE_MAX - 4095) {
        return NULL;
    }
    // calculate size needed (page aligned requested size + header or minimum block size)
    size_t total_needed = aligned_size + HEADER_SIZE;
    total_needed = total_needed > MMAP_CHUNK_SIZE ? total_needed : MMAP_CHUNK_SIZE;
    total_needed = (total_needed + 4095) & ~(size_t)4095;

    // allocate pointer
    // mmap call according to malloc standard
    void *block_ptr = mmap(NULL, total_needed, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // return null on fail
    if (block_ptr == MAP_FAILED) {
        return NULL;
    }

    // init block header
    BlockHeader *header = (BlockHeader *)block_ptr;
    header->is_free = false;
    spinlock_init(&header->lock);
    header->next = NULL;
    header->size = total_needed - HEADER_SIZE;
    header->actual_size = actual_size;

    // insert into heap_head
    spin_lock(&head_lock);
    header->next = heap_head;
    spin_lock(&header->lock);
    split_block(header, aligned_size);
    spin_unlock(&header->lock);
    heap_head = header;
    spin_unlock(&head_lock);

    return header;
}

static void split_block(BlockHeader *curr, size_t requested_size) {
    // return early if curr->size < requested_size
    if (curr->size < requested_size) return;

    // check if remaining space >= HEADER_SIZE + ALIGNMENT
    size_t leftover = curr->size - requested_size;
    if (leftover < HEADER_SIZE + ALIGNMENT) return;

    // carve out new BlockHeader at the offset
    uint8_t *offset = (uint8_t *)curr + sizeof(BlockHeader) + requested_size;
    BlockHeader *new = (BlockHeader *)offset;

    // initialize new header fields
    new->is_free = true;
    spinlock_init(&new->lock);
    new->size = leftover - HEADER_SIZE;
    new->actual_size = 0;

    // add new block to the linked list
    new->next = curr->next;
    curr->next = new;

    curr->size = requested_size;
}

static void *malloc_impl(size_t size) {
    if (size == 0 || size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }

    // align size
    size_t aligned_size = ALIGN(size);

    // traverse the list starting from heap_head using hand-over-hand locking and find a suitable block
    BlockHeader *foundBlock = NULL;
    BlockHeader *curr = NULL;
    BlockHeader *next;

    spin_lock(&head_lock);
    next = heap_head;
    if (next) {
        spin_lock(&next->lock);
    }
    spin_unlock(&head_lock);

    while (next) {
        curr = next;
        next = curr->next;

        if (curr->is_free && curr->size >= aligned_size) {
            foundBlock = curr;
            break;
        }

        if (next) {
            spin_lock(&next->lock);
        }

        spin_unlock(&curr->lock);
        curr = NULL;
    }

    // if a suitable block is found, split if possible, mark it as taken, unlock, and return a pointer to the payload
    if (foundBlock) {
        split_block(foundBlock, aligned_size);
        foundBlock->is_free = false;
        foundBlock->actual_size = size;
        spin_unlock(&foundBlock->lock);
        return foundBlock + 1;
    } else {
        if (curr) {
            spin_unlock(&curr->lock);
        }
        // if no suitable block is found, request a new block and return it
        BlockHeader *newBlock = request_from_os(aligned_size, size);
        if (!newBlock) return NULL;
        return newBlock + 1;
    }
}

static void free_impl(void *ptr) {
    if (!ptr) {
        return;
    }

    // compute header address
    BlockHeader *header = (BlockHeader *)((uint8_t *)ptr - HEADER_SIZE);

    // acquire block lock
    spin_lock(&header->lock);

    // mark is_free = true
    header->is_free = true;
    header->actual_size = 0;

    // release block lock
    spin_unlock(&header->lock);
}

static void *calloc_impl(size_t nmemb, size_t size) {
    // check for multiplication overflow
    if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size) {
        return NULL;
    }

    size_t total_size = nmemb * size;
    if (total_size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }

    // allocate zero'd memory
    if (total_size >= CALLOC_MMAP_THRESHOLD) {
        // use mmap directly for large allocations to avoid a wasteful memset call
        BlockHeader *block = request_from_os(ALIGN(total_size), total_size);
        return block ? (void *)(block + 1) : NULL;
    } else {
        void *block_payload = malloc_impl(total_size);
        if (!block_payload) return NULL;
        memset(block_payload, 0, total_size);
        return block_payload;
    }
}

static void *realloc_impl(void *ptr, size_t size) {
    // handle edge cases
    if (!ptr) {
        return malloc_impl(size);
    }

    if (size == 0) {
        free_impl(ptr);
        return NULL;
    }

    if (size > SIZE_MAX - (ALIGNMENT - 1)) return NULL;

    // allocate new block
    void *new_block = malloc_impl(size);
    if (!new_block) return ptr;

    // copy min(old_size, size) bytes to new block
    size_t old_size = ((BlockHeader *)((uint8_t *)ptr - HEADER_SIZE))->actual_size;
    size_t copy_size = old_size < size ? old_size : size;
    memcpy(new_block, ptr, copy_size);

    // free old block
    free_impl(ptr);

    // return new block pointer
    return new_block;
}

void *malloc(size_t size) { return malloc_impl(size); }
void *calloc(size_t nmemb, size_t size) { return calloc_impl(nmemb, size); }
void *realloc(void *ptr, size_t size) { return realloc_impl(ptr, size); }
void free(void *ptr) { free_impl(ptr); }