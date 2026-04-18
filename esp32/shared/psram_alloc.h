#pragma once

/**
 * Friend or Foe — PSRAM allocation helpers.
 *
 * Thin wrappers around `heap_caps_malloc()` that target external PSRAM when
 * available and fall back to internal SRAM otherwise (so the same binary boots
 * on N16R8, N8R8, and even N4 boards without silent breakage).
 *
 * See esp32/shared/psram_policy.md for the allocation-policy document:
 *   - Regular `malloc()` must NEVER silently migrate to PSRAM — we use
 *     `CONFIG_SPIRAM_USE_CAPS_ALLOC` (not `USE_MALLOC`), so callers opt-in
 *     via the functions here.
 *   - RID / hot-path / static / stack / IRQ code stays on internal SRAM.
 *   - Large buffers (HTTP payloads, dedup caches, entity tables, offline
 *     detection queue) go through `psram_alloc()`.
 */

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate `size` bytes, preferring PSRAM. Falls back to internal SRAM if
 * PSRAM is unavailable or exhausted. Returns NULL on total allocation failure.
 *
 * When PSRAM is compiled out (e.g. legacy ESP32, non-N*R8 S3) this is just
 * `malloc(size)`.
 */
void *psram_alloc(size_t size);

/**
 * Allocate `size` bytes, PSRAM-only. Returns NULL if PSRAM is unavailable.
 * Use when the caller specifically wants to free up internal SRAM and
 * can tolerate falling back to nothing (skipping the feature) if PSRAM
 * isn't present.
 */
void *psram_alloc_strict(size_t size);

/**
 * Zero-initialized equivalent of [psram_alloc].
 */
void *psram_calloc(size_t nmemb, size_t size);

/**
 * Free a pointer returned from any of the psram_* helpers. Safe for NULL.
 * Internally just calls `heap_caps_free()` / `free()`.
 */
void psram_free(void *ptr);

/**
 * True if PSRAM was detected at boot time and at least one allocation has
 * succeeded. False on non-PSRAM boards or if PSRAM init failed (which can
 * happen with `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`).
 */
bool psram_available(void);

/**
 * Free bytes currently available in PSRAM (0 if not present).
 */
size_t psram_free_size(void);

/**
 * Total bytes available in PSRAM (0 if not present).
 */
size_t psram_total_size(void);

#ifdef __cplusplus
}
#endif
