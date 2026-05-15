#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "badge_display_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

void badge_display_policy_runtime_init(void);
const badge_display_policy_t *badge_display_policy_runtime_get(void);
uint32_t badge_display_policy_runtime_hash(void);

bool badge_display_policy_runtime_set(const badge_display_policy_t *policy,
                                      bool persist);
void badge_display_policy_runtime_reset(bool persist);

size_t badge_display_policy_runtime_json(char *out, size_t out_len);
size_t badge_display_policy_runtime_command_json(char *out, size_t out_len);

void badge_display_policy_runtime_note_filtered(
    badge_display_policy_class_t cls);
uint32_t badge_display_policy_runtime_filtered_count(
    badge_display_policy_class_t cls);
void badge_display_policy_runtime_clear_filtered_counts(void);

#ifdef __cplusplus
}
#endif

#endif /* FOF_BADGE_VARIANT */
