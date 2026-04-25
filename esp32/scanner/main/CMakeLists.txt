set(SRCS
    "main.c"
    "detection/wifi_scanner.c"
    "detection/wifi_ssid_patterns.c"
    "detection/wifi_oui_database.c"
    "detection/dji_drone_id_parser.c"
    "detection/wifi_beacon_rid_parser.c"
    "detection/french_dri_parser.c"
    "detection/open_drone_id_parser.c"
    "detection/ble_remote_id.c"
    "detection/ble_fingerprint.c"
    "detection/ble_ja3.c"
    "detection/glasses_detector.c"
    "detection/bayesian_fusion.c"
    "comms/uart_tx.c"
    "comms/uart_ota.c"
    "core/calibration_mode.c"
    "hw/led_status.c"
    "hw/oled_display.c"
    "../../shared/detection_policy.c"
    "../../shared/time_sync_policy.c"
    "../../shared/psram_alloc.c"
)

set(REQUIRES esp_wifi driver nvs_flash esp_timer json esp_event esp_netif app_update esp_partition esp_psram bt)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "detection" "comms" "core" "hw" "../../shared"
    REQUIRES ${REQUIRES}
)
