idf_component_register(SRCS "blink_example_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_event esp_http_client
                                esp_netif esp_rom esp-tls esp_timer esp_wifi nvs_flash)
