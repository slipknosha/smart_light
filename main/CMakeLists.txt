idf_component_register(SRCS "main.c" "ble_service.c" "dimmer.c"
                       PRIV_REQUIRES bt esp_driver_gpio esp_driver_gptimer nvs_flash
                       INCLUDE_DIRS ".")
