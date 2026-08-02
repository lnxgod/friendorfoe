#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "backend_json_reader.h"
#include "detection_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Test-only, field-by-field comparison. Never use this as a wire oracle. */
bool backend_detection_equal(const drone_detection_t *expected,
                             const drone_detection_t *actual);
void backend_assert_detection_equal(const drone_detection_t *expected,
                                    const drone_detection_t *actual);

/* Independent scanner-struct-to-HTTP contract oracle for Task 8. */
void backend_assert_detection_json_equal(
    const drone_detection_t *expected,
    const char *json,
    const backend_json_token_t *tokens,
    size_t token_count,
    size_t object_index);

#ifdef __cplusplus
}
#endif
