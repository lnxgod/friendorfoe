#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scanner_uart_line_framer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_UART_SLOT_COUNT 2U
#define BACKEND_UART_BAUD 921600
#define BACKEND_UART_DATA_BITS 8
#define BACKEND_UART_STOP_BITS 1
#define BACKEND_UART_PARITY_NONE 0

typedef struct {
    int uart;
    int rx_gpio;
    int tx_gpio;
    int baud;
    int data_bits;
    int stop_bits;
    int parity;
    bool flow_control;
} backend_uart_slot_config_t;

typedef struct {
    backend_uart_slot_config_t config;
    scanner_uart_line_framer_t framer;
    uint8_t storage[SCANNER_UART_LINE_BUFFER_SIZE];
} backend_uart_slot_t;

typedef struct {
    backend_uart_slot_t slot[BACKEND_UART_SLOT_COUNT];
} backend_uart_slots_t;

bool backend_uart_slot_config(
    size_t slot_index,
    backend_uart_slot_config_t *out);

bool backend_uart_slots_init(backend_uart_slots_t *slots);

scanner_uart_line_event_t backend_uart_slot_consume(
    backend_uart_slots_t *slots,
    size_t slot_index,
    const uint8_t *bytes,
    size_t byte_len,
    size_t *consumed_out);

scanner_uart_line_event_t backend_uart_slot_expire_partial(
    backend_uart_slots_t *slots,
    size_t slot_index);

bool backend_uart_slot_driver_init(size_t slot_index);
bool backend_uart_slot_driver_reinit(
    backend_uart_slots_t *slots,
    size_t slot_index);

#ifdef __cplusplus
}
#endif
