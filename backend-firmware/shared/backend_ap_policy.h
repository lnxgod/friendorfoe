#ifndef BACKEND_AP_POLICY_H
#define BACKEND_AP_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BACKEND_AP_OUTAGE_START_MS INT64_C(300000)
#define BACKEND_AP_SUCCESS_GRACE_MS INT64_C(30000)

typedef enum {
    BACKEND_AP_NO_CHANGE = 0,
    BACKEND_AP_START,
    BACKEND_AP_STOP,
} backend_ap_action_t;

typedef struct {
    bool config_valid;
    uint32_t config_generation;
    bool backend_connected;
    bool usb_start_requested;
} backend_ap_input_t;

typedef struct {
    bool running;
    int64_t boot_ms;
    int64_t ap_started_ms;
    uint32_t ap_started_config_generation;
    uint32_t last_success_generation;
    int64_t last_success_ms;
    int64_t outage_started_ms;
} backend_ap_policy_t;

void backend_ap_policy_init(backend_ap_policy_t *policy, int64_t boot_ms);
void backend_ap_policy_note_config_commit(
    backend_ap_policy_t *policy,
    uint32_t new_generation,
    int64_t now_ms);
void backend_ap_policy_note_backend_success(
    backend_ap_policy_t *policy,
    uint32_t config_generation,
    int64_t now_ms);
backend_ap_action_t backend_ap_policy_tick(
    backend_ap_policy_t *policy,
    backend_ap_input_t input,
    int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
