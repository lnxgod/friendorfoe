idf_component_register(
    SRCS "main.c"
         "detection/wifi_scanner.c"
         "detection/wifi_ssid_patterns.c"
         "detection/wifi_oui_database.c"
         "detection/dji_drone_id_parser.c"
         "detection/wifi_beacon_rid_parser.c"
         "detection/french_dri_parser.c"
         "detection/open_drone_id_parser.c"
         "detection/ble_remote_id.c"
         "detection/bayesian_fusion.c"
         "detection/glasses_detector.c"
         "comms/uart_tx.c"
         "hw/led_status.c"
         "hw/oled_display.c"
    INCLUDE_DIRS "." "detection" "comms" "core" "hw" "../../shared"
    REQUIRES esp_wifi driver nvs_flash esp_timer json esp_event esp_netif bt
)
