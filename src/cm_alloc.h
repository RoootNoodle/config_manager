#pragma once

#include <stddef.h>

void *cm_malloc(size_t size);
void *cm_calloc(size_t n, size_t size);
void cm_free(void *ptr);

#ifdef CONFIG_MANAGER_TEST_HOOKS
extern int cm_alloc_fail_nth;
#endif
