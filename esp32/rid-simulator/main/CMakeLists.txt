idf_component_register(
    SRCS "main.c"
         "sim/flight_sim.c"
         "sim/odid_encoder.c"
         "comms/ble_advertiser.c"
         "hw/led_status.c"
         "hw/oled_display.c"
    INCLUDE_DIRS "." "sim" "comms" "core" "hw"
    REQUIRES bt nvs_flash esp_timer
)
