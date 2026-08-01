#include "badge_con_vhci.h"

#if !defined(FOF_DC34_GAME_CANARY)
#error "badge_con_vhci is private to the explicit game canary"
#endif

#include "badge_con_radio_runtime_policy.h"
#include "badge_con_runtime.h"
#include "psram_alloc.h"

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#define BADGE_CON_VHCI_EVENT_BYTES 16U
#define BADGE_CON_VHCI_COMMAND_BYTES 36U

static badge_con_vhci_policy_t s_policy;
static uint8_t s_pending_event[BADGE_CON_VHCI_EVENT_BYTES];
static uint8_t s_tx_command[BADGE_CON_VHCI_COMMAND_BYTES];
static uint16_t s_pending_event_size;
static bool s_event_pending;
static bool s_event_overflow;
static bool s_init_attempted;
static bool s_init_in_progress;
static bool s_policy_ready;
static badge_con_vhci_epoch_gate_t s_epoch_gate;
static StaticSemaphore_t s_policy_mutex_storage;
static SemaphoreHandle_t s_policy_mutex;
static portMUX_TYPE s_event_lock = portMUX_INITIALIZER_UNLOCKED;

static bool policy_lock(void)
{
    return s_policy_mutex &&
           xSemaphoreTake(s_policy_mutex, portMAX_DELAY) == pdTRUE;
}

static bool policy_try_lock(void)
{
    return s_policy_mutex &&
           xSemaphoreTake(s_policy_mutex, 0) == pdTRUE;
}

static void policy_unlock(void)
{
    xSemaphoreGive(s_policy_mutex);
}

bool badge_con_vhci_prepare(void)
{
    if (s_policy_mutex) {
        return true;
    }
    s_policy_mutex = xSemaphoreCreateMutexStatic(&s_policy_mutex_storage);
    return s_policy_mutex != NULL;
}

static void notify_host_send_available(void)
{
}

static int notify_host_recv(uint8_t *data, uint16_t len)
{
    if (!data || len < 2U || data[0] != 0x04U ||
        (data[1] != 0x0EU && data[1] != 0x0FU)) {
        return 0;
    }

    portENTER_CRITICAL(&s_event_lock);
    if (len > sizeof(s_pending_event) || s_event_pending) {
        s_event_overflow = true;
    } else {
        memcpy(s_pending_event, data, len);
        s_pending_event_size = len;
        s_event_pending = true;
    }
    portEXIT_CRITICAL(&s_event_lock);
    return 0;
}

static bool send_hci_command(void *context,
                             const uint8_t *bytes,
                             size_t size)
{
    (void)context;
    if (!bytes || size == 0U || size > sizeof(s_tx_command) ||
        !esp_vhci_host_check_send_available()) {
        return false;
    }
    memcpy(s_tx_command, bytes, size);
    esp_vhci_host_send_packet(s_tx_command, (uint16_t)size);
    return true;
}

static bool fail_initialization(const char *failure)
{
    if (!policy_lock()) {
        return false;
    }
    if (s_policy_ready) {
        badge_con_vhci_policy_fail(&s_policy, failure);
    }
    s_init_in_progress = false;
    policy_unlock();
    return false;
}

bool badge_con_vhci_init(uint32_t peer, uint8_t session)
{
    if (!policy_lock()) {
        return false;
    }
    if (s_init_attempted) {
        badge_con_vhci_snapshot_t snapshot = {0};
        if (s_policy_ready) {
            badge_con_vhci_policy_snapshot(&s_policy, &snapshot);
        }
        bool ready = snapshot.controller_initialized &&
                     snapshot.state != BADGE_CON_VHCI_FAILED;
        policy_unlock();
        return ready;
    }
    if (!s_epoch_gate.observed || s_epoch_gate.inhibited) {
        policy_unlock();
        return false;
    }
    s_init_attempted = true;
    s_init_in_progress = true;

    uint8_t initial_sequence = 0U;
    badge_con_vhci_transport_t transport = {
        .context = NULL,
        .send = send_hci_command,
    };
    if (!badge_con_runtime_sequence_start(&initial_sequence)) {
        badge_con_vhci_policy_init(
            &s_policy, peer, session, 0U, &transport);
        s_policy_ready = true;
        badge_con_vhci_policy_fail(&s_policy, "sequence");
        s_init_in_progress = false;
        policy_unlock();
        return false;
    }
    badge_con_vhci_policy_init(
        &s_policy, peer, session, initial_sequence, &transport);
    s_policy_ready = true;
    badge_con_vhci_policy_set_inhibited(
        &s_policy, s_epoch_gate.inhibited);
    policy_unlock();

    uint32_t internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t largest_internal = (uint32_t)heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    badge_con_radio_memory_gate_t memory_gate =
        badge_con_radio_runtime_memory_gate(
            internal_free,
            largest_internal,
            psram_available(),
            (uint32_t)psram_total_size(),
            (uint32_t)psram_free_size());
    if (memory_gate == BADGE_CON_RADIO_MEMORY_PSRAM) {
        return fail_initialization("psram_gate");
    }
    if (memory_gate == BADGE_CON_RADIO_MEMORY_INTERNAL) {
        return fail_initialization("internal_heap_gate");
    }

    esp_bt_controller_config_t controller_config =
        BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    /* The game emits only legacy, non-connectable advertisements. Extended
     * BLE 5 controller state costs scarce internal SRAM and is never used. */
    controller_config.ble_50_feat_supp = false;
    if (esp_bt_controller_init(&controller_config) != ESP_OK) {
        return fail_initialization("controller_init");
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        return fail_initialization("controller_enable");
    }

    static const esp_vhci_host_callback_t callbacks = {
        .notify_host_send_available = notify_host_send_available,
        .notify_host_recv = notify_host_recv,
    };
    if (esp_vhci_host_register_callback(&callbacks) != ESP_OK) {
        return fail_initialization("callback_register");
    }
    if (!policy_lock()) {
        return false;
    }
    badge_con_vhci_policy_set_controller_initialized(&s_policy, true);
    badge_con_vhci_policy_set_inhibited(
        &s_policy, s_epoch_gate.inhibited);
    s_init_in_progress = false;
    badge_con_vhci_snapshot_t snapshot = {0};
    badge_con_vhci_policy_snapshot(&s_policy, &snapshot);
    bool ready = snapshot.controller_initialized &&
                 snapshot.state != BADGE_CON_VHCI_FAILED;
    policy_unlock();
    return ready;
}

void badge_con_vhci_set_identity_state(
    badge_con_role_t role, bool super)
{
    if (!policy_lock()) {
        return;
    }
    if (s_policy_ready) {
        badge_con_vhci_policy_set_identity_state(
            &s_policy, role, super);
    }
    policy_unlock();
}

void badge_con_vhci_set_game_active(bool active)
{
    if (!policy_lock()) {
        return;
    }
    if (s_policy_ready) {
        badge_con_vhci_policy_set_game_active(&s_policy, active);
    }
    policy_unlock();
}

void badge_con_vhci_set_self_ready(bool ready)
{
    if (!policy_lock()) {
        return;
    }
    if (s_policy_ready) {
        badge_con_vhci_policy_set_self_ready(&s_policy, ready);
    }
    policy_unlock();
}

bool badge_con_vhci_apply_radio_policy(
    bool inhibited, uint32_t operation_epoch)
{
    if (!policy_lock()) {
        return false;
    }
    bool accepted = badge_con_vhci_epoch_gate_apply(
        &s_epoch_gate, operation_epoch, inhibited);
    if (accepted && s_policy_ready) {
        badge_con_vhci_policy_set_inhibited(
            &s_policy, s_epoch_gate.inhibited);
    }
    policy_unlock();
    return accepted;
}

bool badge_con_vhci_request_quiescence(uint32_t operation_epoch)
{
    if (!policy_try_lock()) {
        return false;
    }
    bool accepted = badge_con_vhci_epoch_gate_apply(
        &s_epoch_gate, operation_epoch, true);
    if (accepted && s_policy_ready) {
        badge_con_vhci_policy_set_inhibited(
            &s_policy, s_epoch_gate.inhibited);
    }
    policy_unlock();
    return accepted;
}

void badge_con_vhci_poll(uint32_t now_ms)
{
    uint8_t event[BADGE_CON_VHCI_EVENT_BYTES] = {0};
    uint16_t event_size = 0U;
    bool overflow;

    portENTER_CRITICAL(&s_event_lock);
    overflow = s_event_overflow;
    s_event_overflow = false;
    if (s_event_pending) {
        event_size = s_pending_event_size;
        memcpy(event, s_pending_event, event_size);
        s_pending_event_size = 0U;
        s_event_pending = false;
    }
    portEXIT_CRITICAL(&s_event_lock);

    if (!policy_lock()) {
        return;
    }
    if (!s_policy_ready) {
        policy_unlock();
        return;
    }
    if (overflow) {
        badge_con_vhci_policy_fail(&s_policy, "event_overflow");
    } else if (event_size != 0U) {
        badge_con_vhci_policy_on_hci_event(
            &s_policy, event, event_size, now_ms);
    }
    badge_con_vhci_policy_poll(&s_policy, now_ms);
    policy_unlock();
}

void badge_con_vhci_snapshot(badge_con_vhci_snapshot_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = BADGE_CON_VHCI_FAILED;
    out->inhibited = true;
    out->failure = "unavailable";
    if (!policy_lock()) {
        return;
    }
    if (s_policy_ready) {
        badge_con_vhci_policy_snapshot(&s_policy, out);
    }
    policy_unlock();
}

bool badge_con_vhci_radio_quiesced_for_epoch(uint32_t operation_epoch)
{
    if (!policy_try_lock()) {
        return false;
    }
    bool exact_inhibit = badge_con_vhci_epoch_gate_matches_inhibit(
        &s_epoch_gate, operation_epoch);
    bool quiesced = exact_inhibit && !s_init_in_progress &&
        (!s_policy_ready ||
         badge_con_vhci_policy_radio_quiesced(&s_policy));
    policy_unlock();
    return quiesced;
}
