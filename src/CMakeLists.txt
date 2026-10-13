idf_component_register(
    SRCS
        "main.cpp"
        "bme280_handler.cpp"
        "nats_client.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        lwip
        esp_driver_i2c
        esp_driver_gpio
)
