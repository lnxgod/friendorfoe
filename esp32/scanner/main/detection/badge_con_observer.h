#pragma once

#include "badge_con_encounter.h"
#include "badge_con_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void badge_con_observer_init(bool ble_primary);
bool badge_con_observer_set_self(uint32_t peer, uint8_t session);
badge_con_frame_result_t badge_con_observer_consume(
    const uint8_t *advertisement,
    size_t advertisement_size,
    int8_t rssi,
    uint32_t now_ms,
    badge_con_observe_result_t *observe_result_out);
bool badge_con_observer_take_pending(badge_con_packet_t *out);

#ifdef __cplusplus
}
#endif
