#include "backend_uart_slot.h"

#include "backend_hardware_profile.h"

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#define BACKEND_UART_PORT_1 ((int)UART_NUM_1)
#define BACKEND_UART_PORT_2 ((int)UART_NUM_2)
#else
#define BACKEND_UART_PORT_1 1
#define BACKEND_UART_PORT_2 2
#endif

static const backend_uart_slot_config_t SLOT_CONFIG[BACKEND_UART_SLOT_COUNT] = {
    {
        .uart = BACKEND_UART_PORT_1,
        .rx_gpio = FOF_BACKEND_UPLINK_SLOT0_UART_RX_PIN,
        .tx_gpio = FOF_BACKEND_UPLINK_SLOT0_UART_TX_PIN,
        .baud = BACKEND_UART_BAUD,
        .data_bits = BACKEND_UART_DATA_BITS,
        .stop_bits = BACKEND_UART_STOP_BITS,
        .parity = BACKEND_UART_PARITY_NONE,
        .flow_control = false,
    },
    {
        .uart = BACKEND_UART_PORT_2,
        .rx_gpio = FOF_BACKEND_UPLINK_SLOT1_UART_RX_PIN,
        .tx_gpio = FOF_BACKEND_UPLINK_SLOT1_UART_TX_PIN,
        .baud = BACKEND_UART_BAUD,
        .data_bits = BACKEND_UART_DATA_BITS,
        .stop_bits = BACKEND_UART_STOP_BITS,
        .parity = BACKEND_UART_PARITY_NONE,
        .flow_control = false,
    },
};

static scanner_uart_line_event_t invalid_line_event(void)
{
    scanner_uart_line_event_t event = {
        .kind = SCANNER_UART_LINE_EVENT_INVALID_ARGUMENT,
        .reject_reason = SCANNER_UART_LINE_REJECT_NONE,
        .bytes = NULL,
        .byte_len = 0U,
    };
    return event;
}

bool backend_uart_slot_config(
    size_t slot_index,
    backend_uart_slot_config_t *out)
{
    if (!out || slot_index >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
    *out = SLOT_CONFIG[slot_index];
    return true;
}

bool backend_uart_slots_init(backend_uart_slots_t *slots)
{
    if (!slots) {
        return false;
    }
    for (size_t index = 0U; index < BACKEND_UART_SLOT_COUNT; ++index) {
        slots->slot[index].config = SLOT_CONFIG[index];
        if (!scanner_uart_line_framer_init(
                &slots->slot[index].framer,
                slots->slot[index].storage,
                sizeof(slots->slot[index].storage))) {
            return false;
        }
    }
    return true;
}

scanner_uart_line_event_t backend_uart_slot_consume(
    backend_uart_slots_t *slots,
    size_t slot_index,
    const uint8_t *bytes,
    size_t byte_len,
    size_t *consumed_out)
{
    if (!slots || slot_index >= BACKEND_UART_SLOT_COUNT) {
        if (consumed_out) {
            *consumed_out = 0U;
        }
        return invalid_line_event();
    }
    return scanner_uart_line_framer_consume(
        &slots->slot[slot_index].framer,
        bytes,
        byte_len,
        consumed_out);
}

scanner_uart_line_event_t backend_uart_slot_expire_partial(
    backend_uart_slots_t *slots,
    size_t slot_index)
{
    if (!slots || slot_index >= BACKEND_UART_SLOT_COUNT) {
        return invalid_line_event();
    }
    return scanner_uart_line_framer_expire_partial(
        &slots->slot[slot_index].framer);
}

bool backend_uart_slot_driver_init(size_t slot_index)
{
    if (slot_index >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
#ifdef ESP_PLATFORM
    const backend_uart_slot_config_t *slot = &SLOT_CONFIG[slot_index];
    const uart_port_t uart = (uart_port_t)slot->uart;
    if (uart_is_driver_installed(uart)) {
        return true;
    }
    const uart_config_t config = {
        .baud_rate = slot->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(uart, &config) != ESP_OK ||
        uart_set_pin(
            uart,
            slot->tx_gpio,
            slot->rx_gpio,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE) != ESP_OK) {
        return false;
    }
    return uart_driver_install(
        uart,
        SCANNER_UART_LINE_BUFFER_SIZE * 2U,
        SCANNER_UART_LINE_BUFFER_SIZE * 2U,
        0,
        NULL,
        0) == ESP_OK;
#else
    return true;
#endif
}

bool backend_uart_slot_driver_reinit(
    backend_uart_slots_t *slots,
    size_t slot_index)
{
    if (!slots || slot_index >= BACKEND_UART_SLOT_COUNT) {
        return false;
    }
    scanner_uart_line_framer_reset(&slots->slot[slot_index].framer);
#ifdef ESP_PLATFORM
    const uart_port_t uart = (uart_port_t)SLOT_CONFIG[slot_index].uart;
    if (uart_is_driver_installed(uart) && uart_driver_delete(uart) != ESP_OK) {
        return false;
    }
#endif
    return backend_uart_slot_driver_init(slot_index);
}
