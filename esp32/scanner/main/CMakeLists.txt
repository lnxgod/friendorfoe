idf_component_register(
    SRC_DIRS "." "detection" "comms" "hw"
    INCLUDE_DIRS "." "detection" "comms" "core" "hw" "../../shared"
    REQUIRES bt esp_wifi driver nvs_flash esp_timer json esp_event esp_netif
)
