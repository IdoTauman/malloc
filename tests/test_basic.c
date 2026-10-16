
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define ALIGNMENT 16

int main(void) {
    // malloc: basic allocation, alignment, read/write
    char *p = malloc(32);
    assert(p != NULL);
    assert((uintptr_t)p % ALIGNMENT == 0);

    for (size_t i = 0; i < 32; i++) {
        p[i] = (char)i;
    }

    for (size_t i = 0; i < 32; i++) {
        assert(p[i] == (char)i);
    }

    free(p);

    // malloc: zero-size allocation (this implementation returns NULL)
    assert(malloc(0) == NULL);

    // calloc: memory must be zero-initialized
    unsigned char *c = calloc(64, sizeof(*c));
    assert(c != NULL);

    for (size_t i = 0; i < 64; i++) {
        assert(c[i] == 0);
    }

    free(c);

    // calloc: multiplication overflow
    assert(calloc(SIZE_MAX, 2) == NULL);
    assert(calloc(0, 10) == NULL);
    assert(calloc(10, 0) == NULL);

    // realloc: grow while preserving the original contents
    char *r = malloc(16);
    assert(r != NULL);

    for (size_t i = 0; i < 16; i++) {
        r[i] = (char)(i + 1);
    }

    char *grown = realloc(r, 64);
    assert(grown != NULL);

    for (size_t i = 0; i < 16; i++) {
        assert(grown[i] == (char)(i + 1));
    }

    // The newly added portion is writable
    for (size_t i = 16; i < 64; i++) {
        grown[i] = (char)i;
    }

    free(grown);

    // realloc: shrink while preserving the retained contents
    r = malloc(32);
    assert(r != NULL);
    memset(r, 0x5A, 32);

    char *shrunk = realloc(r, 8);
    assert(shrunk != NULL);

    for (size_t i = 0; i < 8; i++) {
        assert(shrunk[i] == 0x5A);
    }

    free(shrunk);

    // realloc: NULL pointer behaves like malloc
    r = realloc(NULL, 24);
    assert(r != NULL);
    free(r);

    // realloc: size zero frees the allocation and returns NULL
    r = malloc(16);
    assert(r != NULL);
    assert(realloc(r, 0) == NULL);

    puts("test_basic: all tests passed");
    return 0;
}
