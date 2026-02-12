#include "scd30_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "epd_169inch.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scd30_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"

#define PANEL_WIDTH 400
#define PANEL_HEIGHT 400

#define COLOR_BLACK 0x0
#define COLOR_WHITE 0x1
#define COLOR_YELLOW 0x2
#define COLOR_RED 0x3
#define COLOR_BLUE 0x5
#define COLOR_GREEN 0x6

static const char *TAG = "scd30";

static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;
static scd30_reading_t s_latest = {0};

static uint8_t *s_sp6;

static bool alloc_buffers(void)
{
    if (s_sp6) {
        return true;
    }

    size_t sp6_size = (PANEL_WIDTH * PANEL_HEIGHT) / 2U;

    s_sp6 = heap_caps_malloc(sp6_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!s_sp6) {
        s_sp6 = malloc(sp6_size);
    }

    if (!s_sp6) {
        free(s_sp6);
        s_sp6 = NULL;
        ESP_LOGE(TAG, "Failed to allocate display buffers");
        return false;
    }

    return true;
}

static const uint8_t *glyph_for(char c) {
    switch (c) {
        case '0': {
            static const uint8_t g[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E};
            return g;
        }
        case '1': {
            static const uint8_t g[5] = {0x00, 0x42, 0x7F, 0x40, 0x00};
            return g;
        }
        case '2': {
            static const uint8_t g[5] = {0x62, 0x51, 0x49, 0x49, 0x46};
            return g;
        }
        case '3': {
            static const uint8_t g[5] = {0x22, 0x49, 0x49, 0x49, 0x36};
            return g;
        }
        case '4': {
            static const uint8_t g[5] = {0x18, 0x14, 0x12, 0x7F, 0x10};
            return g;
        }
        case '5': {
            static const uint8_t g[5] = {0x2F, 0x49, 0x49, 0x49, 0x31};
            return g;
        }
        case '6': {
            static const uint8_t g[5] = {0x3E, 0x49, 0x49, 0x49, 0x32};
            return g;
        }
        case '7': {
            static const uint8_t g[5] = {0x01, 0x71, 0x09, 0x05, 0x03};
            return g;
        }
        case '8': {
            static const uint8_t g[5] = {0x36, 0x49, 0x49, 0x49, 0x36};
            return g;
        }
        case '9': {
            static const uint8_t g[5] = {0x26, 0x49, 0x49, 0x49, 0x3E};
            return g;
        }
        case 'C': {
            static const uint8_t g[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
            return g;
        }
        case 'O': {
            static const uint8_t g[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
            return g;
        }
        case 'P': {
            static const uint8_t g[5] = {0x7F, 0x09, 0x09, 0x09, 0x06};
            return g;
        }
        case 'M': {
            static const uint8_t g[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F};
            return g;
        }
        case 'T': {
            static const uint8_t g[5] = {0x01, 0x01, 0x7F, 0x01, 0x01};
            return g;
        }
        case 'E': {
            static const uint8_t g[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
            return g;
        }
        case 'R': {
            static const uint8_t g[5] = {0x7F, 0x09, 0x19, 0x29, 0x46};
            return g;
        }
        case 'H': {
            static const uint8_t g[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
            return g;
        }
        case '.': {
            static const uint8_t g[5] = {0x00, 0x40, 0x60, 0x00, 0x00};
            return g;
        }
        case '%': {
            static const uint8_t g[5] = {0x62, 0x64, 0x08, 0x13, 0x23};
            return g;
        }
        case '-': {
            static const uint8_t g[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
            return g;
        }
        case ' ': {
            static const uint8_t g[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
            return g;
        }
        default: {
            static const uint8_t g[5] = {0x7F, 0x41, 0x41, 0x41, 0x7F};
            return g;
        }
    }
}

static void set_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT) {
        return;
    }

    int rx = (PANEL_WIDTH - 1) - x;
    int ry = (PANEL_HEIGHT - 1) - y;
    int row_offset = ry * (PANEL_WIDTH / 2);
    int out_index;
    int sp6_size = (PANEL_WIDTH * PANEL_HEIGHT) / 2;

    if ((rx & 1) == 0) {
        out_index = row_offset + ((PANEL_WIDTH - rx) / 2);
        if (out_index >= 0 && out_index < sp6_size) {
            s_sp6[out_index] = (s_sp6[out_index] & 0x0F) | (uint8_t)(color << 4);
        }
    } else {
        out_index = row_offset + ((PANEL_WIDTH - rx - 1) / 2);
        if (out_index >= 0 && out_index < sp6_size) {
            s_sp6[out_index] = (s_sp6[out_index] & 0xF0) | (color & 0x0F);
        }
    }
}

static void draw_char(int x, int y, char c, uint8_t color, int scale) {
    const uint8_t *glyph = glyph_for(c);
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sx = 0; sx < scale; sx++) {
                    for (int sy = 0; sy < scale; sy++) {
                        int px = x + (col * scale) + sx;
                        int py = y + (row * scale) + sy;
                        set_pixel(px, py, color);
                    }
                }
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint8_t color, int scale) {
    int cx = x;
    for (const char *p = text; *p; p++) {
        draw_char(cx, y, *p, color, scale);
        cx += (6 * scale);
    }
}

static void render_screen(float co2, float temp, float rh) {
    if (!alloc_buffers()) {
        return;
    }
    size_t sp6_size = (PANEL_WIDTH * PANEL_HEIGHT) / 2U;
    memset(s_sp6, 0x11, sp6_size);

    char line1[32];
    char line2[32];
    char line3[32];
    snprintf(line1, sizeof(line1), "CO2 %4d ppm", (int)(co2 + 0.5f));
    snprintf(line2, sizeof(line2), "T %.1f C", temp);
    snprintf(line3, sizeof(line3), "RH %.1f %%", rh);

    draw_text(20, 40, line1, COLOR_BLACK, 3);
    draw_text(20, 140, line2, COLOR_BLACK, 3);
    draw_text(20, 240, line3, COLOR_BLACK, 3);

    epd_show_image(s_sp6, sp6_size);
}

static void scd30_task(void *arg) {
    (void)arg;
    sensirion_i2c_hal_init();
    scd30_init(SCD30_I2C_ADDR);

    scd30_stop_periodic_measurement();
    scd30_soft_reset();
    sensirion_i2c_hal_sleep_usec(2000000);

    int16_t err = scd30_start_periodic_measurement(0);
    if (err != NO_ERROR) {
        ESP_LOGE(TAG, "SCD30 start failed: %d", err);
    }

    for (;;) {
        float co2 = 0.0f;
        float temperature = 0.0f;
        float humidity = 0.0f;

        err = scd30_blocking_read_measurement_data(&co2, &temperature, &humidity);
        if (err == NO_ERROR) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            portENTER_CRITICAL(&s_data_lock);
            s_latest.co2_ppm = co2;
            s_latest.temperature_c = temperature;
            s_latest.humidity_rh = humidity;
            s_latest.age_ms = now_ms;
            s_latest.valid = true;
            portEXIT_CRITICAL(&s_data_lock);

            ESP_LOGI(TAG, "CO2 %.2f ppm, T %.2f C, RH %.2f %%", co2, temperature, humidity);
            // Defer EPD updates; keep readings only.

            (void)now_ms;
        } else {
            ESP_LOGW(TAG, "SCD30 read error: %d", err);
        }

        vTaskDelay(pdMS_TO_TICKS(SCD30_READ_INTERVAL_SEC * 1000U));
    }
}

void scd30_app_start(void) {
    xTaskCreate(scd30_task, "scd30_task", 4096, NULL, 5, NULL);
}

bool scd30_get_latest(scd30_reading_t *out) {
    if (!out) {
        return false;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    portENTER_CRITICAL(&s_data_lock);
    *out = s_latest;
    portEXIT_CRITICAL(&s_data_lock);

    if (!out->valid) {
        return false;
    }
    out->age_ms = now_ms - out->age_ms;
    return true;
}
