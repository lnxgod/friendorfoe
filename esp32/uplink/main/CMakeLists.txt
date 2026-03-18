idf_component_register(
    SRC_DIRS "." "comms" "hw" "network" "core"
    INCLUDE_DIRS "." "comms" "hw" "network" "core" "../../../shared"
    REQUIRES esp_wifi esp_http_client driver nvs_flash esp_timer lwip
             esp_adc json esp_netif esp_event
)
