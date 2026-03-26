/**
 * Friend or Foe -- WiFi + BLE Scanner Firmware Entry Point
 *
 * Dual-mode scanner: WiFi promiscuous + BLE Remote ID + privacy device
 * detection. BOOT button double-tap switches OLED between drone view
 * and privacy/glasses view.
 *
 * Architecture:
 *   Core 0 (radio):      WiFi scan task + BLE scan (NimBLE)
 *   Core 1 (processing): UART TX task (fusion runs inline)
 *
 * Detection flow:
 *   WiFi scanner ──▶ detection_queue ──▶ UART TX task
 *                         (50 items)       │
 *                                          ├── Bayesian fusion
 *                                          └── JSON → UART1 → Uplink
 *   BLE scanner  ──▶ detection_queue (drones)
 *                ──▶ glasses_queue   (privacy devices → OLED)
 */

#include "wifi_scanner.h"
#include "bayesian_fusion.h"
#include "uart_tx.h"
#include "detection_types.h"
#include "uart_protocol.h"
#include "task_priorities.h"
#include "led_status.h"
#include "oled_display.h"
#include "ble_remote_id.h"

#if CONFIG_FOF_GLASSES_DETECTION
#include "glasses_detector.h"
#endif

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_scanner";

#define FIRMWARE_VERSION    "0.17.0-beta"
#define DETECTION_QUEUE_LEN 50
#define DISPLAY_UPDATE_MS   250

/* ── BOOT button ────────────────────────────────────────────────────────── */

#define BOOT_BUTTON_GPIO    GPIO_NUM_9   /* ESP32-C5 BOOT = GPIO9 */
#define LONG_PRESS_MS       1500
#define DOUBLE_TAP_WINDOW_MS 600

/* Button state */
static bool     s_btn_was_pressed = false;
static int64_t  s_btn_press_start = 0;
static int      s_tap_count       = 0;
static int64_t  s_last_tap_time   = 0;

/* Detected events */
static volatile bool s_button_short  = false;
static volatile bool s_button_double = false;

/* ── Glasses detection cache ────────────────────────────────────────────── */

#if CONFIG_FOF_GLASSES_DETECTION
#define GLASSES_CACHE_SIZE  10
#define GLASSES_STALE_MS    60000

static QueueHandle_t s_glasses_queue = NULL;

typedef struct {
    glasses_detection_t det;
    bool                occupied;
} glasses_cache_entry_t;

static glasses_cache_entry_t s_glasses_cache[GLASSES_CACHE_SIZE];
static int s_glasses_count = 0;

static void glasses_cache_update(void)
{
    /* Drain queue into cache */
    glasses_detection_t gdet;
    while (xQueueReceive(s_glasses_queue, &gdet, 0) == pdTRUE) {
        /* Find existing entry by MAC or empty slot */
        int slot = -1;
        int oldest_slot = -1;
        int64_t oldest_time = INT64_MAX;

        for (int i = 0; i < GLASSES_CACHE_SIZE; i++) {
            if (s_glasses_cache[i].occupied &&
                memcmp(s_glasses_cache[i].det.mac, gdet.mac, 6) == 0) {
                slot = i;
                break;
            }
            if (!s_glasses_cache[i].occupied && slot < 0) {
                slot = i;
            }
            if (s_glasses_cache[i].occupied &&
                s_glasses_cache[i].det.last_seen_ms < oldest_time) {
                oldest_time = s_glasses_cache[i].det.last_seen_ms;
                oldest_slot = i;
            }
        }

        if (slot < 0) slot = oldest_slot;  /* evict oldest */
        if (slot >= 0) {
            s_glasses_cache[slot].det = gdet;
            s_glasses_cache[slot].occupied = true;
        }
    }

    /* Prune stale entries */
    int64_t now_ms = esp_timer_get_time() / 1000;
    s_glasses_count = 0;
    for (int i = 0; i < GLASSES_CACHE_SIZE; i++) {
        if (s_glasses_cache[i].occupied) {
            if ((now_ms - s_glasses_cache[i].det.last_seen_ms) > GLASSES_STALE_MS) {
                s_glasses_cache[i].occupied = false;
            } else {
                s_glasses_count++;
            }
        }
    }
}

static glasses_detection_t *glasses_cache_get(int index)
{
    int count = 0;
    for (int i = 0; i < GLASSES_CACHE_SIZE; i++) {
        if (s_glasses_cache[i].occupied) {
            if (count == index) return &s_glasses_cache[i].det;
            count++;
        }
    }
    return NULL;
}
#endif /* CONFIG_FOF_GLASSES_DETECTION */

/* ── View state ─────────────────────────────────────────────────────────── */

static bool s_show_privacy = false;  /* false = drone view, true = privacy view */

/* ── Button polling ─────────────────────────────────────────────────────── */

static void poll_button(void)
{
    int level = gpio_get_level(BOOT_BUTTON_GPIO);
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (level == 0 && !s_btn_was_pressed) {
        /* Button just pressed (active low) */
        s_btn_was_pressed = true;
        s_btn_press_start = now_ms;
    } else if (level == 1 && s_btn_was_pressed) {
        /* Button just released */
        s_btn_was_pressed = false;
        int64_t duration = now_ms - s_btn_press_start;

        if (duration < LONG_PRESS_MS) {
            /* Short press = tap */
            s_tap_count++;
            s_last_tap_time = now_ms;
        }
    }

    /* Check for completed tap sequences */
    if (s_tap_count > 0 && !s_btn_was_pressed &&
        (now_ms - s_last_tap_time) > DOUBLE_TAP_WINDOW_MS) {
        if (s_tap_count >= 2) {
            s_button_double = true;
        } else {
            s_button_short = true;
        }
        s_tap_count = 0;
    }
}

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started (BOOT: tap=scroll, 2x=view toggle)");

    int page_index = 0;
    int cycle_counter = 0;

    /* Advance page every 10 ticks = 2.5s at 250ms refresh */
    #define PAGE_CYCLE_TICKS 10

    while (1) {
        /* Poll button every cycle */
        poll_button();

        /* Handle double-tap: toggle view */
        if (s_button_double) {
            s_button_double = false;
            s_show_privacy = !s_show_privacy;
            page_index = 0;
            cycle_counter = 0;
            ESP_LOGI(TAG, "View switched to %s", s_show_privacy ? "PRIVACY" : "DRONES");
        }

        /* Handle single tap: scroll page */
        if (s_button_short) {
            s_button_short = false;
            page_index++;
            cycle_counter = 0;
        }

        uint32_t uptime = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

#if CONFIG_FOF_GLASSES_DETECTION
        if (s_show_privacy) {
            /* ── Privacy view ──────────────────────────────────────────── */
            glasses_cache_update();

            if (s_glasses_count > 0) {
                if (page_index >= s_glasses_count) page_index = 0;

                glasses_detection_t *g = glasses_cache_get(page_index);
                if (g) {
                    oled_show_glasses_paged(
                        g->device_type, g->manufacturer,
                        g->device_name, g->confidence,
                        g->rssi, g->has_camera,
                        page_index + 1, s_glasses_count);
                }

                cycle_counter++;
                if (cycle_counter >= PAGE_CYCLE_TICKS) {
                    cycle_counter = 0;
                    page_index = (page_index + 1) % s_glasses_count;
                }
            } else {
                oled_show_privacy_status(0, uptime);
            }
        } else
#endif
        {
            /* ── Drone view (default) ──────────────────────────────────── */
            int total       = uart_tx_get_total_count();
            int active      = bayesian_fusion_get_active_count();
            uint8_t channel = uart_tx_get_current_channel();
            int ble         = uart_tx_get_ble_count();
            int wifi        = uart_tx_get_wifi_count();

            oled_update(total, active, channel, ble, wifi, uptime);

            scanner_detection_summary_t det_list[DETECTION_CACHE_SIZE];
            int det_count = uart_tx_get_cached_detections(det_list, DETECTION_CACHE_SIZE);

            if (det_count > 0) {
                if (page_index >= det_count) page_index = 0;

                oled_show_detection_paged(
                    det_list[page_index].drone_id,
                    det_list[page_index].manufacturer,
                    det_list[page_index].confidence,
                    det_list[page_index].rssi,
                    page_index + 1, det_count);

                cycle_counter++;
                if (cycle_counter >= PAGE_CYCLE_TICKS) {
                    cycle_counter = 0;
                    page_index = (page_index + 1) % det_count;
                }
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

    /* ── 1b. Initialize TCP/IP network interface (required before WiFi) ── */
    ESP_ERROR_CHECK(esp_netif_init());

    /* ── 2. Initialize default event loop ─────────────────────────────── */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* ── 3. Create shared detection queue ─────────────────────────────── */
    QueueHandle_t detection_queue = xQueueCreate(DETECTION_QUEUE_LEN,
                                                 sizeof(drone_detection_t));
    if (detection_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create detection queue -- halting");
        return;
    }
    ESP_LOGI(TAG, "Detection queue created (%d slots, %u bytes each)",
             DETECTION_QUEUE_LEN, (unsigned)sizeof(drone_detection_t));

    /* ── 4. Initialize Bayesian fusion engine ─────────────────────────── */
    bayesian_fusion_init();
    ESP_LOGI(TAG, "Bayesian fusion engine initialised");

    /* ── 5. Initialize status LED ────────────────────────────────────── */
    led_init();
    led_set_pattern(LED_BOOT);

    /* ── 5b. Initialize OLED display ─────────────────────────────────── */
    oled_init();
    oled_set_version(FIRMWARE_VERSION);
    oled_update(0, 0, 0, 0, 0, 0);

    /* ── 5c. Initialize BOOT button GPIO ─────────────────────────────── */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);
    ESP_LOGI(TAG, "BOOT button on GPIO%d (tap=scroll, 2x=privacy)", BOOT_BUTTON_GPIO);

    /* ── 6. Initialize UART TX (hardware setup, no task yet) ──────────── */
    uart_tx_init();

    /* ── 7. Initialize WiFi scanner (sets up promiscuous mode) ────────── */
    wifi_scanner_init(detection_queue);
    ESP_LOGI(TAG, "WiFi scanner initialised");

    /* ── 7b. Initialize BLE scanner (NimBLE) ─────────────────────────── */
    ble_remote_id_init(detection_queue);
    ESP_LOGI(TAG, "BLE Remote ID scanner initialised");

#if CONFIG_FOF_GLASSES_DETECTION
    /* ── 7c. Create glasses detection queue and wire to BLE scanner ───── */
    s_glasses_queue = xQueueCreate(10, sizeof(glasses_detection_t));
    if (s_glasses_queue != NULL) {
        ble_remote_id_set_glasses_queue(s_glasses_queue);
        memset(s_glasses_cache, 0, sizeof(s_glasses_cache));
        ESP_LOGI(TAG, "Glasses detection queue created (10 slots)");
    }
#endif

    /* ── 8. Start UART TX task on Core 1 (processing core) ──────────────── */
    uart_tx_start(detection_queue);

    /* ── 9. Start WiFi scanner task on Core 0 (radio core) ───────────── */
    wifi_scanner_start();
    ESP_LOGI(TAG, "WiFi scanner started on core %d, priority %d",
             WIFI_SCAN_TASK_CORE, WIFI_SCAN_TASK_PRIORITY);

    /* ── 9b. Start BLE scanner ───────────────────────────────────────── */
    ble_remote_id_start();
    ESP_LOGI(TAG, "BLE scanner started");

    /* ── 10. Start LED task ───────────────────────────────────────────── */
    led_start();
    led_set_pattern(LED_SCANNING);

    /* ── 11. Start display task ──────────────────────────────────────── */
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

    /* ── 12. Startup banner ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe — Scanner v%s", FIRMWARE_VERSION);
#if CONFIG_IDF_TARGET_ESP32C5
    ESP_LOGI(TAG, "  ESP32-C5 single-core RISC-V @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi 6 dual-band + BLE 5");
#else
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
    ESP_LOGI(TAG, "  WiFi + BLE 5");
#endif
    ESP_LOGI(TAG, "  UART1 -> Uplink @ %d baud", UART_BAUD_RATE);
    ESP_LOGI(TAG, "  Detection queue: %d slots", DETECTION_QUEUE_LEN);
    ESP_LOGI(TAG, "  BOOT button: tap=scroll, 2x=privacy view");
    ESP_LOGI(TAG, "============================================");

    /* app_main returns; FreeRTOS scheduler keeps tasks running. */
}
