/* Disposable FoF badge factory topology probe.
 *
 * Every blank XIAO ESP32-S3 receives this same image. The assembled uplink
 * position has peers on UART link A (GPIO1/2) and link B (GPIO3/4), while
 * each scanner position has only link A. A nonce-bound reciprocal graph lets
 * the host assign production images without relying on USB port order. */

#include "factory_probe_protocol.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define PROBE_UART_BAUD          115200
#define PROBE_UART_RX_BYTES      1024
#define PROBE_BROADCAST_MS       200
#define PROBE_REPORT_MS          750
#define PROBE_USB_LINE_MAX       96

typedef struct {
    uart_port_t uart;
    int tx_pin;
    int rx_pin;
    char link;
} probe_link_t;

static const probe_link_t s_links[2] = {
    { UART_NUM_1, 1, 2, 'a' },
    { UART_NUM_2, 3, 4, 'b' },
};

static char s_mac[FOF_FACTORY_PROBE_MAC_TEXT + 1] = {0};
static char s_session[FOF_FACTORY_PROBE_SESSION_HEX + 1] = {0};
static fof_factory_probe_peer_table_t s_peers = {0};
static uint32_t s_sequence = 0;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;


static void format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


static bool session_snapshot(char out[FOF_FACTORY_PROBE_SESSION_HEX + 1])
{
    bool active = false;
    portENTER_CRITICAL(&s_state_lock);
    if (s_session[0] != '\0') {
        memcpy(out, s_session, sizeof(s_session));
        active = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}


static void publish_report(void)
{
    char session[FOF_FACTORY_PROBE_SESSION_HEX + 1] = {0};
    fof_factory_probe_peer_table_t peers = {0};
    portENTER_CRITICAL(&s_state_lock);
    memcpy(session, s_session, sizeof(session));
    peers = s_peers;
    portEXIT_CRITICAL(&s_state_lock);
    if (!fof_factory_probe_session_valid(session)) {
        return;
    }

    char report[FOF_FACTORY_PROBE_REPORT_MAX] = {0};
    if (fof_factory_probe_report_build(
            s_mac, session, &peers, report, sizeof(report))) {
        fputs(report, stdout);
        fflush(stdout);
    }
}


static void observe_line(char received_link, const char *line)
{
    fof_factory_probe_frame_t frame = {0};
    if (!fof_factory_probe_frame_parse(line, &frame)) {
        return;
    }
    char session[FOF_FACTORY_PROBE_SESSION_HEX + 1] = {0};
    if (!session_snapshot(session)) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    (void)fof_factory_probe_peer_observe(
        &s_peers, s_mac, session, received_link, &frame);
    portEXIT_CRITICAL(&s_state_lock);
}


static void probe_link_task(void *arg)
{
    const probe_link_t *link = (const probe_link_t *)arg;
    char rx_line[FOF_FACTORY_PROBE_FRAME_MAX] = {0};
    size_t rx_length = 0;

    while (true) {
        char session[FOF_FACTORY_PROBE_SESSION_HEX + 1] = {0};
        if (session_snapshot(session)) {
            fof_factory_probe_frame_t frame = {0};
            memcpy(frame.session, session, sizeof(frame.session));
            memcpy(frame.mac, s_mac, sizeof(frame.mac));
            frame.link = link->link;
            portENTER_CRITICAL(&s_state_lock);
            frame.sequence = ++s_sequence;
            portEXIT_CRITICAL(&s_state_lock);

            char encoded[FOF_FACTORY_PROBE_FRAME_MAX] = {0};
            if (fof_factory_probe_frame_encode(
                    &frame, encoded, sizeof(encoded))) {
                uart_write_bytes(link->uart, encoded, strlen(encoded));
            }
        }

        uint8_t incoming[128] = {0};
        int received = uart_read_bytes(
            link->uart, incoming, sizeof(incoming),
            pdMS_TO_TICKS(PROBE_BROADCAST_MS));
        for (int i = 0; i < received; ++i) {
            char value = (char)incoming[i];
            if (value == '\n' || value == '\r') {
                if (rx_length > 0) {
                    rx_line[rx_length] = '\0';
                    observe_line(link->link, rx_line);
                    rx_length = 0;
                }
            } else if (rx_length + 1 < sizeof(rx_line)) {
                rx_line[rx_length++] = value;
            } else {
                rx_length = 0;
            }
        }
    }
}


static void report_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_REPORT_MS));
        publish_report();
    }
}


static void apply_session_command(const char *line)
{
    static const char prefix[] = "FOF_PROBE_SESSION:";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) {
        if (strcmp(line, "FOF_PROBE_ID") == 0) {
            printf("FOF_FACTORY_READY:%s:%d\n",
                   s_mac, FOF_FACTORY_PROBE_SCHEMA);
            fflush(stdout);
            return;
        }
        if (strcmp(line, "FOF_PROBE_REPORT") == 0) {
            publish_report();
        }
        return;
    }
    const char *session = line + sizeof(prefix) - 1;
    if (!fof_factory_probe_session_valid(session)) {
        printf("FOF_FACTORY_ERROR:invalid_session\n");
        fflush(stdout);
        return;
    }

    portENTER_CRITICAL(&s_state_lock);
    memcpy(s_session, session, sizeof(s_session));
    memset(&s_peers, 0, sizeof(s_peers));
    s_sequence = 0;
    portEXIT_CRITICAL(&s_state_lock);
    printf("FOF_FACTORY_SESSION_OK:%s:%s\n", s_mac, session);
    fflush(stdout);
}


static void usb_command_task(void *arg)
{
    (void)arg;
    char line[PROBE_USB_LINE_MAX] = {0};
    size_t length = 0;
    while (true) {
        char chunk[32] = {0};
        ssize_t received = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (ssize_t i = 0; i < received; ++i) {
            char value = chunk[i];
            if (value == '\n' || value == '\r') {
                if (length > 0) {
                    line[length] = '\0';
                    apply_session_command(line);
                    length = 0;
                }
            } else if (length + 1 < sizeof(line)) {
                line[length++] = value;
            } else {
                length = 0;
            }
        }
    }
}


static void init_link(const probe_link_t *link)
{
    const uart_config_t config = {
        .baud_rate = PROBE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(link->uart, &config));
    ESP_ERROR_CHECK(uart_set_pin(
        link->uart, link->tx_pin, link->rx_pin,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(
        link->uart, PROBE_UART_RX_BYTES, 512, 0, NULL, 0));
}


void app_main(void)
{
    uint8_t base_mac[6] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(base_mac));
    format_mac(base_mac, s_mac);

    for (size_t i = 0; i < 2; ++i) {
        init_link(&s_links[i]);
    }

    printf("FOF_FACTORY_READY:%s:%d\n", s_mac, FOF_FACTORY_PROBE_SCHEMA);
    fflush(stdout);

    xTaskCreate(probe_link_task, "probe_a", 3072,
                (void *)&s_links[0], 3, NULL);
    xTaskCreate(probe_link_task, "probe_b", 3072,
                (void *)&s_links[1], 3, NULL);
    xTaskCreate(report_task, "probe_report", 3072, NULL, 2, NULL);
    xTaskCreate(usb_command_task, "probe_usb", 3072, NULL, 4, NULL);
}
