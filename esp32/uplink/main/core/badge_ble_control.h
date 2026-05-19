#pragma once

#ifdef FOF_BADGE_VARIANT

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void badge_ble_control_init(void);
bool badge_ble_control_open_pairing_window(void);
bool badge_ble_control_pairing_active(void);
size_t badge_ble_control_status_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* FOF_BADGE_VARIANT */
