#include "cm_alloc.h"
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"

#if CONFIG_CONFIG_MANAGER_ALLOC_INTERNAL
#define CM_CAPS_PRIMARY (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#elif CONFIG_SPIRAM
#define CM_CAPS_PRIMARY (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CM_CAPS_FALLBACK (MALLOC_CAP_DEFAULT)
#endif

void *cm_malloc(size_t size)
{
#if defined(CM_CAPS_FALLBACK)
    return heap_caps_malloc_prefer(size, 2, CM_CAPS_PRIMARY, CM_CAPS_FALLBACK);
#elif defined(CM_CAPS_PRIMARY)
    return heap_caps_malloc(size, CM_CAPS_PRIMARY);
#else
    return malloc(size);
#endif
}

void *cm_calloc(size_t n, size_t size)
{
#if defined(CM_CAPS_FALLBACK)
    return heap_caps_calloc_prefer(n, size, 2, CM_CAPS_PRIMARY, CM_CAPS_FALLBACK);
#elif defined(CM_CAPS_PRIMARY)
    return heap_caps_calloc(n, size, CM_CAPS_PRIMARY);
#else
    return calloc(n, size);
#endif
}

void cm_free(void *ptr)
{
    heap_caps_free(ptr);
}
