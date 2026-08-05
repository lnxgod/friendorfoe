#include "backend_json_writer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void writer_fail(backend_json_writer_t *writer)
{
    if (writer == NULL) {
        return;
    }
    writer->failed = true;
    writer->length = 0;
    if (writer->buffer != NULL && writer->capacity > 0) {
        writer->buffer[0] = '\0';
    }
}

static bool utf8_sequence_valid(const uint8_t *value, size_t remaining,
                                size_t *sequence_length)
{
    const uint8_t first = value[0];
    if (first < 0x80U) {
        *sequence_length = 1;
        return true;
    }
    if (first >= 0xC2U && first <= 0xDFU) {
        if (remaining < 2 || (value[1] & 0xC0U) != 0x80U) {
            return false;
        }
        *sequence_length = 2;
        return true;
    }
    if (first >= 0xE0U && first <= 0xEFU) {
        if (remaining < 3 || (value[1] & 0xC0U) != 0x80U ||
            (value[2] & 0xC0U) != 0x80U ||
            (first == 0xE0U && value[1] < 0xA0U) ||
            (first == 0xEDU && value[1] >= 0xA0U)) {
            return false;
        }
        *sequence_length = 3;
        return true;
    }
    if (first >= 0xF0U && first <= 0xF4U) {
        if (remaining < 4 || (value[1] & 0xC0U) != 0x80U ||
            (value[2] & 0xC0U) != 0x80U ||
            (value[3] & 0xC0U) != 0x80U ||
            (first == 0xF0U && value[1] < 0x90U) ||
            (first == 0xF4U && value[1] >= 0x90U)) {
            return false;
        }
        *sequence_length = 4;
        return true;
    }
    return false;
}

static bool utf8_valid(const char *value)
{
    if (value == NULL) {
        return false;
    }
    const uint8_t *bytes = (const uint8_t *)value;
    size_t remaining = strlen(value);
    while (remaining > 0) {
        size_t sequence_length = 0;
        if (!utf8_sequence_valid(bytes, remaining, &sequence_length)) {
            return false;
        }
        bytes += sequence_length;
        remaining -= sequence_length;
    }
    return true;
}

static bool append_bytes(backend_json_writer_t *writer,
                         const char *bytes, size_t length)
{
    if (writer == NULL || writer->failed || writer->buffer == NULL ||
        writer->capacity == 0 || bytes == NULL ||
        writer->length >= writer->capacity ||
        length > writer->capacity - writer->length - 1U) {
        writer_fail(writer);
        return false;
    }
    if (length > 0) {
        memcpy(writer->buffer + writer->length, bytes, length);
        writer->length += length;
    }
    writer->buffer[writer->length] = '\0';
    return true;
}

void backend_json_writer_init(
    backend_json_writer_t *writer, char *buffer, size_t capacity)
{
    if (writer == NULL) {
        return;
    }
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->length = 0;
    writer->failed = buffer == NULL || capacity == 0;
    if (buffer != NULL && capacity > 0) {
        buffer[0] = '\0';
    }
}

bool backend_json_append(backend_json_writer_t *writer, const char *text)
{
    if (text == NULL) {
        writer_fail(writer);
        return false;
    }
    return append_bytes(writer, text, strlen(text));
}

bool backend_json_append_format(
    backend_json_writer_t *writer, const char *format, ...)
{
    if (writer == NULL || writer->failed || writer->buffer == NULL ||
        writer->capacity == 0 || format == NULL ||
        writer->length >= writer->capacity) {
        writer_fail(writer);
        return false;
    }

    const size_t remaining = writer->capacity - writer->length;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(
        writer->buffer + writer->length, remaining, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= remaining) {
        writer_fail(writer);
        return false;
    }
    writer->length += (size_t)written;
    return true;
}

bool backend_json_append_escaped(
    backend_json_writer_t *writer, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    if (!utf8_valid(value) || !append_bytes(writer, "\"", 1)) {
        writer_fail(writer);
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)value;
    size_t remaining = strlen(value);
    while (remaining > 0) {
        const uint8_t byte = bytes[0];
        const char *escape = NULL;
        switch (byte) {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape != NULL) {
            if (!append_bytes(writer, escape, 2)) {
                return false;
            }
            ++bytes;
            --remaining;
            continue;
        }
        if (byte < 0x20U) {
            const char encoded[6] = {
                '\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0x0FU],
            };
            if (!append_bytes(writer, encoded, sizeof(encoded))) {
                return false;
            }
            ++bytes;
            --remaining;
            continue;
        }
        size_t sequence_length = 0;
        if (!utf8_sequence_valid(bytes, remaining, &sequence_length) ||
            !append_bytes(writer, (const char *)bytes, sequence_length)) {
            writer_fail(writer);
            return false;
        }
        bytes += sequence_length;
        remaining -= sequence_length;
    }
    return append_bytes(writer, "\"", 1);
}

size_t backend_json_writer_finish(backend_json_writer_t *writer)
{
    if (writer == NULL || writer->failed || writer->buffer == NULL ||
        writer->capacity == 0 || writer->length >= writer->capacity) {
        writer_fail(writer);
        return 0;
    }
    writer->buffer[writer->length] = '\0';
    return writer->length;
}
