#include "internal.h"
#include <sys/mman.h>
#include <unistd.h>

#define ALIGNMENT 16
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define HEADER_SIZE sizeof(BlockHeader)

static BlockHeader *heap_head = NULL;
static spinlock_t head_lock = { .lock = ATOMIC_FLAG_INIT };

/**
 * requests new memory using mmap/sbrk and returns a pointer to the new block, inits header.
 */
static BlockHeader *request_from_os(size_t aligned_size);

/**
 * splits an existing block into two if there is enough space for a header and the payload.
 * assumes the block curr is already locked by the caller.
 */
static void split_block(BlockHeader *curr, size_t requested_size);


static BlockHeader *request_from_os(size_t aligned_size) {
    // calculate size needed (page aligned requested size + header or minimum block size)
    size_t total_needed = aligned_size + HEADER_SIZE;
    total_needed = total_needed > MMAP_CHUNK_SIZE ? total_needed : MMAP_CHUNK_SIZE;
    total_needed = (total_needed + 4095) & ~4095;

    // allocate pointer
    // mmap call according to malloc standard
    void *block_ptr = mmap(NULL, total_needed, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // return null on fail
    if (block_ptr == MAP_FAILED) {
        return NULL;
    }

    // init block header
    BlockHeader *header = (BlockHeader *)block_ptr;
    header->is_free = 1;
    spinlock_init(&header->lock);
    header->next = NULL;
    header->size = total_needed - HEADER_SIZE;

    // insert into heap_head
    spin_lock(&head_lock);
    header->next = heap_head;
    heap_head = header;
    spin_unlock(&head_lock);

    return header;
}

static void split_block(BlockHeader *curr, size_t requested_size) {
    // TODO: Check if remaining space >= HEADER_SIZE + ALIGNMENT
    // TODO: Carve out new BlockHeader at the offset
    // TODO: Initialize new header fields and splice into the linked list
    // TODO: Update curr->size to requested_size
}

void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    // TODO:
    // 1. Align size using ALIGN(size)
    // 2. Traverse the list starting from heap_head using hand-over-hand locking
    // 3. If a suitable free block is found:
    //    - split_block(curr, aligned_size) if possible
    //    - Mark curr->is_free = false
    //    - Unlock curr
    //    - Return pointer to payload (curr + 1)
    // 4. If no suitable block is found:
    //    - Call request_from_os(aligned_size)
    //    - Return payload pointer

    return NULL;
}

void my_free(void *ptr) {
    if (!ptr) {
        return;
    }

    // TODO:
    // 1. Compute header address: (BlockHeader*)((uint8_t*)ptr - HEADER_SIZE)
    // 2. Acquire block lock via spin_lock()
    // 3. Mark is_free = true
    // 4. Release block lock via spin_unlock()
}

void *calloc(size_t nmemb, size_t size) {
    // TODO: Check for multiplication overflow
    // TODO: Allocate memory using my_malloc(total_size)
    // TODO: Zero out allocated memory with memset
    return NULL;
}

void *realloc(void *ptr, size_t size) {
    // TODO: Handle edge cases (ptr == NULL, size == 0)
    // TODO: Allocate new block with my_malloc(size)
    // TODO: Copy min(old_size, size) bytes to new block
    // TODO: Free old block via my_free(ptr)
    // TODO: Return new block pointer
    return NULL;
}