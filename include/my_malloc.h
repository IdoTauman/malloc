#pragma once

#ifdef USE_PREFIX
#define malloc  my_malloc
#define calloc  my_calloc
#define realloc my_realloc
#define free    my_free
#endif

#include <stddef.h>

void *malloc(size_t bytes);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);