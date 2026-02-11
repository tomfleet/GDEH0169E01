#include "epd_169inch.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "pins.h"
#include "image.h"

#define MASTER_ONLY 0
#define SLAVE_ONLY 1
#define MASTER_SLAVE 2

#define TEMPTR_ON 0xFF
#define TEMPTR_OFF 0x00

#define WHITE 0x11
#define BLACK 0x00
#define RED 0x33
#define YELLOW 0x22
#define BLUE 0x55
#define GREEN 0x66

#define PIC_HALF 0xFC
#define PIC_A 0xFD
#define STRIPE 0xFE
#define IMAGE 0xFF

static const char *TAG = "epd_169";

static uint8_t temptr_cur;
static uint8_t otp_pwr[5];

static inline void delay_us(uint32_t time_us)
{
    if (time_us > 0) {
        esp_rom_delay_us(time_us);
    }
}

static void delay_ms(uint32_t time_ms)
{
    if (time_ms == 0) {
        return;
    }

    if (time_ms <= 2) {
        esp_rom_delay_us(time_ms * 1000U);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(time_ms));
}

static void delay_s(uint32_t time_s)
{
    if (time_s == 0) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(time_s * 1000U));
}

static inline void sda_high(void) { gpio_set_level(PIN_SDA, 1); }
static inline void sda_low(void) { gpio_set_level(PIN_SDA, 0); }
static inline void sclk_high(void) { gpio_set_level(PIN_SCL, 1); }
static inline void sclk_low(void) { gpio_set_level(PIN_SCL, 0); }
static inline void nrst_high(void) { gpio_set_level(PIN_RES, 1); }
static inline void nrst_low(void) { gpio_set_level(PIN_RES, 0); }
static inline void ndc_high(void) { gpio_set_level(PIN_DC, 1); }
static inline void ndc_low(void) { gpio_set_level(PIN_DC, 0); }
static inline void csb_high(void) { gpio_set_level(PIN_CS, 1); }
static inline void csb_low(void) { gpio_set_level(PIN_CS, 0); }
static inline void csb2_high(void) { gpio_set_level(PIN_CSB2, 1); }
static inline void csb2_low(void) { gpio_set_level(PIN_CSB2, 0); }
static inline void ms_high(void) { gpio_set_level(PIN_MS, 1); }
static inline void ms_low(void) { gpio_set_level(PIN_MS, 0); }

static void sys_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_RES) | (1ULL << PIN_DC) | (1ULL << PIN_CS) |
                        (1ULL << PIN_CSB2) | (1ULL << PIN_MS) | (1ULL << PIN_SCL) |
                        (1ULL << PIN_SDA),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_BUSY);
    gpio_config(&io_conf);

    nrst_high();
    ndc_high();
    csb_high();
    csb2_high();
    ms_high();
    sclk_low();
    sda_high();

    gpio_set_level(PIN_RES, 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_CSB2, 1);
    gpio_set_level(PIN_MS, 1);
    gpio_set_level(PIN_SCL, 0);
    gpio_set_level(PIN_SDA, 1);
}

static void read_busy(void)
{
    int64_t deadline_us = esp_timer_get_time() + 10 * 1000 * 1000;

    while (true) {
        if (gpio_get_level(PIN_BUSY)) {
            ESP_LOGI(TAG, "Busy signal cleared");
            break;
        }

        delay_ms(10);
        if (esp_timer_get_time() > deadline_us) {
            ESP_LOGW(TAG, "Busy signal timeout");
            break;
        }
    }
}

static void reset_panel(void)
{
    nrst_high();
    delay_ms(30);
    nrst_low();
    delay_ms(30);
    nrst_high();
    delay_ms(100);
    ESP_LOGI(TAG, "Reset completed");
}

static void spi4w_write_com(uint8_t init_com)
{
    uint8_t temp_com = init_com;

    gpio_set_direction(PIN_SDA, GPIO_MODE_OUTPUT);
    sclk_low();
    delay_us(2);
    ndc_low();
    delay_us(2);

    for (uint8_t scnt = 0; scnt < 8; scnt++) {
        if (temp_com & 0x80) {
            sda_high();
        } else {
            sda_low();
        }

        delay_us(1);
        sclk_high();
        delay_us(2);
        sclk_low();
        temp_com <<= 1;
    }

    delay_us(2);
}

static void spi4w_write_data(uint8_t init_data)
{
    uint8_t temp_data = init_data;

    gpio_set_direction(PIN_SDA, GPIO_MODE_OUTPUT);
    sclk_low();
    delay_us(2);
    ndc_high();
    delay_us(2);

    for (uint8_t scnt = 0; scnt < 8; scnt++) {
        if (temp_data & 0x80) {
            sda_high();
        } else {
            sda_low();
        }

        delay_us(1);
        sclk_high();
        delay_us(2);
        sclk_low();
        temp_data <<= 1;
    }

    delay_us(2);
}

static uint8_t spi4w_read_data(void)
{
    uint8_t temp = 0;

    gpio_set_direction(PIN_SDA, GPIO_MODE_INPUT);
    delay_us(1);
    sclk_low();
    delay_us(2);
    ndc_high();
    delay_us(2);

    for (uint8_t scnt = 0; scnt < 8; scnt++) {
        sclk_high();
        delay_us(2);
        if (gpio_get_level(PIN_SDA)) {
            temp = (temp << 1) | 0x01;
        } else {
            temp <<= 1;
        }

        delay_us(1);
        sclk_low();
        delay_us(2);
    }

    delay_us(2);
    gpio_set_direction(PIN_SDA, GPIO_MODE_OUTPUT);
    return temp;
}

static void msdev_write_com(uint8_t ms_opt, uint8_t init_com)
{
    if (ms_opt == MASTER_ONLY) {
        csb_low();
        csb2_high();
    } else if (ms_opt == SLAVE_ONLY) {
        csb_high();
        csb2_low();
    } else {
        csb_low();
        csb2_low();
    }

    delay_us(10);
    spi4w_write_com(init_com);
    delay_us(10);
    csb_high();
    csb2_high();
    delay_us(10);
}

static void msdev_write_data(uint8_t ms_opt, uint8_t init_data)
{
    if (ms_opt == MASTER_ONLY) {
        csb_low();
        csb2_high();
    } else if (ms_opt == SLAVE_ONLY) {
        csb_high();
        csb2_low();
    } else {
        csb_low();
        csb2_low();
    }

    delay_us(10);
    spi4w_write_data(init_data);
    delay_us(10);
    csb_high();
    csb2_high();
    delay_us(10);
}

static uint8_t msdev_read_data(uint8_t ms_opt)
{
    uint8_t temp = 0;

    if (ms_opt == MASTER_ONLY) {
        csb_low();
        csb2_high();
    } else if (ms_opt == SLAVE_ONLY) {
        csb_high();
        csb2_low();
    } else {
        csb_low();
        csb2_low();
    }

    delay_us(10);
    temp = spi4w_read_data();
    delay_us(10);
    csb_high();
    csb2_high();
    delay_us(10);

    return temp;
}

static uint8_t read_temptr(void)
{
    uint8_t temptr_intgr;

    msdev_write_com(MASTER_ONLY, 0x40);
    delay_ms(100);
    read_busy();
    temptr_intgr = msdev_read_data(MASTER_ONLY);
    (void)msdev_read_data(MASTER_ONLY);

    temptr_cur = temptr_intgr;
    return temptr_intgr;
}

static void write_temptr(uint8_t temptr_lock)
{
    msdev_write_com(MASTER_SLAVE, 0xE0);
    msdev_write_data(MASTER_SLAVE, 0x03);

    msdev_write_com(MASTER_SLAVE, 0xE5);
    msdev_write_data(MASTER_SLAVE, temptr_lock);
    read_busy();
}

static void read_otp_pwr(uint8_t temptr_opt)
{
    uint8_t otp_vcom;
    uint8_t temptr_val;

    ms_high();
    reset_panel();
    ms_low();

    msdev_write_com(MASTER_SLAVE, 0x00);
    msdev_write_data(MASTER_SLAVE, 0x0F);
    msdev_write_data(MASTER_SLAVE, 0x69);

    read_temptr();

    if (temptr_opt > 0 && temptr_opt != TEMPTR_ON) {
        temptr_val = temptr_opt;
    } else {
        temptr_val = temptr_cur;
    }

    ms_high();
    delay_us(10);

    msdev_write_com(MASTER_SLAVE, 0x00);
    msdev_write_data(MASTER_SLAVE, 0x0F);
    msdev_write_data(MASTER_SLAVE, 0x69);

    msdev_write_com(MASTER_SLAVE, 0x01);
    msdev_write_data(MASTER_SLAVE, 0x00);

    write_temptr(temptr_val);

    msdev_write_com(MASTER_SLAVE, 0x04);
    read_busy();
    delay_ms(10);

    msdev_write_com(MASTER_SLAVE, 0x02);
    msdev_write_data(MASTER_SLAVE, 0x00);
    read_busy();
    delay_ms(10);

    ms_low();
    delay_us(10);

    msdev_write_com(MASTER_SLAVE, 0xF0);
    (void)msdev_read_data(MASTER_ONLY);

    for (uint16_t j = 0; j < 207; j++) {
        (void)msdev_read_data(MASTER_ONLY);
    }

    otp_vcom = msdev_read_data(MASTER_ONLY);

    msdev_write_com(MASTER_SLAVE, 0xF5);
    msdev_write_data(MASTER_SLAVE, 0xA5);

    msdev_write_com(MASTER_SLAVE, 0x94);
    (void)msdev_read_data(MASTER_ONLY);

    for (uint8_t i = 0; i < 5; i++) {
        otp_pwr[i] = msdev_read_data(MASTER_ONLY);
    }

    msdev_write_com(MASTER_SLAVE, 0xF5);
    msdev_write_data(MASTER_SLAVE, 0x00);

    ms_high();
    reset_panel();
    ms_low();

    msdev_write_com(MASTER_SLAVE, 0x66);
    msdev_write_data(MASTER_SLAVE, 0x49);
    msdev_write_data(MASTER_SLAVE, 0x55);
    msdev_write_data(MASTER_SLAVE, 0x13);
    msdev_write_data(MASTER_SLAVE, 0x5D);
    msdev_write_data(MASTER_SLAVE, 0x05);
    msdev_write_data(MASTER_SLAVE, 0x10);

    msdev_write_com(MASTER_SLAVE, 0x13);
    msdev_write_data(MASTER_SLAVE, 0x00);
    msdev_write_data(MASTER_SLAVE, 0x00);

    msdev_write_com(MASTER_SLAVE, 0xE0);
    msdev_write_data(MASTER_SLAVE, 0x01);

    msdev_write_com(MASTER_SLAVE, 0x00);
    msdev_write_data(MASTER_SLAVE, 0x13);
    msdev_write_data(MASTER_SLAVE, 0xE9);

    msdev_write_com(MASTER_SLAVE, 0x01);
    msdev_write_data(MASTER_SLAVE, 0x0F);
    msdev_write_data(MASTER_SLAVE, otp_pwr[0]);
    msdev_write_data(MASTER_SLAVE, otp_pwr[1]);
    msdev_write_data(MASTER_SLAVE, otp_pwr[2]);
    msdev_write_data(MASTER_SLAVE, otp_pwr[3]);
    msdev_write_data(MASTER_SLAVE, otp_pwr[4]);

    msdev_write_com(MASTER_SLAVE, 0x06);
    msdev_write_data(MASTER_SLAVE, 0xD7);
    msdev_write_data(MASTER_SLAVE, 0xDE);
    msdev_write_data(MASTER_SLAVE, 0x12);

    msdev_write_com(MASTER_SLAVE, 0x61);
    msdev_write_data(MASTER_SLAVE, 0x00);
    msdev_write_data(MASTER_SLAVE, 0xC8);
    msdev_write_data(MASTER_SLAVE, 0x01);
    msdev_write_data(MASTER_SLAVE, 0x90);

    msdev_write_com(MASTER_SLAVE, 0x82);
    msdev_write_data(MASTER_SLAVE, otp_vcom);

    msdev_write_com(MASTER_SLAVE, 0xE3);
    msdev_write_data(MASTER_SLAVE, 0x01);

    msdev_write_com(MASTER_SLAVE, 0xE9);
    msdev_write_data(MASTER_SLAVE, 0x01);
}

static void enter_deepsleep(void)
{
    msdev_write_com(MASTER_SLAVE, 0x07);
    msdev_write_data(MASTER_SLAVE, 0xA5);
    ESP_LOGI(TAG, "Entered deep sleep");
}

static void send_hv_stripe_data(void)
{
    ESP_LOGI(TAG, "Sending stripe data to MASTER");
    msdev_write_com(MASTER_ONLY, 0x00);
    msdev_write_data(MASTER_ONLY, 0x13);
    msdev_write_data(MASTER_ONLY, 0xE9);

    msdev_write_com(MASTER_ONLY, 0x10);
    delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            if (col >= 82 && col < 200 && row >= 10 && row <= 36) {
                msdev_write_data(MASTER_ONLY, WHITE);
            } else if (col >= 82 && col < 200 && row > 36 && row <= 62) {
                msdev_write_data(MASTER_ONLY, YELLOW);
            } else if (col >= 82 && col < 200 && row > 62 && row <= 89) {
                msdev_write_data(MASTER_ONLY, GREEN);
            } else if (col >= 200 && col < 318 && row >= 10 && row <= 36) {
                msdev_write_data(MASTER_ONLY, BLACK);
            } else if (col >= 200 && col < 318 && row > 36 && row <= 62) {
                msdev_write_data(MASTER_ONLY, BLUE);
            } else if (col >= 200 && col < 318 && row > 62 && row <= 89) {
                msdev_write_data(MASTER_ONLY, RED);
            } else {
                msdev_write_data(MASTER_ONLY, WHITE);
            }
        }
    }

    ESP_LOGI(TAG, "Sending stripe data to SLAVE");
    msdev_write_com(SLAVE_ONLY, 0x00);
    msdev_write_data(SLAVE_ONLY, 0x17);
    msdev_write_data(SLAVE_ONLY, 0xE9);

    msdev_write_com(SLAVE_ONLY, 0x10);
    delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            if (col >= 82 && col < 200 && row >= 10 && row <= 36) {
                msdev_write_data(SLAVE_ONLY, WHITE);
            } else if (col >= 82 && col < 200 && row > 36 && row <= 62) {
                msdev_write_data(SLAVE_ONLY, YELLOW);
            } else if (col >= 82 && col < 200 && row > 62 && row <= 89) {
                msdev_write_data(SLAVE_ONLY, GREEN);
            } else if (col >= 200 && col < 318 && row >= 10 && row <= 36) {
                msdev_write_data(SLAVE_ONLY, BLACK);
            } else if (col >= 200 && col < 318 && row > 36 && row <= 62) {
                msdev_write_data(SLAVE_ONLY, BLUE);
            } else if (col >= 200 && col < 318 && row > 62 && row <= 89) {
                msdev_write_data(SLAVE_ONLY, RED);
            } else {
                msdev_write_data(SLAVE_ONLY, WHITE);
            }
        }
    }

    ESP_LOGI(TAG, "Stripe data sent");
}

static void send_hv_stripe_image_data(const uint8_t *pic)
{
    ESP_LOGI(TAG, "Sending image data to MASTER (rows 0-99)");
    msdev_write_com(MASTER_ONLY, 0x00);
    msdev_write_data(MASTER_ONLY, 0x13);
    msdev_write_data(MASTER_ONLY, 0xE9);

    msdev_write_com(MASTER_ONLY, 0x10);
    delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            uint32_t index = (col * 200U) + (row * 2U);
            uint8_t temp1 = (uint8_t)(pic[index] & 0xF0);
            uint8_t temp2 = (uint8_t)(pic[index + 1] >> 4);
            uint8_t temp = (uint8_t)(temp1 | temp2);
            msdev_write_data(MASTER_ONLY, temp);
        }
    }

    ESP_LOGI(TAG, "Sending image data to SLAVE (rows 100-199)");
    msdev_write_com(SLAVE_ONLY, 0x00);
    msdev_write_data(SLAVE_ONLY, 0x17);
    msdev_write_data(SLAVE_ONLY, 0xE9);

    msdev_write_com(SLAVE_ONLY, 0x10);
    delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            uint32_t index = (col * 200U) + (row * 2U);
            uint8_t temp1 = (uint8_t)((pic[index] & 0x0F) << 4);
            uint8_t temp2 = (uint8_t)(pic[index + 1] & 0x0F);
            uint8_t temp = (uint8_t)(temp1 | temp2);
            msdev_write_data(SLAVE_ONLY, temp);
        }
    }

    ESP_LOGI(TAG, "Image data sent");
}

static void send_hv_stripe_clean_data(void)
{
    ESP_LOGI(TAG, "Sending full white data to MASTER");
    msdev_write_com(MASTER_ONLY, 0x00);
    msdev_write_data(MASTER_ONLY, 0x13);
    msdev_write_data(MASTER_ONLY, 0xE9);

    msdev_write_com(MASTER_ONLY, 0x10);
    delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            msdev_write_data(MASTER_ONLY, WHITE);
        }
    }

    ESP_LOGI(TAG, "Sending full white data to SLAVE");
    msdev_write_com(SLAVE_ONLY, 0x00);
    msdev_write_data(SLAVE_ONLY, 0x17);
    msdev_write_data(SLAVE_ONLY, 0xE9);

    msdev_write_com(SLAVE_ONLY, 0x10);
    delay_ms(10);
    for (uint16_t col = 0; col < 400; col++) {
        for (uint16_t row = 0; row < 100; row++) {
            msdev_write_data(SLAVE_ONLY, WHITE);
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
    msdev_write_com(MASTER_SLAVE, 0x04);
    read_busy();

    ESP_LOGI(TAG, "Sending refresh command");
    msdev_write_com(MASTER_SLAVE, 0x12);
    msdev_write_data(MASTER_SLAVE, 0x00);
    delay_ms(10);
    read_busy();

    ESP_LOGI(TAG, "Sending power off command");
    msdev_write_com(MASTER_SLAVE, 0x02);
    msdev_write_data(MASTER_SLAVE, 0x00);
    read_busy();
    delay_ms(20);
    ESP_LOGI(TAG, "EPD display completed");
}

void epd_demo_run(void)
{
    sys_init();

    ESP_LOGI(TAG, "ESP32-S3 EPD Color Bar Test Start");

    ESP_LOGI(TAG, "Testing SPI communication");
    reset_panel();
    read_busy();
    uint8_t temp = read_temptr();
    ESP_LOGI(TAG, "IC Temperature: %u C", temp);

    if (temp < 10 || temp > 50) {
        ESP_LOGW(TAG, "SPI communication may have issues. Check connections!");
    }

    ESP_LOGI(TAG, "Starting color bar display");
    reset_panel();
    read_busy();
    read_otp_pwr(TEMPTR_ON);
    send_hv_stripe_data();
    epd_display(PIC_A);
    enter_deepsleep();

    delay_s(3);

    read_otp_pwr(TEMPTR_ON);
    send_hv_stripe_image_data(gImage1);
    epd_display(PIC_A);
    enter_deepsleep();

    delay_s(3);

    read_otp_pwr(TEMPTR_ON);
    send_hv_stripe_image_data(gImage2);
    epd_display(PIC_A);
    enter_deepsleep();

    delay_s(3);

    read_otp_pwr(TEMPTR_ON);
    send_hv_stripe_clean_data();
    epd_display(PIC_A);
    enter_deepsleep();
}
