set(BACKEND_STATUS_LED_SRCS "../../shared/backend_status_led.c")
set(BACKEND_STATUS_LED_REQUIRES)
set(BACKEND_STATUS_DISCOVERY_PROJECT "${PROJECT_NAME}")
set(BACKEND_STATUS_FINAL_PROJECT "${CMAKE_PROJECT_NAME}")
if("${BACKEND_STATUS_DISCOVERY_PROJECT}" STREQUAL "" OR
   "${BACKEND_STATUS_DISCOVERY_PROJECT}" STREQUAL "esp-idf")
    set(BACKEND_STATUS_PROJECT_NAME "${BACKEND_STATUS_FINAL_PROJECT}")
elseif("${BACKEND_STATUS_DISCOVERY_PROJECT}" STREQUAL "fof_backend_scanner" OR
       "${BACKEND_STATUS_DISCOVERY_PROJECT}" STREQUAL "fof_backend_scanner_fullsize")
    set(BACKEND_STATUS_PROJECT_NAME "${BACKEND_STATUS_DISCOVERY_PROJECT}")
    if(NOT "${BACKEND_STATUS_FINAL_PROJECT}" STREQUAL "" AND
       NOT "${BACKEND_STATUS_FINAL_PROJECT}" STREQUAL
           "${BACKEND_STATUS_PROJECT_NAME}")
        message(FATAL_ERROR "Backend scanner project identity mismatch")
    endif()
else()
    message(FATAL_ERROR
        "Unknown backend scanner project: ${BACKEND_STATUS_DISCOVERY_PROJECT}")
endif()
if("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_scanner")
    list(APPEND BACKEND_STATUS_LED_SRCS "hw/backend_yellow_led.c")
elseif("${BACKEND_STATUS_PROJECT_NAME}" STREQUAL "fof_backend_scanner_fullsize")
    list(APPEND BACKEND_STATUS_LED_SRCS "../../shared/backend_rgb_led_pattern.c")
    list(APPEND BACKEND_STATUS_LED_REQUIRES backend_fullsize_led)
else()
    message(FATAL_ERROR
        "Unknown backend scanner project: ${BACKEND_STATUS_PROJECT_NAME}")
endif()

idf_component_register(
    SRCS
    "main.c"
    "comms/backend_uart_rx.c"
    "comms/backend_uart_tx.c"
    "comms/uart_ota.c"
    "core/backend_detection_sink.c"
    "core/backend_investigation_sink.c"
    "core/backend_scanner_runtime.c"
    "core/scanner_rollback.c"
    "detection/backend_feature_adapter.c"
    "detection/backend_glasses_classifier.c"
    "detection/backend_glasses_settings.c"
    "detection/bayesian_fusion.c"
    "detection/ble_fingerprint.c"
    "detection/ble_investigator.c"
    "detection/ble_ja3.c"
    "detection/ble_remote_id.c"
    "detection/ble_threat_detector.c"
    "detection/dji_drone_id_parser.c"
    "detection/french_dri_parser.c"
    "detection/open_drone_id_parser.c"
    "detection/wifi_beacon_rid_parser.c"
    "detection/wifi_oui_database.c"
    "detection/wifi_scanner.c"
    "detection/wifi_ssid_patterns.c"
    ${BACKEND_STATUS_LED_SRCS}
    "../../shared/backend_embedded_identity.c"
    "../../shared/backend_detection_codec.c"
    "../../shared/backend_flow_policy.c"
    "../../shared/backend_identity.c"
    "../../shared/backend_json_reader.c"
    "../../shared/backend_json_writer.c"
    "../../shared/backend_led_pattern.c"
    "../../shared/backend_led_protocol.c"
    "../../shared/backend_ota_identity.c"
    "../../shared/backend_recovery_policy.c"
    "../../shared/backend_scanner_control_codec.c"
    "../../shared/backend_scanner_role.c"
    "../../shared/backend_scanner_status_codec.c"
    "../../shared/backend_scanner_topology.c"
    "../../shared/ble_investigation_protocol.c"
    "../../shared/detection_policy.c"
    "../../shared/firmware_image_contract.c"
    "../../shared/firmware_version_order.c"
    "../../shared/privacy_rf_signatures.c"
    "../../shared/psram_alloc.c"
    "../../shared/scanner_uart_line_framer.c"
    "../../shared/time_sync_policy.c"
    INCLUDE_DIRS "." "comms" "core" "detection" "hw" "../../shared"
    LDFRAGMENTS "backend_identity.lf"
    REQUIRES
    bt
    esp_wifi
    nvs_flash
    esp_timer
    app_update
    mbedtls
    esp_psram
    driver
    ${BACKEND_STATUS_LED_REQUIRES}
)
