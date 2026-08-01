#pragma once

#include <stdbool.h>

bool uart_tx_is_enabled(void);
void uart_tx_set_enabled(bool enabled);
void uart_tx_flush_detection_queue(void);
void uart_tx_reset_counts(void);
