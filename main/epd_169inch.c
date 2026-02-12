#include "epd_169inch.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "epd_169inch_bus.h"

#define TEMPTR_ON 0xFF
#define TEMPTR_OFF 0x00

#define WHITE 0x11
#define BLACK 0x00
#define RED 0x33
#define YELLOW 0x22
#define BLUE 0x55
#define GREEN 0x66

#define PIC_A 0xFD
#define STRIPE 0xFE

static const char *TAG = "epd_169";

static uint8_t temptr_cur;
static uint8_t otp_pwr[5];
static bool epd_ready;

static uint8_t read_temptr(void)
{
    uint8_t temptr_intgr;

    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x40);
    epd_bus_delay_ms(100);
    epd_bus_wait_busy();
    temptr_intgr = epd_bus_read_data(EPD_MASTER_ONLY);
    (void)epd_bus_read_data(EPD_MASTER_ONLY);

    temptr_cur = temptr_intgr;
    return temptr_intgr;
}

static void write_temptr(uint8_t temptr_lock)
{
    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xE0);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x03);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xE5);
    epd_bus_write_data(EPD_MASTER_SLAVE, temptr_lock);
    epd_bus_wait_busy();
}

static void read_otp_pwr(uint8_t temptr_opt)
{
    uint8_t otp_vcom;
    uint8_t temptr_val;

    epd_bus_set_master_mode(true);
    epd_bus_reset();
    epd_bus_set_master_mode(false);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x00);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x0F);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x69);

    read_temptr();

    if (temptr_opt > 0 && temptr_opt != TEMPTR_ON) {
        temptr_val = temptr_opt;
    } else {
        temptr_val = temptr_cur;
    }

    epd_bus_set_master_mode(true);
    epd_bus_delay_ms(1);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x00);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x0F);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x69);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x01);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);

    write_temptr(temptr_val);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x04);
    epd_bus_wait_busy();
    epd_bus_delay_ms(10);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x02);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);
    epd_bus_wait_busy();
    epd_bus_delay_ms(10);

    epd_bus_set_master_mode(false);
    epd_bus_delay_ms(1);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xF0);
    (void)epd_bus_read_data(EPD_MASTER_ONLY);

    for (uint16_t j = 0; j < 207; j++) {
        (void)epd_bus_read_data(EPD_MASTER_ONLY);
    }

    otp_vcom = epd_bus_read_data(EPD_MASTER_ONLY);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xF5);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0xA5);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x94);
    (void)epd_bus_read_data(EPD_MASTER_ONLY);

    for (uint8_t i = 0; i < 5; i++) {
        otp_pwr[i] = epd_bus_read_data(EPD_MASTER_ONLY);
    }

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xF5);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);

    epd_bus_set_master_mode(true);
    epd_bus_reset();
    epd_bus_set_master_mode(false);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x66);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x49);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x55);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x13);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x5D);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x05);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x10);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x13);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xE0);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x01);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x00);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x13);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0xE9);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x01);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x0F);
    epd_bus_write_data(EPD_MASTER_SLAVE, otp_pwr[0]);
    epd_bus_write_data(EPD_MASTER_SLAVE, otp_pwr[1]);
    epd_bus_write_data(EPD_MASTER_SLAVE, otp_pwr[2]);
    epd_bus_write_data(EPD_MASTER_SLAVE, otp_pwr[3]);
    epd_bus_write_data(EPD_MASTER_SLAVE, otp_pwr[4]);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x06);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0xD7);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0xDE);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x12);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x61);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0xC8);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x01);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x90);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x82);
    epd_bus_write_data(EPD_MASTER_SLAVE, otp_vcom);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xE3);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x01);

    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0xE9);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x01);
}

static void enter_deepsleep(void)
{
    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x07);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0xA5);
    ESP_LOGI(TAG, "Entered deep sleep");
}

static void send_hv_stripe_data(void)
{
    uint32_t yield_counter = 0;

    ESP_LOGI(TAG, "Sending stripe data to MASTER");
    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x00);
    epd_bus_write_data(EPD_MASTER_ONLY, 0x13);
    epd_bus_write_data(EPD_MASTER_ONLY, 0xE9);

    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x10);
    epd_bus_delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            if (col >= 82 && col < 200 && row >= 10 && row <= 36) {
                epd_bus_write_data(EPD_MASTER_ONLY, WHITE);
            } else if (col >= 82 && col < 200 && row > 36 && row <= 62) {
                epd_bus_write_data(EPD_MASTER_ONLY, YELLOW);
            } else if (col >= 82 && col < 200 && row > 62 && row <= 89) {
                epd_bus_write_data(EPD_MASTER_ONLY, GREEN);
            } else if (col >= 200 && col < 318 && row >= 10 && row <= 36) {
                epd_bus_write_data(EPD_MASTER_ONLY, BLACK);
            } else if (col >= 200 && col < 318 && row > 36 && row <= 62) {
                epd_bus_write_data(EPD_MASTER_ONLY, BLUE);
            } else if (col >= 200 && col < 318 && row > 62 && row <= 89) {
                epd_bus_write_data(EPD_MASTER_ONLY, RED);
            } else {
                epd_bus_write_data(EPD_MASTER_ONLY, WHITE);
            }
            yield_counter++;
            if ((yield_counter % 2000U) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    ESP_LOGI(TAG, "Sending stripe data to SLAVE");
    epd_bus_write_cmd(EPD_SLAVE_ONLY, 0x00);
    epd_bus_write_data(EPD_SLAVE_ONLY, 0x17);
    epd_bus_write_data(EPD_SLAVE_ONLY, 0xE9);

    epd_bus_write_cmd(EPD_SLAVE_ONLY, 0x10);
    epd_bus_delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            if (col >= 82 && col < 200 && row >= 10 && row <= 36) {
                epd_bus_write_data(EPD_SLAVE_ONLY, WHITE);
            } else if (col >= 82 && col < 200 && row > 36 && row <= 62) {
                epd_bus_write_data(EPD_SLAVE_ONLY, YELLOW);
            } else if (col >= 82 && col < 200 && row > 62 && row <= 89) {
                epd_bus_write_data(EPD_SLAVE_ONLY, GREEN);
            } else if (col >= 200 && col < 318 && row >= 10 && row <= 36) {
                epd_bus_write_data(EPD_SLAVE_ONLY, BLACK);
            } else if (col >= 200 && col < 318 && row > 36 && row <= 62) {
                epd_bus_write_data(EPD_SLAVE_ONLY, BLUE);
            } else if (col >= 200 && col < 318 && row > 62 && row <= 89) {
                epd_bus_write_data(EPD_SLAVE_ONLY, RED);
            } else {
                epd_bus_write_data(EPD_SLAVE_ONLY, WHITE);
            }
            yield_counter++;
            if ((yield_counter % 2000U) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    ESP_LOGI(TAG, "Stripe data sent");
}

static void send_hv_stripe_image_data(const uint8_t *pic)
{
    uint32_t yield_counter = 0;

    ESP_LOGI(TAG, "Sending image data to MASTER (rows 0-99)");
    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x00);
    epd_bus_write_data(EPD_MASTER_ONLY, 0x13);
    epd_bus_write_data(EPD_MASTER_ONLY, 0xE9);

    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x10);
    epd_bus_delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            uint32_t index = (col * 200U) + (row * 2U);
            uint8_t temp1 = (uint8_t)(pic[index] & 0xF0);
            uint8_t temp2 = (uint8_t)(pic[index + 1] >> 4);
            uint8_t temp = (uint8_t)(temp1 | temp2);
            epd_bus_write_data(EPD_MASTER_ONLY, temp);
            yield_counter++;
            if ((yield_counter % 2000U) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    ESP_LOGI(TAG, "Sending image data to SLAVE (rows 100-199)");
    epd_bus_write_cmd(EPD_SLAVE_ONLY, 0x00);
    epd_bus_write_data(EPD_SLAVE_ONLY, 0x17);
    epd_bus_write_data(EPD_SLAVE_ONLY, 0xE9);

    epd_bus_write_cmd(EPD_SLAVE_ONLY, 0x10);
    epd_bus_delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            uint32_t index = (col * 200U) + (row * 2U);
            uint8_t temp1 = (uint8_t)((pic[index] & 0x0F) << 4);
            uint8_t temp2 = (uint8_t)(pic[index + 1] & 0x0F);
            uint8_t temp = (uint8_t)(temp1 | temp2);
            epd_bus_write_data(EPD_SLAVE_ONLY, temp);
            yield_counter++;
            if ((yield_counter % 2000U) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    ESP_LOGI(TAG, "Image data sent");
}

static void send_hv_stripe_clean_data(void)
{
    uint32_t yield_counter = 0;

    ESP_LOGI(TAG, "Sending full white data to MASTER");
    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x00);
    epd_bus_write_data(EPD_MASTER_ONLY, 0x13);
    epd_bus_write_data(EPD_MASTER_ONLY, 0xE9);

    epd_bus_write_cmd(EPD_MASTER_ONLY, 0x10);
    epd_bus_delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            epd_bus_write_data(EPD_MASTER_ONLY, WHITE);
            yield_counter++;
            if ((yield_counter % 2000U) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    ESP_LOGI(TAG, "Sending full white data to SLAVE");
    epd_bus_write_cmd(EPD_SLAVE_ONLY, 0x00);
    epd_bus_write_data(EPD_SLAVE_ONLY, 0x17);
    epd_bus_write_data(EPD_SLAVE_ONLY, 0xE9);

    epd_bus_write_cmd(EPD_SLAVE_ONLY, 0x10);
    epd_bus_delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            epd_bus_write_data(EPD_SLAVE_ONLY, WHITE);
            yield_counter++;
            if ((yield_counter % 2000U) == 0U) {
                vTaskDelay(1);
            }
        }
    }

    ESP_LOGI(TAG, "Full white data sent");
}

static void epd_display(uint8_t display_bkg)
{
    ESP_LOGI(TAG, "Starting EPD display");
    if (display_bkg == STRIPE) {
        send_hv_stripe_data();
    }

    ESP_LOGI(TAG, "Sending power on command");
    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x04);
    epd_bus_wait_busy();

    ESP_LOGI(TAG, "Sending refresh command");
    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x12);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);
    epd_bus_delay_ms(10);
    epd_bus_wait_busy();

    ESP_LOGI(TAG, "Sending power off command");
    epd_bus_write_cmd(EPD_MASTER_SLAVE, 0x02);
    epd_bus_write_data(EPD_MASTER_SLAVE, 0x00);
    epd_bus_wait_busy();
    epd_bus_delay_ms(20);
    ESP_LOGI(TAG, "EPD display completed");
}

void epd_setup(void)
{
    if (epd_ready) {
        return;
    }

    epd_bus_init();
    epd_bus_reset();
    epd_bus_wait_busy();
    epd_ready = true;
}

void epd_show_image(const uint8_t *image_data, size_t length)
{
    const size_t expected = 400U * 400U / 2U;
    if (!image_data || length != expected) {
        ESP_LOGE(TAG, "Invalid image data length: %u (expected %u)",
                 (unsigned)length, (unsigned)expected);
        return;
    }

    if (!epd_ready) {
        epd_setup();
    }

    read_otp_pwr(TEMPTR_ON);
    send_hv_stripe_image_data(image_data);
    epd_display(PIC_A);
}

void epd_demo_run(void)
{
    epd_setup();

    ESP_LOGI(TAG, "EPD demo: stripe pattern");
    read_otp_pwr(TEMPTR_ON);
    send_hv_stripe_data();
    epd_display(PIC_A);
    enter_deepsleep();
}
