/**
 * Friend or Foe — PSRAM allocation helpers.
 * See psram_alloc.h for usage; see psram_policy.md for the rules.
 */

#include "psram_alloc.h"

#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  #include "esp_heap_caps.h"
  #include "esp_psram.h"
  #define PSRAM_ENABLED 1
#else
  #define PSRAM_ENABLED 0
#endif

void *psram_alloc(size_t size)
{
    if (size == 0) return NULL;
#if PSRAM_ENABLED
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    /* Fall back to internal — caller accepted that cost by using psram_alloc. */
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
#else
    return malloc(size);
#endif
}

void *psram_alloc_strict(size_t size)
{
    if (size == 0) return NULL;
#if PSRAM_ENABLED
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return NULL;
#endif
}

void *psram_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (total == 0) return NULL;
    if (nmemb != 0 && total / nmemb != size) return NULL;  /* overflow */
    void *p = psram_alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void psram_free(void *ptr)
{
    if (!ptr) return;
#if PSRAM_ENABLED
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
}

bool psram_available(void)
{
#if PSRAM_ENABLED
    return esp_psram_is_initialized();
#else
    return false;
#endif
}

size_t psram_free_size(void)
{
#if PSRAM_ENABLED
    if (!esp_psram_is_initialized()) return 0;
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

size_t psram_total_size(void)
{
#if PSRAM_ENABLED
    if (!esp_psram_is_initialized()) return 0;
    return esp_psram_get_size();
#else
    return 0;
#endif
}
