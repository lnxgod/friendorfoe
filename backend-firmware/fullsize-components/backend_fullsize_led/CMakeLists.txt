idf_component_register(
    SRCS "backend_fullsize_rgb_led.c"
    INCLUDE_DIRS "include" "../../shared"
    REQUIRES freertos led_strip
)
