#pragma once

#include <stdbool.h>

void wifi_scanner_lockon_cancel(void);
void wifi_scanner_pause(void);
void wifi_scanner_resume(void);
bool wifi_scanner_is_paused(void);
bool wifi_scanner_is_quiesced(void);
bool wifi_scanner_is_active(void);
void wifi_scanner_reset_attack_counters(void);
void wifi_scanner_reset_fc_histogram(void);
