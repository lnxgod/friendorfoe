#ifndef FOF_WIFI_PROBE_CAPTURE_H
#define FOF_WIFI_PROBE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* JSON transport requires valid UTF-8. Keep binary names as unknown metadata. */
static inline bool fof_probe_text_ssid(const uint8_t *text, size_t len)
{
    for (size_t i = 0; i < len;) {
        uint8_t c = text[i++];
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned following;
        uint32_t point, minimum;
        if (c >= 0xc2 && c <= 0xdf) { following = 1; point = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { following = 2; point = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { following = 3; point = c & 7; minimum = 0x10000; }
        else return false;
        if (i + following > len) return false;
        while (following--) {
            c = text[i++];
            if ((c & 0xc0) != 0x80) return false;
            point = (point << 6) | (c & 0x3f);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    }
    return true;
}

/* Validate one SSID IE before treating an empty string as a wildcard. Binary
 * names are captured with no text; they must never be labeled wildcard. */
static inline bool fof_probe_ssid(const uint8_t *frame, size_t len,
                                  char out[33], bool *wildcard, bool *binary)
{
    if (!frame || len < 26 || (frame[0] & 0xfc) != 0x40 || (frame[10] & 1)) return false;
    bool found = false;
    *wildcard = false;
    *binary = false;
    out[0] = '\0';
    for (size_t offset = 24; offset < len;) {
        if (offset + 2 > len) return false;
        uint8_t tag = frame[offset], size = frame[offset + 1];
        offset += 2;
        if (offset + size > len) return false;
        if (tag == 0) {
            if (found || size > 32) return false;
            found = true;
            *wildcard = size == 0;
            *binary = !fof_probe_text_ssid(frame + offset, size);
            if (!*binary) {
                memcpy(out, frame + offset, size);
                out[size] = '\0';
            }
        }
        offset += size;
    }
    return found;
}

static inline uint16_t fof_probe_frequency_mhz(uint16_t channel)
{
    if (channel >= 1 && channel <= 13) return 2407 + channel * 5;
    if (channel == 14) return 2484;
    if (channel >= 36 && channel <= 177) return 5000 + channel * 5;
    return 0;
}

/* Ordinary probes are sampled with a global ceiling as well as the scanner's
 * per-address/name limiter. A busy venue must not starve Remote ID traffic. */
typedef struct {
    int64_t window_ms;
    unsigned count;
} fof_probe_budget_t;

static inline bool fof_probe_budget_allow(fof_probe_budget_t *budget, int64_t now_ms)
{
    if (now_ms < budget->window_ms || now_ms - budget->window_ms >= 1000) {
        budget->window_ms = now_ms;
        budget->count = 0;
    }
    if (budget->count >= 3) return false;
    budget->count++;
    return true;
}
#endif
