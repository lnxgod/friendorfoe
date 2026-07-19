#pragma once

#include <stdbool.h>

bool ble_remote_id_is_scanning(void);
bool ble_remote_id_is_quiesced(void);
bool ble_remote_id_is_active(void);
void ble_remote_id_start(void);
void ble_remote_id_stop(void);
void ble_remote_id_reset_profile_counters(void);
void ble_rid_lockon_cancel(void);
