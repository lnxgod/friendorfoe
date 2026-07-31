#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_CON_PROTOCOL_VERSION 1U
#define BADGE_CON_ROUND 0x22U
#define BADGE_CON_SERVICE_PAYLOAD_BYTES 10U
#define BADGE_CON_LEGACY_ADV_BYTES 31U
#define BADGE_CON_UART_LINE_CHARS 33U
#define BADGE_CON_UART_WIRE_BYTES 34U
#define BADGE_CON_UART_BUFFER_BYTES 35U

typedef enum {
    BADGE_CON_ROLE_NORMAL = 0,
    BADGE_CON_ROLE_INFECTED = 1,
    BADGE_CON_ROLE_IMMUNE = 2,
} badge_con_role_t;

typedef struct {
    uint8_t version;
    uint8_t round;
    badge_con_role_t role;
    bool super;
    uint32_t peer;
    uint8_t session;
    uint8_t sequence;
    int8_t rssi;
} badge_con_packet_t;

typedef enum {
    BADGE_CON_FRAME_NOT_GAME = 0,
    BADGE_CON_FRAME_INVALID = 1,
    BADGE_CON_FRAME_VALID = 2,
} badge_con_frame_result_t;

uint64_t badge_con_siphash24(const uint8_t key[16],
                             const uint8_t *bytes,
                             size_t byte_count);
bool badge_con_build_service_payload(
    badge_con_role_t role,
    bool super,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_SERVICE_PAYLOAD_BYTES]);
bool badge_con_build_legacy_advertisement(
    badge_con_role_t role,
    bool super,
    uint32_t peer,
    uint8_t session,
    uint8_t sequence,
    uint8_t out[BADGE_CON_LEGACY_ADV_BYTES]);
badge_con_frame_result_t badge_con_parse_advertisement(
    const uint8_t *advertisement,
    size_t advertisement_size,
    int8_t rssi,
    badge_con_packet_t *out);
bool badge_con_render_uart_line(const badge_con_packet_t *packet,
                                char *out,
                                size_t out_size,
                                size_t *wire_size_out);
bool badge_con_parse_uart_line(const uint8_t *bytes,
                               size_t byte_count,
                               badge_con_packet_t *out);
bool badge_con_render_self_command(uint32_t peer,
                                   uint8_t session,
                                   char *out,
                                   size_t out_size);
bool badge_con_render_self_ack(uint32_t peer,
                               uint8_t session,
                               char *out,
                               size_t out_size);

#ifdef __cplusplus
}
#endif
