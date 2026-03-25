/**
 * Friend or Foe — BLE Remote ID Simulator Entry Point
 *
 * Broadcasts ASTM F3411-compliant OpenDroneID BLE advertisements
 * simulating a drone flying a circle orbit. Debug tool for testing
 * BLE scanner and Android app without a real drone.
 *
 * Sim loop:
 *   flight_sim_tick() → encode 4 ODID messages → update BLE advertiser →
 *   update OLED → repeat every 250ms
 */

#include "flight_sim.h"
#include "odid_encoder.h"
#include "ble_advertiser.h"
#include "led_status.h"
#include "oled_display.h"
#include "core/task_priorities.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Include local coordinate overrides if available */
#if __has_include("sim/local_coords.h")
#include "sim/local_coords.h"
#endif

/* ── Constants ──────────────────────────────────────────────────────────── */

static const char *TAG = "fof_rid_sim";

#define FIRMWARE_VERSION     "0.1.0"
#define SIM_OPERATOR_ID      "FOF-TEST-OP"
#define SIM_UA_TYPE          2    /* Rotorcraft */
#define SIM_ID_TYPE          1    /* Serial number */
#define DRONE_SWITCH_TICKS   8    /* Switch broadcast drone every 8 ticks (2 seconds) */

/* Home coordinates loaded from KConfig at runtime (set via menuconfig) */
static double s_home_lat;
static double s_home_lon;

#define SIM_TICK_MS          250
#define DISPLAY_UPDATE_MS    500

/* ── Display update task ───────────────────────────────────────────────── */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");
    int display_drone = 0;
    int cycle = 0;

    while (1) {
        double lat = flight_sim_get_lat(display_drone);
        double lon = flight_sim_get_lon(display_drone);
        double alt = flight_sim_get_alt(display_drone);
        float heading = flight_sim_get_heading(display_drone);
        uint32_t adv_count = ble_advertiser_get_adv_count();
        uint32_t uptime = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

        oled_sim_update(lat, lon, alt, heading, adv_count, uptime);

        /* Cycle displayed drone every 3 seconds */
        cycle++;
        if (cycle >= 6) {
            cycle = 0;
            display_drone = (display_drone + 1) % SIM_DRONE_COUNT;
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_MS));
    }
}

/* ── Sim loop task ─────────────────────────────────────────────────────── */

static void sim_task(void *arg)
{
    ESP_LOGI(TAG, "Sim task started — %d drones", SIM_DRONE_COUNT);

    uint8_t basic_id_msg[25];
    uint8_t location_msg[25];
    uint8_t system_msg[25];
    uint8_t operator_msg[25];

    /* Encode static operator message (shared by all drones) */
    odid_encode_system(system_msg, s_home_lat, s_home_lon, 1, 100.0);
    odid_encode_operator_id(operator_msg, SIM_OPERATOR_ID);

    uint32_t tick_count = 0;
    int active_drone = 0;

    while (1) {
        flight_sim_tick();

        /* Switch which drone we're broadcasting every DRONE_SWITCH_TICKS */
        if (tick_count % DRONE_SWITCH_TICKS == 0) {
            active_drone = (active_drone + 1) % SIM_DRONE_COUNT;

            /* Re-encode Basic ID for the new active drone */
            const char *serial = flight_sim_get_serial(active_drone);
            odid_encode_basic_id(basic_id_msg, serial, SIM_UA_TYPE, SIM_ID_TYPE);
        }

        double lat = flight_sim_get_lat(active_drone);
        double lon = flight_sim_get_lon(active_drone);
        double alt = flight_sim_get_alt(active_drone);
        float heading = flight_sim_get_heading(active_drone);
        float speed = flight_sim_get_speed(active_drone);

        odid_encode_location(location_msg, lat, lon, alt, heading, speed, 0.0f);
        ble_advertiser_update_messages(basic_id_msg, location_msg,
                                        system_msg, operator_msg);

        tick_count++;
        if (tick_count % 20 == 0) {
            ESP_LOGI(TAG, "ADV[%d/%s]: lat=%.4f lon=%.4f alt=%.0f hdg=%.0f spd=%.1f",
                     active_drone, flight_sim_get_serial(active_drone),
                     lat, lon, alt, heading, speed);
        }

        vTaskDelay(pdMS_TO_TICKS(SIM_TICK_MS));
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

    /* ── 2. Initialize status LED ────────────────────────────────────── */
    led_init();
    led_set_pattern(LED_BOOT);

    /* ── 3. Initialize OLED display ─────────────────────────────────── */
    oled_init();

    /* ── 4. Load home coordinates ────────────────────────────────────── */
#ifdef FOF_HOME_LAT_OVERRIDE
    s_home_lat = FOF_HOME_LAT_OVERRIDE;
    s_home_lon = FOF_HOME_LON_OVERRIDE;
#else
    s_home_lat = atof(CONFIG_FOF_SIM_HOME_LAT);
    s_home_lon = atof(CONFIG_FOF_SIM_HOME_LON);
#endif

    /* ── 5. Initialize flight simulator ─────────────────────────────── */
    flight_sim_init();
    ESP_LOGI(TAG, "Flight sim initialized (orbit around %.4f, %.4f)",
             s_home_lat, s_home_lon);

    /* ── 5. Initialize BLE advertiser ───────────────────────────────── */
    ble_advertiser_init();
    ble_advertiser_start();
    ESP_LOGI(TAG, "BLE ODID advertiser started");

    /* ── 6. Start LED with simulating pattern ───────────────────────── */
    led_start();
    led_set_pattern(LED_SIMULATING);

    /* ── 7. Start sim task ──────────────────────────────────────────── */
#ifdef CONFIG_FREERTOS_UNICORE
    xTaskCreate(sim_task, "sim", SIM_TASK_STACK_SIZE,
                NULL, SIM_TASK_PRIORITY, NULL);
#else
    xTaskCreatePinnedToCore(sim_task, "sim", SIM_TASK_STACK_SIZE,
                            NULL, SIM_TASK_PRIORITY, NULL, SIM_TASK_CORE);
#endif

    /* ── 8. Start display task ──────────────────────────────────────── */
#ifdef CONFIG_FREERTOS_UNICORE
    xTaskCreate(display_task, "display", DISPLAY_TASK_STACK_SIZE,
                NULL, DISPLAY_TASK_PRIORITY, NULL);
#else
    xTaskCreatePinnedToCore(display_task, "display", DISPLAY_TASK_STACK_SIZE,
                            NULL, DISPLAY_TASK_PRIORITY, NULL, DISPLAY_TASK_CORE);
#endif

    /* ── 9. Startup banner ─────────────────────────────────────────── */
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Friend or Foe — RID Simulator v%s", FIRMWARE_VERSION);
#if CONFIG_IDF_TARGET_ESP32S3
    ESP_LOGI(TAG, "  ESP32-S3 dual-core @ 240 MHz");
#elif CONFIG_IDF_TARGET_ESP32C5
    ESP_LOGI(TAG, "  ESP32-C5 single-core RISC-V @ 240 MHz");
#elif CONFIG_IDF_TARGET_ESP32C3
    ESP_LOGI(TAG, "  ESP32-C3 single-core RISC-V @ 160 MHz");
#endif
    ESP_LOGI(TAG, "  Drones: %d simulated", SIM_DRONE_COUNT);
    for (int i = 0; i < SIM_DRONE_COUNT; i++) {
        ESP_LOGI(TAG, "    [%d] %s — %.0fm/s, alt %.0fm",
                 i, flight_sim_get_serial(i),
                 flight_sim_get_speed(i), flight_sim_get_alt(i));
    }
    ESP_LOGI(TAG, "  Operator: %s", SIM_OPERATOR_ID);
    ESP_LOGI(TAG, "  Orbit: %.4f, %.4f  60m radius, 30m AGL",
             s_home_lat, s_home_lon);
    ESP_LOGI(TAG, "  BLE ODID broadcast (UUID 0xFFFA)");
    ESP_LOGI(TAG, "============================================");
}
