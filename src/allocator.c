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

    // add new block to the linked list
    new->next = curr->next;
    curr->next = new;

    curr->size = requested_size;
}

void *malloc(size_t size) {
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
        spin_unlock(&foundBlock->lock);
        return foundBlock + 1;
    } else {
        if (curr) {
            spin_unlock(&curr->lock);
        }
        // if no suitable block is found, request a new block and return it
        BlockHeader *newBlock = request_from_os(aligned_size);
        if (!newBlock) return NULL;
        return newBlock + 1;
    }
}

void free(void *ptr) {
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