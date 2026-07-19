#include "badge_power_runtime.h"

#ifdef FOF_BADGE_VARIANT

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fw_store.h"
#include "oled_display.h"
#include "uart_protocol.h"
#include "uart_rx.h"

#define BADGE_POWER_REASSERT_MS 10000
#define BADGE_POWER_PENDING_RETRY_MS 500

static const char *TAG = "badge_power";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static badge_power_state_t s_state;
static bool s_initialized;
static bool s_started;
static bool s_applied_quiet;
static bool s_relay_was_active;
static int64_t s_last_send_ms[BADGE_POWER_SCANNER_COUNT];
static StaticSemaphore_t s_transition_mutex_storage;
static SemaphoreHandle_t s_transition_mutex;

static bool transition_mutex_init(void)
{
    if (!s_transition_mutex) {
        s_transition_mutex = xSemaphoreCreateMutexStatic(
            &s_transition_mutex_storage);
    }
    return s_transition_mutex != NULL;
}

static void ensure_initialized(void)
{
    if (s_initialized) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_initialized) {
        badge_power_state_init(&s_state);
        memset(s_last_send_ms, 0, sizeof(s_last_send_ms));
        s_applied_quiet = false;
        s_relay_was_active = false;
        s_initialized = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void badge_power_runtime_init(void)
{
    ensure_initialized();
    if (!transition_mutex_init()) {
        ESP_LOGE(TAG, "Power transition mutex initialization failed");
    }
    ESP_LOGI(TAG, "Power mode initialized ACTIVE (volatile; reboot restores ACTIVE)");
}

bool badge_power_runtime_start(void)
{
    ensure_initialized();
    if (!transition_mutex_init()) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    s_started = true;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool badge_power_runtime_request(bool quiet, const char *source)
{
    ensure_initialized();
    if (!s_transition_mutex ||
        xSemaphoreTake(s_transition_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Power mode request lock unavailable");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool changed = badge_power_state_request(&s_state, quiet);
    uint32_t generation = s_state.generation;
    if (changed) {
        memset(s_last_send_ms, 0, sizeof(s_last_send_ms));
    }
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_transition_mutex);
    if (changed) {
        ESP_LOGW(TAG, "Power mode requested %s generation=%lu source=%s",
                 quiet ? "QUIET" : "ACTIVE",
                 (unsigned long)generation,
                 source ? source : "unknown");
    }
    return changed;
}

bool badge_power_runtime_toggle(const char *source)
{
    ensure_initialized();
    if (!s_transition_mutex ||
        xSemaphoreTake(s_transition_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Power mode toggle lock unavailable");
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    bool quiet = !s_state.quiet;
    bool changed = badge_power_state_request(&s_state, quiet);
    uint32_t generation = s_state.generation;
    if (changed) {
        memset(s_last_send_ms, 0, sizeof(s_last_send_ms));
    }
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_transition_mutex);
    if (changed) {
        ESP_LOGW(TAG, "Power mode toggled %s generation=%lu source=%s",
                 quiet ? "QUIET" : "ACTIVE",
                 (unsigned long)generation,
                 source ? source : "unknown");
    }
    return changed;
}

bool badge_power_runtime_is_quiet(void)
{
    ensure_initialized();
    portENTER_CRITICAL(&s_lock);
    bool quiet = s_state.quiet;
    portEXIT_CRITICAL(&s_lock);
    return quiet;
}

const char *badge_power_runtime_mode_name(void)
{
    return badge_power_runtime_is_quiet() ? "quiet" : "active";
}

void badge_power_runtime_snapshot(badge_power_state_t *out)
{
    if (!out) {
        return;
    }
    ensure_initialized();
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
}

void badge_power_runtime_note_scanner_identity(int scanner_id)
{
    ensure_initialized();
    if (scanner_id < 0 || scanner_id >= BADGE_POWER_SCANNER_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    badge_power_state_note_identity(&s_state, scanner_id);
    s_last_send_ms[scanner_id] = 0;
    portEXIT_CRITICAL(&s_lock);
}

bool badge_power_runtime_note_scanner_ack(int scanner_id,
                                          bool transition_ok,
                                          bool quiet,
                                          uint32_t generation,
                                          bool tx_enabled,
                                          bool ble_scanning,
                                          bool wifi_paused,
                                          bool ble_quiesced,
                                          bool wifi_quiesced,
                                          bool ble_active,
                                          bool wifi_active,
                                          bool radios_ready,
                                          bool tx_restored,
                                          bool uart_commands)
{
    ensure_initialized();
    portENTER_CRITICAL(&s_lock);
    bool accepted = badge_power_state_note_ack(
        &s_state, scanner_id, transition_ok, quiet, generation,
        tx_enabled, ble_scanning, wifi_paused,
        ble_quiesced, wifi_quiesced, ble_active, wifi_active,
        radios_ready, tx_restored, uart_commands);
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG,
             "Scanner[%d] quiet ack accepted=%d transition_ok=%d mode=%d generation=%lu tx=%d ble=%d wifi_paused=%d ble_q=%d wifi_q=%d radios_ready=%d tx_restored=%d uart=%d",
             scanner_id, accepted ? 1 : 0, transition_ok ? 1 : 0,
             quiet ? 1 : 0, (unsigned long)generation, tx_enabled ? 1 : 0,
             ble_scanning ? 1 : 0, wifi_paused ? 1 : 0,
             ble_quiesced ? 1 : 0, wifi_quiesced ? 1 : 0,
             radios_ready ? 1 : 0, tx_restored ? 1 : 0,
             uart_commands ? 1 : 0);
    return accepted;
}

static bool scanner_connected(int scanner_id)
{
    return scanner_id == 0
        ? uart_rx_is_ble_scanner_connected()
        : uart_rx_is_wifi_scanner_connected();
}

static bool scanner_identity_known(int scanner_id)
{
    return scanner_id == 0
        ? uart_rx_get_ble_scanner_info() != NULL
        : uart_rx_get_wifi_scanner_info() != NULL;
}

static bool scanner_target_known(int scanner_id)
{
    /* Scanner identity is sticky across the 15-second RX freshness window.
     * It authorizes a probe, but never counts as a live connection or ACK. */
    return scanner_connected(scanner_id) || scanner_identity_known(scanner_id);
}

static bool badge_power_state_still_matches(int scanner_id,
                                             bool quiet,
                                             uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    bool matches = s_state.quiet == quiet &&
        s_state.generation == generation &&
        scanner_id >= 0 && scanner_id < BADGE_POWER_SCANNER_COUNT;
    portEXIT_CRITICAL(&s_lock);
    return matches;
}

static void note_relay_finished(void)
{
    bool fresh[BADGE_POWER_SCANNER_COUNT];
    for (int i = 0; i < BADGE_POWER_SCANNER_COUNT; i++) {
        fresh[i] = scanner_connected(i);
    }
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < BADGE_POWER_SCANNER_COUNT; i++) {
        /* RX freshness expires while a long upload/relay owns the UART.  A
         * scanner whose identity was already learned remains a command target
         * so the first post-operation quiet command can re-establish its ACK. */
        if (fresh[i]) {
            badge_power_state_note_identity(&s_state, i);
        } else {
            badge_power_state_note_disconnected(&s_state, i);
        }
        s_last_send_ms[i] = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "Firmware relay released UART; reasserting desired scanner power mode");
}

void badge_power_runtime_poll(void)
{
    ensure_initialized();
    portENTER_CRITICAL(&s_lock);
    bool started = s_started;
    bool desired_quiet = s_state.quiet;
    bool applied_quiet = s_applied_quiet;
    portEXIT_CRITICAL(&s_lock);
    if (!started) {
        return;
    }

    if (desired_quiet != applied_quiet) {
        oled_set_power(!desired_quiet);
        bool applied = oled_is_powered() == !desired_quiet;
        if (applied) {
            portENTER_CRITICAL(&s_lock);
            s_applied_quiet = desired_quiet;
            portEXIT_CRITICAL(&s_lock);
        }
    }

    bool relay_active = fw_store_is_relay_active();
    if (relay_active) {
        if (!s_relay_was_active) {
            /* A firmware operation can suppress scanner RX beyond its
             * freshness window. Never carry a pre-operation ACK through it. */
            portENTER_CRITICAL(&s_lock);
            for (int i = 0; i < BADGE_POWER_SCANNER_COUNT; i++) {
                badge_power_state_note_disconnected(&s_state, i);
                s_last_send_ms[i] = 0;
            }
            portEXIT_CRITICAL(&s_lock);
        }
        s_relay_was_active = true;
        return;
    }
    if (s_relay_was_active) {
        s_relay_was_active = false;
        note_relay_finished();
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    for (int scanner_id = 0; scanner_id < BADGE_POWER_SCANNER_COUNT; scanner_id++) {
        bool fresh_rx = scanner_connected(scanner_id);
        bool target_known = scanner_target_known(scanner_id);
        portENTER_CRITICAL(&s_lock);
        badge_power_scanner_state_t scanner = s_state.scanners[scanner_id];
        if (fresh_rx && !scanner.connected) {
            badge_power_state_note_identity(&s_state, scanner_id);
            scanner = s_state.scanners[scanner_id];
        } else if (!fresh_rx && scanner.connected) {
            badge_power_state_note_disconnected(&s_state, scanner_id);
            scanner = s_state.scanners[scanner_id];
        }
        int64_t retry_ms = scanner.connected && !scanner.acked
            ? BADGE_POWER_PENDING_RETRY_MS
            : BADGE_POWER_REASSERT_MS;
        bool should_send = target_known &&
            (s_last_send_ms[scanner_id] == 0 ||
             now_ms - s_last_send_ms[scanner_id] >= retry_ms);
        bool quiet = s_state.quiet;
        uint32_t generation = s_state.generation;
        if (should_send) {
            /* A periodic probe is a new proof obligation. Keep the sticky
             * target, but invalidate convergence until its fresh ACK arrives. */
            if (fresh_rx) {
                badge_power_state_note_identity(&s_state, scanner_id);
            } else {
                badge_power_state_note_disconnected(&s_state, scanner_id);
            }
        }
        portEXIT_CRITICAL(&s_lock);

        if (!should_send) {
            continue;
        }
        if (!s_transition_mutex ||
            xSemaphoreTake(s_transition_mutex, pdMS_TO_TICKS(150)) != pdTRUE) {
            continue;
        }
        /* Re-snapshot while requests are serialized with this send.  A mode
         * request either completes before this point or waits until the whole
         * JSON frame has left the UART; it can never split check from send. */
        portENTER_CRITICAL(&s_lock);
        quiet = s_state.quiet;
        generation = s_state.generation;
        portEXIT_CRITICAL(&s_lock);
        char command[112];
        snprintf(command, sizeof(command),
                 "{\"type\":\"%s\",\"enabled\":%s,\"generation\":%lu}",
                 MSG_TYPE_SCANNER_QUIET,
                 quiet ? "true" : "false",
                 (unsigned long)generation);
        /* Firmware staging/relay owns this same recursive lease for its full
         * binary session.  Acquiring it here, then rechecking state, closes
         * both directions of the JSON-versus-binary interleaving race. */
        if (!uart_rx_scanner_tx_lease_acquire(pdMS_TO_TICKS(100))) {
            xSemaphoreGive(s_transition_mutex);
            continue;
        }
        bool valid = !fw_store_is_relay_active() &&
            badge_power_state_still_matches(scanner_id, quiet, generation);
        bool sent = valid &&
            uart_rx_send_command_to_scanner_checked(scanner_id, command);
        uart_rx_scanner_tx_lease_release();
        xSemaphoreGive(s_transition_mutex);

        if (sent) {
            portENTER_CRITICAL(&s_lock);
            if (s_state.quiet == quiet && s_state.generation == generation) {
                s_last_send_ms[scanner_id] = now_ms;
            }
            portEXIT_CRITICAL(&s_lock);
        }
    }
}

#else

void badge_power_runtime_init(void) {}
bool badge_power_runtime_start(void) { return true; }
void badge_power_runtime_poll(void) {}
bool badge_power_runtime_request(bool quiet, const char *source)
{
    (void)quiet;
    (void)source;
    return false;
}
bool badge_power_runtime_toggle(const char *source)
{
    (void)source;
    return false;
}
bool badge_power_runtime_is_quiet(void) { return false; }
const char *badge_power_runtime_mode_name(void) { return "active"; }
void badge_power_runtime_snapshot(badge_power_state_t *out)
{
    badge_power_state_init(out);
}
void badge_power_runtime_note_scanner_identity(int scanner_id)
{
    (void)scanner_id;
}
bool badge_power_runtime_note_scanner_ack(int scanner_id,
                                          bool transition_ok,
                                          bool quiet,
                                          uint32_t generation,
                                          bool tx_enabled,
                                          bool ble_scanning,
                                          bool wifi_paused,
                                          bool ble_quiesced,
                                          bool wifi_quiesced,
                                          bool ble_active,
                                          bool wifi_active,
                                          bool radios_ready,
                                          bool tx_restored,
                                          bool uart_commands)
{
    (void)scanner_id;
    (void)transition_ok;
    (void)quiet;
    (void)generation;
    (void)tx_enabled;
    (void)ble_scanning;
    (void)wifi_paused;
    (void)ble_quiesced;
    (void)wifi_quiesced;
    (void)ble_active;
    (void)wifi_active;
    (void)radios_ready;
    (void)tx_restored;
    (void)uart_commands;
    return false;
}

#endif
