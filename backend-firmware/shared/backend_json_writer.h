#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool failed;
} backend_json_writer_t;

void backend_json_writer_init(
    backend_json_writer_t *writer, char *buffer, size_t capacity);
bool backend_json_append(backend_json_writer_t *writer, const char *text);
bool backend_json_append_format(
    backend_json_writer_t *writer, const char *format, ...);

/* Appends one complete JSON string value, including its quotes. */
bool backend_json_append_escaped(
    backend_json_writer_t *writer, const char *value);

size_t backend_json_writer_finish(backend_json_writer_t *writer);

#ifdef __cplusplus
}
#endif
