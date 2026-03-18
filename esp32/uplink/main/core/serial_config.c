/**
 * Friend or Foe -- Serial Configuration Handler
 *
 * Reads config commands from the USB console (stdin) during a startup
 * window, allowing the web flasher to push NVS settings right after flash.
 */

#include "serial_config.h"
#include "nvs_config.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "serial_cfg";

#define LINE_BUF_SIZE   256
#define CMD_PREFIX      "FOF_SET:"
#define CMD_SAVE        "FOF_SAVE"
#define RESP_READY      "FOF_READY\n"
#define RESP_OK         "FOF_OK:"
#define RESP_SAVED      "FOF_SAVED\n"
#define RESP_ERROR      "FOF_ERROR:"
#define RESP_TIMEOUT    "FOF_TIMEOUT\n"

/* Allowed NVS keys — only accept known config keys */
static const char *ALLOWED_KEYS[] = {
    "wifi_ssid", "wifi_pass", "backend_url", "device_id", NULL
};

static bool is_allowed_key(const char *key)
{
    for (int i = 0; ALLOWED_KEYS[i] != NULL; i++) {
        if (strcmp(key, ALLOWED_KEYS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void send_response(const char *msg)
{
    printf("%s", msg);
    fflush(stdout);
}

/**
 * Try to read one line from stdin with a timeout.
 * Returns the number of characters read, or 0 on timeout.
 */
static int read_line(char *buf, int buf_size, int timeout_ms)
{
    int pos = 0;
    int elapsed = 0;
    const int poll_interval = 50;

    memset(buf, 0, buf_size);

    while (elapsed < timeout_ms && pos < buf_size - 1) {
        int len = usb_serial_jtag_read_bytes(
            (uint8_t *)(buf + pos), 1, pdMS_TO_TICKS(poll_interval));

        if (len > 0) {
            if (buf[pos] == '\n' || buf[pos] == '\r') {
                buf[pos] = '\0';
                if (pos > 0) {
                    return pos;
                }
                /* Skip empty lines / lone CR/LF */
                continue;
            }
            pos++;
            /* Reset timeout on each received character */
            elapsed = 0;
        } else {
            elapsed += poll_interval;
        }
    }

    buf[pos] = '\0';
    return pos;
}

static bool handle_set_command(const char *line)
{
    /* Expected format: FOF_SET:key=value */
    const char *payload = line + strlen(CMD_PREFIX);

    /* Find the '=' separator */
    const char *eq = strchr(payload, '=');
    if (!eq || eq == payload) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%smalformed command\n", RESP_ERROR);
        send_response(msg);
        return false;
    }

    /* Extract key */
    int key_len = eq - payload;
    char key[32] = {0};
    if (key_len >= (int)sizeof(key)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%skey too long\n", RESP_ERROR);
        send_response(msg);
        return false;
    }
    memcpy(key, payload, key_len);
    key[key_len] = '\0';

    /* Validate key */
    if (!is_allowed_key(key)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%sunknown key '%s'\n", RESP_ERROR, key);
        send_response(msg);
        return false;
    }

    /* Extract value */
    const char *value = eq + 1;

    /* Write to NVS */
    if (nvs_config_set_string(key, value)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s%s\n", RESP_OK, key);
        send_response(msg);
        ESP_LOGI(TAG, "Set %s = %s", key,
                 strcmp(key, "wifi_pass") == 0 ? "****" : value);
        return true;
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "%sNVS write failed for '%s'\n",
                 RESP_ERROR, key);
        send_response(msg);
        return false;
    }
}

bool serial_config_listen(int timeout_ms)
{
    char line[LINE_BUF_SIZE];
    bool any_saved = false;

    /* Install USB-JTAG serial driver for reading */
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB serial driver install failed: %s (may already be installed)",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Config mode: waiting %dms for serial commands...", timeout_ms);
    send_response(RESP_READY);

    /* Wait for first command */
    int n = read_line(line, sizeof(line), timeout_ms);
    if (n == 0) {
        ESP_LOGI(TAG, "No serial config received, continuing boot");
        send_response(RESP_TIMEOUT);
        goto cleanup;
    }

    /* Process commands until SAVE or timeout */
    while (1) {
        if (strncmp(line, CMD_PREFIX, strlen(CMD_PREFIX)) == 0) {
            if (handle_set_command(line)) {
                any_saved = true;
            }
        } else if (strcmp(line, CMD_SAVE) == 0) {
            send_response(RESP_SAVED);
            ESP_LOGI(TAG, "Configuration saved to NVS");
            break;
        } else if (strlen(line) > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%sunknown command\n", RESP_ERROR);
            send_response(msg);
        }

        /* Read next line with 5-second inter-command timeout */
        n = read_line(line, sizeof(line), 5000);
        if (n == 0) {
            ESP_LOGI(TAG, "Config timeout (inter-command), continuing boot");
            send_response(RESP_TIMEOUT);
            break;
        }
    }

cleanup:
    /* Uninstall driver so normal logging continues */
    usb_serial_jtag_driver_uninstall();
    return any_saved;
}
