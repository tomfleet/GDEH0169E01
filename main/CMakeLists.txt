idf_component_register(SRCS "hello_world_main.c"
                       "epd_169inch.c"
                       "epd_169inch_bus.c"
                       "image_upload.c"
                       "../third_party/heatshrink/heatshrink_decoder.c"
                       PRIV_REQUIRES spi_flash esp_driver_gpio esp_driver_spi esp_timer
                                     esp_wifi esp_event esp_netif esp_http_server
                                     nvs_flash spiffs
                       INCLUDE_DIRS "" "../third_party/heatshrink")
spiffs_create_partition_image(spiffs ../spiffs FLASH_IN_PROJECT)
# 
# 