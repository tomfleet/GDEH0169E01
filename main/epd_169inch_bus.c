#include "epd_169inch_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "pins.h"

#define EPD_SPI_HOST SPI2_HOST
#define EPD_SPI_CLOCK_HZ 250000

static const char *TAG = "epd_bus";

static spi_device_handle_t spi_handle;
static bool spi_ready;

static inline void delay_us(uint32_t time_us)
{
    if (time_us > 0) {
        esp_rom_delay_us(time_us);
    }
}

void epd_bus_delay_ms(uint32_t time_ms)
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

void epd_bus_delay_s(uint32_t time_s)
{
    if (time_s == 0) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(time_s * 1000U));
}

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

static void spi_init(void)
{
    if (spi_ready) {
        return;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_SDA,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX,
    };

    esp_err_t ret = spi_bus_initialize(EPD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &devcfg, &spi_handle));
    spi_ready = true;
}

static void spi_write_byte(uint8_t value)
{
    spi_transaction_t t = {
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
    };
    t.tx_data[0] = value;
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handle, &t));
}

static uint8_t spi_read_byte(void)
{
    spi_transaction_t t = {
        .length = 0,
        .rxlength = 8,
        .flags = SPI_TRANS_USE_RXDATA,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_handle, &t));
    return t.rx_data[0];
}

static void select_target(epd_ms_target_t target)
{
    if (target == EPD_MASTER_ONLY) {
        csb_low();
        csb2_high();
    } else if (target == EPD_SLAVE_ONLY) {
        csb_high();
        csb2_low();
    } else {
        csb_low();
        csb2_low();
    }
}

void epd_bus_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_RES) | (1ULL << PIN_DC) | (1ULL << PIN_CS) |
                        (1ULL << PIN_CSB2) | (1ULL << PIN_MS),
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

    gpio_set_level(PIN_RES, 1);
    gpio_set_level(PIN_DC, 1);
    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_CSB2, 1);
    gpio_set_level(PIN_MS, 1);

    spi_init();
}

void epd_bus_wait_busy(void)
{
    int64_t deadline_us = esp_timer_get_time() + 10 * 1000 * 1000;

    while (true) {
        if (gpio_get_level(PIN_BUSY)) {
            ESP_LOGI(TAG, "Busy signal cleared");
            break;
        }

        epd_bus_delay_ms(10);
        if (esp_timer_get_time() > deadline_us) {
            ESP_LOGW(TAG, "Busy signal timeout");
            break;
        }
    }
}

void epd_bus_reset(void)
{
    nrst_high();
    epd_bus_delay_ms(30);
    nrst_low();
    epd_bus_delay_ms(30);
    nrst_high();
    epd_bus_delay_ms(100);
    ESP_LOGI(TAG, "Reset completed");
}

void epd_bus_write_cmd(epd_ms_target_t target, uint8_t cmd)
{
    select_target(target);
    delay_us(10);
    ndc_low();
    delay_us(1);
    spi_write_byte(cmd);
    delay_us(1);
    delay_us(10);
    csb_high();
    csb2_high();
    delay_us(10);
}

void epd_bus_write_data(epd_ms_target_t target, uint8_t data)
{
    select_target(target);
    delay_us(10);
    ndc_high();
    delay_us(1);
    spi_write_byte(data);
    delay_us(1);
    delay_us(10);
    csb_high();
    csb2_high();
    delay_us(10);
}

uint8_t epd_bus_read_data(epd_ms_target_t target)
{
    select_target(target);
    delay_us(10);
    ndc_high();
    delay_us(1);
    uint8_t temp = spi_read_byte();
    delay_us(10);
    csb_high();
    csb2_high();
    delay_us(10);
    return temp;
}

void epd_bus_set_master_mode(bool high)
{
    if (high) {
        ms_high();
    } else {
        ms_low();
    }
}
