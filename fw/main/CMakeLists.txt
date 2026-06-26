idf_component_register(SRCS "monita_main.c" "i2c_bus.c" "power.c" "touch.c" "ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_lcd_co5300 driver esp_timer esp_hw_support
                                esp_wifi esp_netif esp_event nvs_flash esp_http_client json
                                app_update esp_https_ota esp_driver_i2c)
