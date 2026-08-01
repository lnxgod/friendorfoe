set(FOF_UPLINK_REQUIRES
    esp_wifi esp_http_client esp_http_server driver nvs_flash esp_timer lwip
    esp_adc cjson esp_netif esp_event app_update esp_partition esp_psram
    mbedtls esp_driver_usb_serial_jtag
)

if(CONFIG_BT_ENABLED)
    list(APPEND FOF_UPLINK_REQUIRES bt)
endif()

set(FOF_UPLINK_EXCLUDE_SRCS)
set(FOF_UPLINK_GAME_SRCS)
if(FOF_DC34_GAME_CANARY)
    list(APPEND FOF_UPLINK_GAME_SRCS "game/badge_con_vhci.c")
endif()

if(NOT FOF_DC34_GAME_CANARY)
    list(APPEND FOF_UPLINK_EXCLUDE_SRCS
        EXCLUDE_SRCS
            "core/badge_con_runtime.c"
            "../../shared/badge_con_presentation.c"
            "../../shared/badge_con_radio_runtime_policy.c"
            "../../shared/badge_con_vhci_policy.c"
            "../../shared/badge_update_maintenance_policy.c")
endif()

idf_component_register(
    SRC_DIRS "." "comms" "hw" "hw/assets" "network" "core" "../../shared"
    ${FOF_UPLINK_EXCLUDE_SRCS}
    INCLUDE_DIRS "." "comms" "hw" "hw/assets" "network" "core" "game" "../../shared"
    REQUIRES ${FOF_UPLINK_REQUIRES}
)

if(FOF_DC34_GAME_CANARY)
    target_sources(${COMPONENT_LIB} PRIVATE ${FOF_UPLINK_GAME_SRCS})
endif()

target_sources(${COMPONENT_LIB} PRIVATE "../../shared/time_sync_policy.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/badge_display_contract.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/badge_runtime_rtc_policy.c")
if(NOT FOF_DC34_GAME_CANARY)
    target_sources(${COMPONENT_LIB} PRIVATE "production/badge_update_admission_policy.c")
endif()
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_legacy_ready.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_relay_policy.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_auto_policy.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/firmware_coordinator_migration.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/badge_usb_uplink_ota.c")
target_sources(${COMPONENT_LIB} PRIVATE "../../shared/scanner_uplink_ingress_registry.c")
