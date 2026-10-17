
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define ALIGNMENT 16

#define CHECK_START(name) do { \
    printf("[START] %s\n", name); \
    fflush(stdout); \
} while (0)

#define CHECK_DONE(name) do { \
    printf("[ OK  ] %s\n", name); \
    fflush(stdout); \
} while (0)

int main(void) {
    char *p;
    unsigned char *c;
    char *r;
    char *grown;
    char *shrunk;
    volatile size_t huge_count = SIZE_MAX;
    void *zero_ptr;
    void *zero_count;
    void *zero_size;

    CHECK_START("malloc basic allocation");
    p = malloc(32);
    assert(p != NULL);
    assert((uintptr_t)p % ALIGNMENT == 0);
    CHECK_DONE("malloc basic allocation");

    CHECK_START("write and read allocated memory");
    for (size_t i = 0; i < 32; i++) {
        p[i] = (char)i;
    }
    for (size_t i = 0; i < 32; i++) {
        assert(p[i] == (char)i);
    }
    CHECK_DONE("write and read allocated memory");

    CHECK_START("free basic allocation");
    free(p);
    CHECK_DONE("free basic allocation");

    CHECK_START("malloc zero");
    zero_ptr = malloc(0);
    free(zero_ptr);
    CHECK_DONE("malloc zero");

    CHECK_START("calloc zero initialization");
    c = calloc(64, sizeof(*c));
    assert(c != NULL);
    for (size_t i = 0; i < 64; i++) {
        assert(c[i] == 0);
    }
    CHECK_DONE("calloc zero initialization");

    CHECK_START("free calloc allocation");
    free(c);
    CHECK_DONE("free calloc allocation");

    CHECK_START("calloc multiplication overflow");
    assert(calloc(huge_count, 2) == NULL);
    CHECK_DONE("calloc multiplication overflow");

    CHECK_START("calloc zero count");
    zero_count = calloc(0, 10);
    free(zero_count);
    CHECK_DONE("calloc zero count");

    CHECK_START("calloc zero element size");
    zero_size = calloc(10, 0);
    free(zero_size);
    CHECK_DONE("calloc zero element size");

    CHECK_START("realloc grow");
    r = malloc(16);
    assert(r != NULL);
    for (size_t i = 0; i < 16; i++) {
        r[i] = (char)(i + 1);
    }

    grown = realloc(r, 64);
    assert(grown != NULL);
    for (size_t i = 0; i < 16; i++) {
        assert(grown[i] == (char)(i + 1));
    }
    for (size_t i = 16; i < 64; i++) {
        grown[i] = (char)i;
    }
    CHECK_DONE("realloc grow");

    CHECK_START("free grown allocation");
    free(grown);
    CHECK_DONE("free grown allocation");

    CHECK_START("realloc shrink");
    r = malloc(32);
    assert(r != NULL);
    memset(r, 0x5A, 32);

    shrunk = realloc(r, 8);
    assert(shrunk != NULL);
    for (size_t i = 0; i < 8; i++) {
        assert(shrunk[i] == 0x5A);
    }
    CHECK_DONE("realloc shrink");

    CHECK_START("free shrunk allocation");
    free(shrunk);
    CHECK_DONE("free shrunk allocation");

    CHECK_START("realloc NULL pointer");
    r = realloc(NULL, 24);
    assert(r != NULL);
    CHECK_DONE("realloc NULL pointer");

    CHECK_START("free realloc NULL result");
    free(r);
    CHECK_DONE("free realloc NULL result");

    CHECK_START("realloc size zero");
    r = malloc(16);
    assert(r != NULL);
    assert(realloc(r, 0) == NULL);
    CHECK_DONE("realloc size zero");

    puts("test_basic: all tests passed");
    return 0;
}
