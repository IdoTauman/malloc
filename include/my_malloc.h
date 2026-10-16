#pragma once

#include <stddef.h>

void *malloc(size_t bytes);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);