#pragma once

/* Pure, allocation-free protocol used only by the temporary badge factory
 * topology probe and its native tests. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOF_FACTORY_PROBE_SCHEMA       1
#define FOF_FACTORY_PROBE_SESSION_HEX  32
#define FOF_FACTORY_PROBE_MAC_TEXT     17
#define FOF_FACTORY_PROBE_FRAME_MAX    112
#define FOF_FACTORY_PROBE_REPORT_MAX   288
#define FOF_FACTORY_PROBE_PREFIX       "FOF_FACTORY_PROBE:"

typedef struct {
    char session[FOF_FACTORY_PROBE_SESSION_HEX + 1];
    char mac[FOF_FACTORY_PROBE_MAC_TEXT + 1];
    char link;
    uint32_t sequence;
} fof_factory_probe_frame_t;

typedef struct {
    bool has_a;
    bool has_b;
    char peer_a[FOF_FACTORY_PROBE_MAC_TEXT + 1];
    char peer_b[FOF_FACTORY_PROBE_MAC_TEXT + 1];
} fof_factory_probe_peer_table_t;

bool fof_factory_probe_session_valid(const char *session);
bool fof_factory_probe_mac_valid(const char *mac);
uint32_t fof_factory_probe_crc32(const void *data, size_t length);

bool fof_factory_probe_frame_encode(const fof_factory_probe_frame_t *frame,
                                    char *out, size_t out_size);
bool fof_factory_probe_frame_parse(const char *encoded,
                                   fof_factory_probe_frame_t *out);

bool fof_factory_probe_peer_observe(fof_factory_probe_peer_table_t *table,
                                    const char *self_mac,
                                    const char *expected_session,
                                    char received_link,
                                    const fof_factory_probe_frame_t *frame);

bool fof_factory_probe_report_build(
    const char *self_mac,
    const char *session,
    const fof_factory_probe_peer_table_t *table,
    char *out,
    size_t out_size);

#ifdef __cplusplus
}
#endif
