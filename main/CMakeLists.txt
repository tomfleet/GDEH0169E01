idf_component_register(SRCS "hello_world_main.c"
                       "epd_169inch.c"
                       PRIV_REQUIRES spi_flash esp_driver_gpio esp_driver_spi esp_timer
                       INCLUDE_DIRS "")
