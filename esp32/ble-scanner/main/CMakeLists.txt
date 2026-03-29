idf_component_register(
    SRCS "main.c"
         "comms/console_output.c"
         "hw/led_status.c"
         "hw/oled_display.c"
         "detection/wifi_privacy_scanner.c"
         "../../scanner/main/detection/ble_remote_id.c"
         "../../scanner/main/detection/ble_fingerprint.c"
         "../../scanner/main/detection/open_drone_id_parser.c"
         "../../scanner/main/detection/bayesian_fusion.c"
         "../../scanner/main/detection/glasses_detector.c"
    INCLUDE_DIRS "." "comms" "core" "hw" "detection"
                 "../../scanner/main/detection"
                 "../../shared"
    REQUIRES bt driver nvs_flash esp_timer json esp_wifi esp_event esp_netif
)
