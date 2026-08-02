#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "backend_identity.h"
#include "backend_upload_batch.h"

static backend_scanner_status_t fixture_scanner(
    uint32_t sequence,
    uint32_t boot_id,
    const char *mac,
    backend_scan_profile_t profile,
    bool ble_healthy,
    bool wifi_healthy)
{
    backend_scanner_status_t status = {
        .schema = BACKEND_SCANNER_STATUS_SCHEMA,
        .sequence = sequence,
        .boot_id = boot_id,
        .profile = profile,
        .role_generation = 4U,
        .role_acked = true,
        .command_ingress = true,
        .ble_healthy = ble_healthy,
        .wifi_healthy = wifi_healthy,
        .uptime_ms = UINT64_C(9000),
    };

    snprintf(status.mac, sizeof(status.mac), "%s", mac);
    snprintf(status.target, sizeof(status.target), "%s",
             FOF_BACKEND_SCANNER_TARGET);
    snprintf(status.project, sizeof(status.project), "%s",
             FOF_BACKEND_SCANNER_PROJECT);
    snprintf(status.hardware, sizeof(status.hardware), "%s",
             FOF_BACKEND_HARDWARE);
    snprintf(status.version, sizeof(status.version), "%s",
             FOF_VERSION_BACKEND);
    snprintf(status.ota_state, sizeof(status.ota_state), "%s", "idle");
    snprintf(status.rollback_state, sizeof(status.rollback_state), "%s",
             "valid");
    return status;
}

static backend_batch_context_t fixture_context(void)
{
#if defined(FOF_BACKEND_PROFILE_S3_FULLSIZE)
    static const char *const capabilities[] = {
        "display_none", "rgb_led", "scanner_uart", "http_uplink",
        "config_ap", "remote_ota", "uart_relay_ota",
    };
#else
    static const char *const capabilities[] = {
        "display_none", "yellow_led", "scanner_uart", "http_uplink",
        "config_ap", "remote_ota", "uart_relay_ota",
    };
#endif
    backend_batch_context_t context = {
        .device_id = "uplink_CB77A4",
        .product_family = FOF_BACKEND_PRODUCT_FAMILY,
        .firmware_line = FOF_BACKEND_FIRMWARE_LINE,
        .component = "uplink",
        .firmware_version = FOF_VERSION_BACKEND,
        .firmware_target = FOF_BACKEND_UPLINK_TARGET,
        .app_project = FOF_BACKEND_UPLINK_PROJECT,
        .hardware_type = FOF_BACKEND_HARDWARE,
        .hardware_mac = "A4:CF:12:CB:77:A4",
        .node_name = "Roof backend sensor",
        .capability_count = sizeof(capabilities) / sizeof(capabilities[0]),
        .has_device_location = true,
        .device_lat = 36.1699,
        .device_lon = -115.1398,
        .device_alt = 610.5,
        .scanner_present = {true, true},
        .clock_valid = true,
        .epoch_ms = INT64_C(1785600000999),
        .wifi_ssid = "FoF Lab",
        .wifi_rssi = -53,
        .ap_active = true,
        .config_generation = 9U,
        .command_success_count = 17U,
        .command_failure_count = 2U,
        .uptime_ms = UINT64_C(9876543210),
        .led_state = BACKEND_LED_DRONE_META,
        .upload_queue = {
            .depth_batches = 7U,
            .capacity_batches = BACKEND_UPLOAD_FIFO_CAPACITY,
            .overflow_dropped_batches = 2U,
            .quarantined_batches = 1U,
        },
        .upload = {
            .ok = 11U,
            .failed = 3U,
            .retry_count = 4U,
            .has_last_success_age = true,
            .last_success_age_s = 8U,
        },
        .sequence = 41U,
    };

    for (size_t index = 0U;
         index < sizeof(capabilities) / sizeof(capabilities[0]); ++index) {
        snprintf(context.capabilities[index], sizeof(context.capabilities[index]),
                 "%s", capabilities[index]);
    }

    context.scanners[0] = fixture_scanner(
        12U, UINT32_C(0x12345678), "AA:BB:CC:DD:EE:01",
        BACKEND_SCAN_PROFILE_BLE_PRIMARY, true, false);
    context.scanners[1] = fixture_scanner(
        19U, UINT32_C(0x87654321), "AA:BB:CC:DD:EE:02",
        BACKEND_SCAN_PROFILE_WIFI_PRIMARY, false, true);
    return context;
}

static drone_detection_t fixture_drone(void)
{
    drone_detection_t value = {
        .drone_id = "RID-CANON-001",
        .source = DETECTION_SRC_WIFI_PROBE_REQUEST,
        .confidence = 0.875f,
        .latitude = 37.7749,
        .longitude = -122.4194,
        .altitude_m = 123.75,
        .heading_deg = 271.25f,
        .speed_mps = 14.5f,
        .vertical_speed_mps = -1.5f,
        .rssi = -47,
        .estimated_distance_m = 8.125,
        .manufacturer = "Acme Air",
        .model = "RID-X1",
        .operator_lat = 37.75,
        .operator_lon = -122.4,
        .operator_id = "OP-42",
        .ua_type = 2U,
        .id_type = 1U,
        .self_id_desc_type = 3U,
        .self_id_text = "inspection",
        .height_agl_m = 42.25,
        .geodetic_alt_m = 130.5,
        .h_accuracy_m = 1.75f,
        .v_accuracy_m = 2.5f,
        .area_count = 17U,
        .area_radius = 250U,
        .area_ceiling = 160.25,
        .area_floor = 15.5,
        .classification_type = 3U,
        .ssid = "FieldNet",
        .bssid = "02:12:34:56:78:9A",
        .freq_mhz = 2437,
        .channel_width_mhz = 80,
        .wifi_auth_mode = 3U,
        .first_seen_ms = INT64_C(1785600000001),
        .last_updated_ms = INT64_C(1785600000120),
        .fused_confidence = 0.9375f,
        .probed_ssids = "FieldNet,Guest",
        .probe_ie_hash = UINT32_C(0x00abcdef),
        .wifi_generation = 6U,
        .scanner_slot = BACKEND_SCANNER_SLOT_BLE,
        .scanner_slots_seen = 3U,
    };
    return value;
}

static drone_detection_t fixture_meta(void)
{
    drone_detection_t value = {
        .drone_id = "META-AA:BB:CC:DD:EE:02",
        .source = DETECTION_SRC_BLE_FINGERPRINT,
        .confidence = 0.8125f,
        .latitude = 37.775,
        .longitude = -122.4195,
        .altitude_m = 5.0,
        .heading_deg = 90.0f,
        .speed_mps = 1.25f,
        .vertical_speed_mps = 0.25f,
        .rssi = -55,
        .estimated_distance_m = 4.5,
        .manufacturer = "Meta",
        .model = "Ray-Ban",
        .bssid = "AA:BB:CC:DD:EE:02",
        .wifi_auth_mode = UINT8_MAX,
        .ble_company_id = 0x004cU,
        .ble_apple_type = 0x10U,
        .ble_service_uuids = {0x180fU, 0xffe0U},
        .ble_svc_uuid_count = 2U,
        .ble_svc_uuids_raw = "180f,ffe0",
        .ble_ad_type_count = 6U,
        .ble_payload_len = 31U,
        .ble_addr_type = 2U,
        .ble_ja3_hash = UINT32_C(0x0123abcd),
        .ble_name = "Ray-Ban Meta",
        .class_reason = "camera_service",
        .ble_apple_auth = {0x01U, 0xa5U, 0xffU},
        .ble_apple_activity = 3U,
        .ble_apple_flags = 5U,
        .ble_raw_mfr = {0x4cU, 0x00U, 0x10U, 0x07U, 0x01U, 0x02U},
        .ble_raw_mfr_len = 6U,
        .ble_adv_interval_us = INT64_C(125500),
        .first_seen_ms = INT64_C(1785600000200),
        .last_updated_ms = INT64_C(1785600000400),
        .fused_confidence = 0.90625f,
        .scanner_slot = BACKEND_SCANNER_SLOT_WIFI,
        .scanner_slots_seen = 2U,
        .ble_threat_kind = BLE_THREAT_KIND_SERIAL_SKIMMER,
        .ble_prompt_family_mask = 19U,
        .ble_unique_macs = 12U,
        .ble_observation_count = 23U,
        .ble_serial_service_uuid = 0xffe0U,
        .ble_threat_evidence_mask = 49U,
    };
    return value;
}

int main(void)
{
    backend_batch_context_t context = fixture_context();
    backend_detection_observation_t drone = {
        .detection = fixture_drone(),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1785600000123),
    };
    backend_detection_observation_t meta = {
        .detection = fixture_meta(),
        .timestamp_valid = true,
        .timestamp_epoch_ms = INT64_C(1785600000456),
    };
    backend_upload_builder_t builder;
    backend_upload_batch_t batch = {0};

    backend_upload_builder_init(&builder, &context, 1000);
    if (backend_upload_builder_add(&builder, &drone, 1001) !=
            BACKEND_ENCODE_OK ||
        backend_upload_builder_add(&builder, &meta, 1002) !=
            BACKEND_ENCODE_OK ||
        !backend_upload_builder_finish(&builder, &batch) ||
        batch.json_len > BACKEND_UPLOAD_MAX_JSON) {
        return 2;
    }
    return fwrite(batch.json, 1, batch.json_len, stdout) == batch.json_len
        ? 0 : 3;
}
