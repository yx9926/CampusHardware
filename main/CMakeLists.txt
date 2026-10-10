idf_component_register(SRCS "main.c" "state.c" "rtc.c" "sensor.c" "buttons.c" "network.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc esp_wifi esp_netif mqtt nvs_flash esp_timer esp_event lwip mbedtls)
