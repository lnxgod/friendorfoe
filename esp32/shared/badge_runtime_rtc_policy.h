#pragma once

#include "badge_runtime_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The first three words are a permanent cross-version ABI. New fields may
 * only be appended after BADGE_RUNTIME_RTC_EXTENSION_OFFSET.
 */
#define BADGE_RUNTIME_RTC_RECOVERY_OFFSET          0U
#define BADGE_RUNTIME_RTC_GENERATION_OFFSET        4U
#define BADGE_RUNTIME_RTC_EXPECTED_MAGIC_OFFSET    8U
#define BADGE_RUNTIME_RTC_EXTENSION_OFFSET         12U
#define BADGE_RUNTIME_RTC_LAYOUT_MAGIC_OFFSET      12U
#define BADGE_RUNTIME_RTC_LAYOUT_VERSION_OFFSET    16U
#define BADGE_RUNTIME_RTC_LAYOUT_SIZE_OFFSET       18U
#define BADGE_RUNTIME_RTC_PRODUCTION_SIZE          0x14U
#define BADGE_RUNTIME_RTC_CANARY_MARKER_OFFSET     0x14U
#define BADGE_RUNTIME_RTC_CANARY_GAME_OFFSET       0xB4U
#define BADGE_RUNTIME_RTC_CANARY_SIZE              0xC8U

/* One-hop migration offsets from the shifted pre-fix game canary. */
#define BADGE_RUNTIME_RTC_OLD_CANARY_GAME_OFFSET       0x00U
#define BADGE_RUNTIME_RTC_OLD_CANARY_MARKER_OFFSET     0x14U
#define BADGE_RUNTIME_RTC_OLD_CANARY_RECOVERY_OFFSET   0xB4U
#define BADGE_RUNTIME_RTC_OLD_CANARY_GENERATION_OFFSET 0xB8U
#define BADGE_RUNTIME_RTC_OLD_CANARY_MAGIC_OFFSET      0xBCU
#define BADGE_RUNTIME_RTC_OLD_CANARY_SIZE              0xC0U

#define BADGE_RUNTIME_RTC_LAYOUT_MAGIC             0x464F4652U
#define BADGE_RUNTIME_RTC_LAYOUT_VERSION           1U

typedef enum {
    BADGE_RUNTIME_RTC_SOURCE_NONE = 0,
    BADGE_RUNTIME_RTC_SOURCE_CURRENT,
    BADGE_RUNTIME_RTC_SOURCE_OLD_CANARY,
    BADGE_RUNTIME_RTC_SOURCE_LEGACY_V078,
    BADGE_RUNTIME_RTC_SOURCE_AMBIGUOUS,
} badge_runtime_rtc_source_t;

typedef struct {
    badge_runtime_rtc_source_t source;
    bool expected_software_reset;
    bool prior_layout_valid;
    uint32_t consumed_generation;
} badge_runtime_rtc_boot_result_t;

/*
 * Classify retained bytes without modifying them. This is safe to call before
 * badge_runtime_init(), including from rollback admission.
 */
badge_runtime_rtc_boot_result_t badge_runtime_rtc_classify(
    const uint8_t *storage,
    size_t storage_size,
    bool software_reset,
    uint16_t expected_layout_size);

/*
 * Classify, consume, and migrate retained bytes in one operation. Callers must
 * pass the exact size of the RTC object compiled into the running image.
 */
badge_runtime_rtc_boot_result_t badge_runtime_rtc_transition(
    uint8_t *storage,
    size_t storage_size,
    bool software_reset,
    uint16_t expected_layout_size);

#ifdef __cplusplus
}
#endif
