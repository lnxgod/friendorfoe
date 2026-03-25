/**
 * Friend or Foe — BLE Scanner Firmware Entry Point
 *
 * Dedicated BLE Remote ID scanner. Runs NimBLE full-time to capture
 * OpenDroneID advertisements (UUID 0xFFFA), fuses detections through
 * a Bayesian engine, and outputs JSON to the serial console.
 *
 * Detection flow:
 *   BLE scanner ──▶ detection_queue ──▶ Console output task
 *                        (50 items)       │
 *                                         ├── Bayesian fusion
 *                                         └── JSON → printf (console)
 */

#include "ble_remote_id.h"
#include "bayesian_fusion.h"
#include "console_output.h"
#include "detection_types.h"
#include "core/task_priorities.h"
#include "led_status.h"
#include "oled_display.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_ble_scanner";

#define FIRMWARE_VERSION    "0.27.0-beta"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   500

/* ── BOOT button (GPIO0) ─────────────────────────────────────────────── */

#define BOOT_BUTTON_GPIO    0
#define LONG_PRESS_MS       1500

static volatile bool s_button_short = false;   /* short press detected */
static volatile bool s_button_long = false;    /* long press detected */
static volatile int64_t s_button_down_ms = 0;

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "BOOT button on GPIO%d: short=scroll, long=dismiss", BOOT_BUTTON_GPIO);
}

/** Call from display loop to poll button state (non-blocking). */
static void button_poll(void)
{
    int level = gpio_get_level(BOOT_BUTTON_GPIO);
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (level == 0) {
        /* Button pressed (active low) */
        if (s_button_down_ms == 0) {
            s_button_down_ms = now_ms;  /* record press start */
        } else if ((now_ms - s_button_down_ms) >= LONG_PRESS_MS && !s_button_long) {
            s_button_long = true;
            ESP_LOGI(TAG, "BOOT button: LONG press (dismiss/ack)");
        }
    } else {
        /* Button released */
        if (s_button_down_ms > 0) {
            int64_t held_ms = now_ms - s_button_down_ms;
            if (held_ms < LONG_PRESS_MS && !s_button_long) {
                s_button_short = true;
                ESP_LOGI(TAG, "BOOT button: SHORT press (next page)");
            }
            s_button_down_ms = 0;
            s_button_long = false;
        }
    }
}

/* ── Display update task ───────────────────────────────────────────────── */

/*
 * Display modes — user cycles through with BOOT button:
 *   PRIVACY: Show privacy/glasses detections (one per page)
 *   DRONES:  Show drone detections (one per page)
 *
 * BOOT short press: next item. When past last item, switch mode.
 * BOOT long press:  force switch mode immediately.
 */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    bool show_privacy = true;     /* true = privacy mode, false = drone mode */
    int page_index = 0;
    int cycle_counter = 0;

    #define PAGE_CYCLE_TICKS  6   /* 6 × 500ms = 3s auto-advance */
    #define NUM_MODES         2

    while (1) {
        button_poll();

        /* Fetch data */
        scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
        int det_count = console_output_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

        int glasses_count = 0;
#if CONFIG_FOF_GLASSES_DETECTION
        glasses_detection_t glasses_list[GLASSES_CACHE_SIZE];
        glasses_count = console_output_get_cached_glasses(glasses_list, GLASSES_CACHE_SIZE);
#endif

        int current_max = show_privacy ? glasses_count : det_count;

        /* ── Short press: next page, wrap to other mode ─────────── */
        if (s_button_short) {
            s_button_short = false;
            page_index++;
            if (page_index >= current_max || current_max == 0) {
                /* Wrap to other mode */
                show_privacy = !show_privacy;
                page_index = 0;
                ESP_LOGI(TAG, "Mode: %s", show_privacy ? "PRIVACY" : "DRONES");
            }
            cycle_counter = 0;
        }

        /* ── Long press: force switch mode ──────────────────────── */
        if (s_button_long) {
            s_button_long = false;
            show_privacy = !show_privacy;
            page_index = 0;
            cycle_counter = 0;
            ESP_LOGI(TAG, "Mode switch: %s", show_privacy ? "PRIVACY" : "DRONES");
        }

        /* ── Render current mode ────────────────────────────────── */
#if CONFIG_FOF_GLASSES_DETECTION
        if (show_privacy && glasses_count > 0) {
            if (page_index >= glasses_count) page_index = 0;
            oled_show_glasses_alert(
                glasses_list[page_index].device_type,
                glasses_list[page_index].manufacturer,
                glasses_list[page_index].device_name,
                glasses_list[page_index].rssi,
                glasses_list[page_index].has_camera
            );
        } else if (show_privacy && glasses_count == 0) {
            /* No privacy devices — show placeholder then auto-switch */
            oled_draw_drone_list(NULL, 0, 0);
            show_privacy = false;
            page_index = 0;
        } else
#endif
        {
            /* Drone mode */
            oled_drone_entry_t oled_drones[DETECTION_CACHE_SIZE];
            for (int i = 0; i < det_count; i++) {
                oled_drones[i].id   = det_list[i].drone_id;
                oled_drones[i].lat  = det_list[i].latitude;
                oled_drones[i].lon  = det_list[i].longitude;
                oled_drones[i].alt_m = det_list[i].altitude_m;
                oled_drones[i].speed_mps = det_list[i].speed_mps;
                oled_drones[i].rssi = det_list[i].rssi;
            }
            if (page_index >= det_count && det_count > 0) page_index = 0;
            oled_draw_drone_list(oled_drones, det_count, page_index);
        }

        /* ── Auto-advance page every 3s ─────────────────────────── */
        cycle_counter++;
        if (cycle_counter >= PAGE_CYCLE_TICKS) {
            cycle_counter = 0;
            int max_pages = show_privacy ? glasses_count : det_count;
            if (max_pages > 1) {
                page_index = (page_index + 1) % max_pages;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* ── 1. Initialize NVS flash ──────────────────────────────────────── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. Create shared detection queue ─────────────────────────────── */
    QueueHandle_t detection_queue = xQueueCreate(DETECTION_QUEUE_LEN,
                                                 sizeof(drone_detection_t));
    if (detection_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create detection queue -- halting");
        return;
    }
    ESP_LOGI(TAG, "Detection queue created (%d slots, %u bytes each)",
             DETECTION_QUEUE_LEN, (unsigned)sizeof(drone_detection_t));

    /* ── 3. Initialize Bayesian fusion engine ─────────────────────────── */
    bayesian_fusion_init();
    ESP_LOGI(TAG, "Bayesian fusion engine initialised");

    /* ── 4. Initialize status LED + BOOT button ──────────────────────── */
    led_init();
    led_set_pattern(LED_BOOT);
    button_init();

    /* ── 5. Initialize OLED display ─────────────────────────────────── */
    oled_init();
    oled_set_version(FIRMWARE_VERSION);
    oled_update(0, 0, 0, 0, 0, 0);

    /* ── 6. Initialize BLE Remote ID scanner ──────────────────────────── */
    ble_remote_id_init(detection_queue);
    ESP_LOGI(TAG, "BLE Remote ID scanner initialised");

#if CONFIG_FOF_GLASSES_DETECTION
    /* ── 6b. Create glasses detection queue and attach ──────────────── */
    QueueHandle_t glasses_queue = xQueueCreate(20, sizeof(glasses_detection_t));
    if (glasses_queue != NULL) {
        ble_remote_id_set_glasses_queue(glasses_queue);
        console_output_set_glasses_queue(glasses_queue);
        bool glasses_on = glasses_detection_is_enabled();
        ESP_LOGI(TAG, "Smart glasses detection: %s", glasses_on ? "ENABLED" : "DISABLED");
    }
#endif

    /* ── 7. Start console output task ─────────────────────────────────── */
    console_output_start(detection_queue);

    /* ── 8. Start BLE scanner ─────────────────────────────────────────── */
    ble_remote_id_start();
    ESP_LOGI(TAG, "BLE Remote ID scanner started on core %d, priority %d",
             BLE_SCAN_TASK_CORE, BLE_SCAN_TASK_PRIORITY);

    /* ── 9. Start LED task ───────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_SCANNING);

    /* ── 10. Start display task ─────────────────────────────────────── */
    xTaskCreatePinnedToCore(
        display_task,
        "display",
        DISPLAY_TASK_STACK_SIZE,
        NULL,
        DISPLAY_TASK_PRIORITY,
        NULL,
        DISPLAY_TASK_CORE
    );
    ESP_LOGI(TAG, "Display task started on core %d, priority %d",
             DISPLAY_TASK_CORE, DISPLAY_TASK_PRIORITY);

    /* ── 11. Startup banner ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe — BLE Scanner v%s", FIRMWARE_VERSION);
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
#elif CONFIG_IDF_TARGET_ESP32C5
    ESP_LOGI(TAG, "  ESP32-C5 single-core RISC-V @ 240 MHz");
#elif CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "  ESP32-C3 single-core RISC-V @ 160 MHz");
#elif CONFIG_IDF_TARGET_ESP32
    ESP_LOGI(TAG, "  ESP32 dual-core Xtensa @ 240 MHz");
#endif
    ESP_LOGI(TAG, "  BLE Remote ID (OpenDroneID 0xFFFA)");
#if CONFIG_FOF_GLASSES_DETECTION
    ESP_LOGI(TAG, "  Smart glasses / privacy device detection: ON");
#endif
    ESP_LOGI(TAG, "  Console JSON output");
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
