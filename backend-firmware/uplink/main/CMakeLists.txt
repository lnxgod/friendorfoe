set(BACKEND_STATUS_LED_SRCS "../../shared/backend_status_led.c")
set(BACKEND_STATUS_LED_REQUIRES)
set(BACKEND_STATUS_PROJECT_NAME "${PROJECT_NAME}")
if(NOT "${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_uplink" AND
   NOT "${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_uplink_fullsize")
    set(BACKEND_STATUS_PROJECT_NAME "${CMAKE_PROJECT_NAME}")
endif()
if("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_uplink")
    list(APPEND BACKEND_STATUS_LED_SRCS "hw/backend_yellow_led.c")
elseif("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_uplink_fullsize")
    list(APPEND BACKEND_STATUS_LED_SRCS "../../shared/backend_rgb_led_pattern.c")
    list(APPEND BACKEND_STATUS_LED_REQUIRES backend_fullsize_led)
else()
    message(FATAL_ERROR
        "Unknown backend uplink project: ${BACKEND_STATUS_PROJECT_NAME}")
endif()

idf_component_register(
    SRCS
        "main.c"
        "comms/backend_uart_investigation.c"
        "comms/backend_uart_slot.c"
        "core/backend_coordinator.c"
        "core/backend_health.c"
        ${BACKEND_STATUS_LED_SRCS}
        "network/backend_command_client.c"
        "network/backend_config_portal.c"
        "network/backend_http_transport.c"
        "network/backend_time_sync.c"
        "network/backend_uploader.c"
        "network/backend_wifi_manager.c"
        "ota/backend_ota_maintenance.c"
        "ota/backend_scanner_relay.c"
        "ota/backend_self_ota.c"
        "storage/backend_firmware_buffer.c"
        "storage/backend_firmware_store.c"
        "storage/backend_nvs_config.c"
        "storage/backend_ota_journal.c"
        "../../shared/backend_ap_policy.c"
        "../../shared/backend_ble_investigation.c"
        "../../shared/backend_config.c"
        "../../shared/backend_detection_codec.c"
        "../../shared/backend_detection_router.c"
        "../../shared/backend_embedded_identity.c"
        "../../shared/backend_flow_policy.c"
        "../../shared/backend_http_policy.c"
        "../../shared/backend_identity.c"
        "../../shared/backend_ingest_ack.c"
        "../../shared/backend_json_reader.c"
        "../../shared/backend_json_writer.c"
        "../../shared/backend_led_pattern.c"
        "../../shared/backend_led_protocol.c"
        "../../shared/backend_ota_identity.c"
        "../../shared/backend_portal_contract.c"
        "../../shared/backend_recovery_policy.c"
        "../../shared/backend_scanner_control_codec.c"
        "../../shared/backend_scanner_role.c"
        "../../shared/backend_scanner_status_codec.c"
        "../../shared/backend_scanner_topology.c"
        "../../shared/backend_threat_policy.c"
        "../../shared/backend_upload_batch.c"
        "../../shared/backend_upload_fifo.c"
        "../../shared/ble_investigation_protocol.c"
        "../../shared/detection_policy.c"
        "../../shared/firmware_image_contract.c"
        "../../shared/firmware_json_schema.c"
        "../../shared/firmware_operation_token.c"
        "../../shared/firmware_version_order.c"
        "../../shared/privacy_rf_signatures.c"
        "../../shared/psram_alloc.c"
        "../../shared/scanner_uart_line_framer.c"
        "../../shared/time_sync_policy.c"
    INCLUDE_DIRS
        "."
        "comms"
        "core"
        "hw"
        "network"
        "ota"
        "storage"
        "../../shared"
    LDFRAGMENTS
        "backend_identity.lf"
    REQUIRES
        esp_wifi
        esp_http_server
        esp_netif
        esp_event
        nvs_flash
        esp_timer
        app_update
        mbedtls
        lwip
        esp_psram
        driver
        ${BACKEND_STATUS_LED_REQUIRES}
)
