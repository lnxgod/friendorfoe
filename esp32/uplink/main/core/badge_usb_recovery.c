#include "badge_usb_recovery.h"

#include "badge_runtime.h"
#include "badge_usb_transport.h"
#include "fw_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

static const char *TAG = "badge_usb_recovery";

static _Noreturn void park_after_irreversible_restart_failure(
    const char *message)
{
    ESP_LOGE(TAG, "%s", message ? message : "restart ownership failure");
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

badge_usb_firmware_restart_prepare_result_t
badge_usb_recovery_prepare_firmware_restart(
    const char *reason,
    badge_runtime_expected_reboot_target_t expected_target,
    badge_runtime_expected_reboot_lease_t *out_lease)
{
    if (!out_lease) {
        return BADGE_USB_FIRMWARE_RESTART_PREPARE_FAILED;
    }
    memset(out_lease, 0, sizeof(*out_lease));

    badge_runtime_expected_reboot_arm_result_t arm_result =
        badge_runtime_arm_expected_reboot(
            reason, expected_target, out_lease);
    if (arm_result != BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED) {
        return arm_result == BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY
            ? BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY
            : BADGE_USB_FIRMWARE_RESTART_PREPARE_FAILED;
    }

    bool firmware_reserved =
        fw_store_try_reserve_recovery_restart();
    if (!firmware_reserved) {
        /*
         * Never wait for firmware ownership while retaining reboot
         * ownership. A current firmware owner may need the reboot lease to
         * finish its own non-returning handoff.
         */
        if (!badge_runtime_release_expected_reboot(out_lease)) {
            ESP_LOGE(
                TAG,
                "Firmware restart preparation could not release reboot "
                "ownership");
            return BADGE_USB_FIRMWARE_RESTART_PREPARE_FAILED;
        }
        memset(out_lease, 0, sizeof(*out_lease));
        return BADGE_USB_FIRMWARE_RESTART_PREPARE_BUSY;
    }

    /*
     * The firmware reservation has no release path. From here onward a lost
     * expected-reboot lease cannot safely return to the running application.
     */
    if (!badge_runtime_expected_reboot_lease_is_owned(out_lease)) {
        park_after_irreversible_restart_failure(
            "Firmware restart reservation lost expected-reboot ownership");
    }
    return BADGE_USB_FIRMWARE_RESTART_PREPARE_OWNED;
}

_Noreturn void badge_usb_recovery_restart_with_owned_lease(
    badge_usb_reset_target_t target,
    const char *reason,
    const badge_runtime_expected_reboot_lease_t *lease)
{
    if (!badge_runtime_expected_reboot_lease_is_owned(lease)) {
        park_after_irreversible_restart_failure(
            "Owned USB recovery executor entered without reboot ownership");
    }
    if (reason &&
        (strcmp(reason, "usb_safe_once") == 0 ||
         strcmp(reason, "uart_start_token_release") == 0)) {
        badge_runtime_arm_usb_recovery_once();
    }

    if (target == BADGE_USB_RESET_ROM) {
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    }

    ESP_LOGW(TAG, "Expected USB recovery restart target=%s reason=%s",
             target == BADGE_USB_RESET_ROM ? "rom" : "app",
             reason ? reason : "planned");
    vTaskDelay(pdMS_TO_TICKS(120));
    if (!badge_runtime_expected_reboot_lease_is_owned(lease)) {
        park_after_irreversible_restart_failure(
            "Owned USB recovery executor lost reboot ownership");
    }
    esp_restart();
    park_after_irreversible_restart_failure(
        "Owned USB recovery executor returned from esp_restart");
}

bool badge_usb_recovery_restart(
    badge_usb_reset_target_t target, const char *reason)
{
    badge_runtime_expected_reboot_lease_t lease = {0};

    if (target == BADGE_USB_RESET_ROM &&
        !badge_usb_transport_drain(pdMS_TO_TICKS(250))) {
        ESP_LOGE(TAG, "ROM recovery output did not drain; continuing to ROM");
    }
    badge_runtime_expected_reboot_arm_result_t arm_result =
        badge_runtime_arm_expected_reboot(
            reason,
            BADGE_RUNTIME_EXPECTED_REBOOT_TARGET_CURRENT,
            &lease);
    if (arm_result != BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_OWNED) {
        ESP_LOGE(
            TAG,
            "USB recovery restart blocked: expected-reboot owner=%s",
            arm_result == BADGE_RUNTIME_EXPECTED_REBOOT_ARM_RESULT_BUSY
                ? "busy"
                : "failed");
        return false;
    }
    badge_usb_recovery_restart_with_owned_lease(
        target, reason, &lease);
}
