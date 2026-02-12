idf_component_register(SRCS "hello_world_main.c"
                       "epd_169inch.c"
                       "epd_169inch_bus.c"
                       "image_upload.c"
                       "led_ws2812.c"
                       "scd30_app.c"
                       "../third_party/heatshrink/heatshrink_decoder.c"
                       "../third_party/embedded-i2c-scd30/scd30_i2c.c"
                       "../third_party/embedded-i2c-scd30/sensirion_common.c"
                       "../third_party/embedded-i2c-scd30/sensirion_i2c.c"
                       "../third_party/embedded-i2c-scd30/sensirion_i2c_hal.c"
                       PRIV_REQUIRES spi_flash esp_driver_gpio esp_driver_spi esp_timer
                                     esp_wifi esp_event esp_netif esp_http_server
                                     esp_driver_rmt
                                     driver
                                     nvs_flash spiffs
                       INCLUDE_DIRS "" "../third_party/heatshrink"
                                    "../third_party/embedded-i2c-scd30")
spiffs_create_partition_image(spiffs ../spiffs FLASH_IN_PROJECT)
# 
# 