/**
 * Friend or Foe -- Serial Configuration Handler
 *
 * Reads config commands from the USB console (stdin) during a startup
 * window, allowing the web flasher to push NVS settings right after flash.
 *
 * Uses standard stdin/stdout via VFS — works with whatever console backend
 * is configured (USB-JTAG on C3, UART0 on other chips).
 */

#include "serial_config.h"
#include "nvs_config.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

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
    "wifi_ssid", "wifi_pass", "backend_url", "device_id",
    "ap_ssid", "ap_pass",
    NULL
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
 * Check if data is available on stdin within timeout_ms.
 * Returns true if data is ready to read.
 */
static bool stdin_has_data(int timeout_ms)
{
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    return (ret > 0 && FD_ISSET(STDIN_FILENO, &fds));
}

/**
 * Read one line from stdin with a timeout.
 * Returns the number of characters read, or 0 on timeout.
 */
static int read_line(char *buf, int buf_size, int timeout_ms)
{
    int pos = 0;
    int elapsed = 0;
    const int poll_interval = 50;

    memset(buf, 0, buf_size);

    while (elapsed < timeout_ms && pos < buf_size - 1) {
        if (stdin_has_data(poll_interval)) {
            int ch = fgetc(stdin);
            if (ch == EOF) {
                elapsed += poll_interval;
                continue;
            }

            if (ch == '\n' || ch == '\r') {
                buf[pos] = '\0';
                if (pos > 0) {
                    return pos;
                }
                /* Skip empty lines / lone CR/LF */
                continue;
            }

            buf[pos++] = (char)ch;
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
        char msg[96];
        snprintf(msg, sizeof(msg), "%smalformed command\n", RESP_ERROR);
        send_response(msg);
        return false;
    }

    /* Extract key */
    int key_len = eq - payload;
    char key[32] = {0};
    if (key_len >= (int)sizeof(key)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%skey too long\n", RESP_ERROR);
        send_response(msg);
        return false;
    }
    memcpy(key, payload, key_len);
    key[key_len] = '\0';

    /* Validate key */
    if (!is_allowed_key(key)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%sunknown key '%s'\n", RESP_ERROR, key);
        send_response(msg);
        return false;
    }

    /* Extract value */
    const char *value = eq + 1;

    /* Write to NVS */
    if (nvs_config_set_string(key, value)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s%s\n", RESP_OK, key);
        send_response(msg);
        ESP_LOGI(TAG, "Set %s = %s", key,
                 (strcmp(key, "wifi_pass") == 0 || strcmp(key, "ap_pass") == 0)
                 ? "****" : value);
        return true;
    } else {
        char msg[96];
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

    /* Make stdin non-blocking friendly via VFS select */
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

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
            char msg[96];
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
    /* Restore blocking mode */
    fcntl(STDIN_FILENO, F_SETFL, 0);
    return any_saved;
}
