idf_component_register(SRCS "app_main.c"
                            "generic_task.c"
                            "gnss.c"
                            "http_stream.c"
                            "ota_update.c"
                            "wifi_ap.c"
                    PRIV_REQUIRES app_update esp_driver_gpio esp_driver_uart esp_http_server esp_netif esp_system esp_wifi lwip mbedtls nvs_flash
                    INCLUDE_DIRS ".")
