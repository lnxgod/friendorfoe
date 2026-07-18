set(FOF_UPLINK_REQUIRES
    esp_wifi esp_http_client esp_http_server driver nvs_flash esp_timer lwip
    esp_adc cjson esp_netif esp_event app_update esp_partition esp_psram
    mbedtls
)

if(CONFIG_BT_ENABLED)
    list(APPEND FOF_UPLINK_REQUIRES bt)
endif()

idf_component_register(
    SRC_DIRS "." "comms" "hw" "hw/assets" "network" "core" "../../shared"
    INCLUDE_DIRS "." "comms" "hw" "hw/assets" "network" "core" "../../shared"
    REQUIRES ${FOF_UPLINK_REQUIRES}
)

target_sources(${COMPONENT_LIB} PRIVATE "../../shared/time_sync_policy.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/badge_display_contract.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_legacy_ready.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_relay_policy.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_auto_policy.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_coordinator_migration.c")
