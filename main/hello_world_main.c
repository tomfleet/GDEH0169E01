/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epd_169inch.h"
#include "image_upload.h"
#include "led_ws2812.h"
#include "scd30_app.h"

static volatile image_upload_status_t s_led_status = IMAGE_UPLOAD_STATUS_BOOT;

static void on_status(image_upload_status_t status, void *ctx)
{
    (void)ctx;
    s_led_status = status;
}

static void set_led(ws2812_strip_t *strip, uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_set_pixel(strip, 0, r, g, b);
    ws2812_show(strip);
}

static void led_task(void *arg)
{
    ws2812_strip_t strip;
    ws2812_clear(&strip);
    ws2812_init();

    uint8_t blue = 4;
    bool blue_up = true;
    bool red_on = false;

    for (;;) {
        switch (s_led_status) {
            case IMAGE_UPLOAD_STATUS_CONNECTING:
                set_led(&strip, 0, 0, blue);
                if (blue_up) {
                    blue = (uint8_t)(blue + 2);
                    if (blue >= 24) {
                        blue = 24;
                        blue_up = false;
                    }
                } else {
                    if (blue <= 4) {
                        blue = 4;
                        blue_up = true;
                    } else {
                        blue = (uint8_t)(blue - 2);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(60));
                break;
            case IMAGE_UPLOAD_STATUS_UPLOADING:
                set_led(&strip, 0, 0, 40);
                vTaskDelay(pdMS_TO_TICKS(120));
                break;
            case IMAGE_UPLOAD_STATUS_WIFI_FAILED:
                red_on = !red_on;
                if (red_on) {
                    set_led(&strip, 20, 0, 0);
                } else {
                    set_led(&strip, 0, 0, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case IMAGE_UPLOAD_STATUS_CONNECTED:
            case IMAGE_UPLOAD_STATUS_IDLE:
                set_led(&strip, 0, 8, 8);
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
            case IMAGE_UPLOAD_STATUS_BOOT:
            default:
                set_led(&strip, 0, 8, 0);
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
        }
    }
}

void app_main(void)
{
    printf("Hello world!\n");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    epd_setup();
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    image_upload_set_status_callback(on_status, NULL);
    scd30_app_start();
    image_upload_start(epd_show_image, 400U * 400U / 2U);
}
