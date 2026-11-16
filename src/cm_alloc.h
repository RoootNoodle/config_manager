#pragma once

#include <stddef.h>

void *cm_malloc(size_t size);
void *cm_calloc(size_t n, size_t size);
void cm_free(void *ptr);
