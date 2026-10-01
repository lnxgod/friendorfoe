idf_component_register(SRCS "main.c" INCLUDE_DIRS "." "../../shared"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer esp_system json)
