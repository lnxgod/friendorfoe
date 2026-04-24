idf_component_register(
    SRC_DIRS "." "comms" "hw" "network" "core" "../../shared"
    INCLUDE_DIRS "." "comms" "hw" "network" "core" "../../shared"
    REQUIRES esp_wifi esp_http_client esp_http_server driver nvs_flash esp_timer lwip
             esp_adc json esp_netif esp_event app_update esp_partition esp_psram
)

target_sources(${COMPONENT_LIB} PRIVATE "../../shared/time_sync_policy.c")
