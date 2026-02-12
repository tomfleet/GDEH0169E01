#include "scd30_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "config.h"
#include "epd_169inch.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "led_ws2812.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "scd30_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"

#define PANEL_WIDTH 400
#define PANEL_HEIGHT 400
#define PANEL_RADIUS ((PANEL_WIDTH / 2) - 1)

#define GRAPH_MIRROR_X 0
#define GRAPH_MIRROR_Y 0

#define COLOR_BLACK 0x0
#define COLOR_WHITE 0x1
#define COLOR_YELLOW 0x2
#define COLOR_RED 0x3
#define COLOR_BLUE 0x5
#define COLOR_GREEN 0x6

static const char *TAG = "scd30";

#define SCD30_NVS_NAMESPACE "scd30"
#define SCD30_NVS_KEY "history"

static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;
static scd30_reading_t s_latest = {0};

typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_rh;
    uint32_t timestamp_ms;
} scd30_history_entry_t;

typedef struct {
    uint32_t count;
    uint32_t interval_sec;
    float co2_ppm[SCD30_NVS_MAX_SAMPLES];
    float temperature_c[SCD30_NVS_MAX_SAMPLES];
    float humidity_rh[SCD30_NVS_MAX_SAMPLES];
} scd30_nvs_blob_t;

static scd30_history_entry_t s_history[SCD30_HISTORY_MAX_SAMPLES];
static size_t s_history_count;
static size_t s_history_head;

static bool s_auto_render_enabled;
static uint32_t s_auto_render_interval_ms = SCD30_DISPLAY_INTERVAL_SEC * 1000U;
static uint32_t s_last_render_ms;
static uint32_t s_nvs_save_counter;
static bool s_nvs_ready;

static uint8_t *s_sp6;

static SemaphoreHandle_t s_power_lock;
static bool s_power_init;
static bool s_power_enabled;

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

static void power_domain_init(void)
{
    if (s_power_init) {
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << NEOPIXEL_PWR_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_power_enabled = (SCD30_POWER_DEFAULT_ON != 0);
    gpio_set_level(NEOPIXEL_PWR_PIN, s_power_enabled ? 1 : 0);

    s_power_lock = xSemaphoreCreateMutex();
    s_power_init = true;
}

static void power_domain_set(bool enabled)
{
    power_domain_init();
    if (s_power_enabled == enabled) {
        return;
    }
    gpio_set_level(NEOPIXEL_PWR_PIN, enabled ? 1 : 0);
    s_power_enabled = enabled;
    if (enabled) {
        vTaskDelay(pdMS_TO_TICKS(2));
        ws2812_refresh();
    }
}

static bool power_domain_take(uint32_t timeout_ms)
{
    power_domain_init();
    if (!s_power_lock) {
        return false;
    }
    return xSemaphoreTake(s_power_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void power_domain_give(void)
{
    if (s_power_lock) {
        xSemaphoreGive(s_power_lock);
    }
}

static bool scd30_sensor_begin(uint32_t timeout_ms)
{
    if (!power_domain_take(timeout_ms)) {
        return false;
    }
    power_domain_set(true);
    vTaskDelay(pdMS_TO_TICKS(SCD30_POWER_WARMUP_MS));
    return true;
}

static void scd30_sensor_end(void)
{
    if (SCD30_POWER_OFF_AFTER_READ) {
        power_domain_set(false);
    }
    power_domain_give();
}

static bool ensure_nvs_ready(void)
{
    if (!SCD30_NVS_ENABLE) {
        return false;
    }
    if (s_nvs_ready) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        s_nvs_ready = true;
        return true;
    }

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, skipping history restore");
        return false;
    }

    ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(err));
    return false;
}

static size_t fill_nvs_blob(scd30_nvs_blob_t *blob)
{
    if (!blob) {
        return 0;
    }

    size_t count = s_history_count;
    if (count == 0) {
        return 0;
    }

    if (count > SCD30_NVS_MAX_SAMPLES) {
        count = SCD30_NVS_MAX_SAMPLES;
    }

    size_t oldest = (s_history_head + SCD30_HISTORY_MAX_SAMPLES - s_history_count) %
                    SCD30_HISTORY_MAX_SAMPLES;
    size_t skip = s_history_count > count ? (s_history_count - count) : 0;
    oldest = (oldest + skip) % SCD30_HISTORY_MAX_SAMPLES;

    for (size_t i = 0; i < count; i++) {
        size_t idx = (oldest + i) % SCD30_HISTORY_MAX_SAMPLES;
        blob->co2_ppm[i] = s_history[idx].co2_ppm;
        blob->temperature_c[i] = s_history[idx].temperature_c;
        blob->humidity_rh[i] = s_history[idx].humidity_rh;
    }

    blob->count = (uint32_t)count;
    blob->interval_sec = SCD30_READ_INTERVAL_SEC;
    return count;
}

static void history_save_to_nvs(void)
{
    if (!ensure_nvs_ready()) {
        return;
    }

    scd30_nvs_blob_t blob = {0};
    size_t count;

    portENTER_CRITICAL(&s_data_lock);
    count = fill_nvs_blob(&blob);
    portEXIT_CRITICAL(&s_data_lock);

    if (count == 0) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCD30_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, SCD30_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS save failed: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static void history_maybe_save_to_nvs(void)
{
    if (!SCD30_NVS_ENABLE) {
        return;
    }

    s_nvs_save_counter++;
    if (s_nvs_save_counter < SCD30_NVS_SAVE_EVERY) {
        return;
    }
    s_nvs_save_counter = 0;
    history_save_to_nvs();
}

static void history_restore_from_nvs(void)
{
    if (!ensure_nvs_ready()) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SCD30_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    scd30_nvs_blob_t blob = {0};
    size_t blob_len = sizeof(blob);
    err = nvs_get_blob(handle, SCD30_NVS_KEY, &blob, &blob_len);
    nvs_close(handle);
    if (err != ESP_OK || blob_len != sizeof(blob)) {
        return;
    }

    size_t count = blob.count;
    if (count == 0 || count > SCD30_NVS_MAX_SAMPLES) {
        return;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t interval_ms = blob.interval_sec ? (blob.interval_sec * 1000U)
                                             : (SCD30_READ_INTERVAL_SEC * 1000U);

    portENTER_CRITICAL(&s_data_lock);
    s_history_count = 0;
    s_history_head = 0;
    for (size_t i = 0; i < count; i++) {
        size_t idx = i % SCD30_HISTORY_MAX_SAMPLES;
        uint32_t age_steps = (uint32_t)(count - 1 - i);
        s_history[idx].co2_ppm = blob.co2_ppm[i];
        s_history[idx].temperature_c = blob.temperature_c[i];
        s_history[idx].humidity_rh = blob.humidity_rh[i];
        s_history[idx].timestamp_ms = now_ms - (age_steps * interval_ms);
    }
    s_history_count = count;
    s_history_head = count % SCD30_HISTORY_MAX_SAMPLES;

    s_latest.co2_ppm = blob.co2_ppm[count - 1];
    s_latest.temperature_c = blob.temperature_c[count - 1];
    s_latest.humidity_rh = blob.humidity_rh[count - 1];
    s_latest.age_ms = now_ms;
    s_latest.valid = true;
    portEXIT_CRITICAL(&s_data_lock);

    ESP_LOGI(TAG, "Restored %u SCD30 samples from NVS", (unsigned)count);
}

static void history_add(float co2, float temp, float rh, uint32_t timestamp_ms)
{
    s_history[s_history_head].co2_ppm = co2;
    s_history[s_history_head].temperature_c = temp;
    s_history[s_history_head].humidity_rh = rh;
    s_history[s_history_head].timestamp_ms = timestamp_ms;

    s_history_head = (s_history_head + 1U) % SCD30_HISTORY_MAX_SAMPLES;
    if (s_history_count < SCD30_HISTORY_MAX_SAMPLES) {
        s_history_count++;
    }
}

static size_t copy_history(uint32_t now_ms, scd30_history_point_t *out, size_t max,
                           scd30_minmax_t *out_minmax)
{
    uint32_t window_ms = SCD30_HISTORY_WINDOW_SEC * 1000U;
    uint32_t cutoff = (now_ms > window_ms) ? (now_ms - window_ms) : 0U;
    size_t out_count = 0;
    bool has_value = false;

    if (out_minmax) {
        out_minmax->co2_min = 0.0f;
        out_minmax->co2_max = 0.0f;
        out_minmax->temperature_min = 0.0f;
        out_minmax->temperature_max = 0.0f;
        out_minmax->humidity_min = 0.0f;
        out_minmax->humidity_max = 0.0f;
    }

    if (s_history_count == 0) {
        return 0;
    }

    size_t start = (s_history_head + SCD30_HISTORY_MAX_SAMPLES - s_history_count) %
                   SCD30_HISTORY_MAX_SAMPLES;

    for (size_t i = 0; i < s_history_count; i++) {
        size_t idx = (start + i) % SCD30_HISTORY_MAX_SAMPLES;
        scd30_history_entry_t *entry = &s_history[idx];
        if (entry->timestamp_ms < cutoff) {
            continue;
        }

        if (out && out_count < max) {
            out[out_count].co2_ppm = entry->co2_ppm;
            out[out_count].temperature_c = entry->temperature_c;
            out[out_count].humidity_rh = entry->humidity_rh;
            out[out_count].age_ms = now_ms - entry->timestamp_ms;
            out_count++;
        }

        if (!has_value && out_minmax) {
            out_minmax->co2_min = entry->co2_ppm;
            out_minmax->co2_max = entry->co2_ppm;
            out_minmax->temperature_min = entry->temperature_c;
            out_minmax->temperature_max = entry->temperature_c;
            out_minmax->humidity_min = entry->humidity_rh;
            out_minmax->humidity_max = entry->humidity_rh;
            has_value = true;
        } else if (has_value && out_minmax) {
            if (entry->co2_ppm < out_minmax->co2_min) out_minmax->co2_min = entry->co2_ppm;
            if (entry->co2_ppm > out_minmax->co2_max) out_minmax->co2_max = entry->co2_ppm;
            if (entry->temperature_c < out_minmax->temperature_min) out_minmax->temperature_min = entry->temperature_c;
            if (entry->temperature_c > out_minmax->temperature_max) out_minmax->temperature_max = entry->temperature_c;
            if (entry->humidity_rh < out_minmax->humidity_min) out_minmax->humidity_min = entry->humidity_rh;
            if (entry->humidity_rh > out_minmax->humidity_max) out_minmax->humidity_max = entry->humidity_rh;
        }
    }

    return out_count;
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

    int cx = (PANEL_WIDTH - 1) / 2;
    int cy = (PANEL_HEIGHT - 1) / 2;
    int dx = x - cx;
    int dy = y - cy;
    if ((dx * dx + dy * dy) > (PANEL_RADIUS * PANEL_RADIUS)) {
        return;
    }

    int rx = GRAPH_MIRROR_X ? (PANEL_WIDTH - 1) - x : x;
    int ry = GRAPH_MIRROR_Y ? (PANEL_HEIGHT - 1) - y : y;
    int sp6_size = (PANEL_WIDTH * PANEL_HEIGHT) / 2;
    int out_index = (ry * PANEL_WIDTH + rx) / 2;

    if ((rx & 1) == 0) {
        if (out_index >= 0 && out_index < sp6_size) {
            s_sp6[out_index] = (s_sp6[out_index] & 0x0F) | (uint8_t)(color << 4);
        }
    } else {
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

static void draw_line(int x0, int y0, int x1, int y1, uint8_t color)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_line_thick(int x0, int y0, int x1, int y1, uint8_t color, int thickness)
{
    int half = thickness / 2;
    for (int ox = -half; ox <= half; ox++) {
        for (int oy = -half; oy <= half; oy++) {
            draw_line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
        }
    }
}

static float clampf(float value, float min_val, float max_val)
{
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

static float map_value(float value, float min_val, float max_val)
{
    if (max_val - min_val < 0.001f) {
        return 0.0f;
    }
    return (value - min_val) / (max_val - min_val);
}

static void draw_arc(int cx, int cy, int radius, float start_deg, float end_deg, uint8_t color)
{
    float step = (end_deg > start_deg) ? 2.0f : -2.0f;
    for (float deg = start_deg; (step > 0) ? (deg <= end_deg) : (deg >= end_deg); deg += step) {
        float rad = deg * (float)M_PI / 180.0f;
        int x = cx + (int)roundf(cosf(rad) * radius);
        int y = cy + (int)roundf(sinf(rad) * radius);
        set_pixel(x, y, color);
    }
}

static void draw_arc_thick(int cx, int cy, int radius, float start_deg, float end_deg,
                           uint8_t color, int thickness)
{
    int half = thickness / 2;
    for (int r = radius - half; r <= radius + half; r++) {
        draw_arc(cx, cy, r, start_deg, end_deg, color);
    }
}

static void draw_tick(int cx, int cy, int radius, float angle_deg, uint8_t color, int thickness)
{
    float rad = angle_deg * (float)M_PI / 180.0f;
    int x0 = cx + (int)roundf(cosf(rad) * (radius - 10));
    int y0 = cy + (int)roundf(sinf(rad) * (radius - 10));
    int x1 = cx + (int)roundf(cosf(rad) * (radius + 2));
    int y1 = cy + (int)roundf(sinf(rad) * (radius + 2));
    draw_line_thick(x0, y0, x1, y1, color, thickness);
}

static void render_graph(const scd30_history_point_t *points, size_t count,
                         const scd30_minmax_t *minmax)
{
    if (!alloc_buffers() || !points || count == 0 || !minmax) {
        return;
    }

    size_t sp6_size = (PANEL_WIDTH * PANEL_HEIGHT) / 2U;
    memset(s_sp6, 0x11, sp6_size);

    const scd30_history_point_t *latest = &points[count - 1];
    char header[32];
    snprintf(header, sizeof(header), "SCD30 %us", (unsigned)SCD30_HISTORY_WINDOW_SEC);
    draw_text(18, 10, header, COLOR_BLACK, 2);

    int cx = 200;
    int cy = 200;
    int radius = 150;
    float start_deg = 210.0f;
    float end_deg = -30.0f;
    int axis_thick = 2;
    int series_thick = 2;
    int arc_thick = 3;

    draw_arc_thick(cx, cy, radius, start_deg, end_deg, COLOR_BLUE, arc_thick);

    float co2_min = 400.0f;
    float co2_max = 2000.0f;
    float current = clampf(latest->co2_ppm, co2_min, co2_max);
    float min_val = clampf(minmax->co2_min, co2_min, co2_max);
    float max_val = clampf(minmax->co2_max, co2_min, co2_max);

    float current_t = map_value(current, co2_min, co2_max);
    float min_t = map_value(min_val, co2_min, co2_max);
    float max_t = map_value(max_val, co2_min, co2_max);

    float span = end_deg - start_deg;
    draw_tick(cx, cy, radius, start_deg + span * min_t, COLOR_GREEN, axis_thick);
    draw_tick(cx, cy, radius, start_deg + span * max_t, COLOR_RED, axis_thick);
    draw_tick(cx, cy, radius, start_deg + span * current_t, COLOR_BLACK, axis_thick);

    char co2_line[32];
    snprintf(co2_line, sizeof(co2_line), "CO2 %4d", (int)(latest->co2_ppm + 0.5f));
    draw_text(120, 60, co2_line, COLOR_BLACK, 2);

    char minmax_line[32];
    snprintf(minmax_line, sizeof(minmax_line), "min %4d max %4d",
             (int)(minmax->co2_min + 0.5f), (int)(minmax->co2_max + 0.5f));
    draw_text(90, 90, minmax_line, COLOR_BLACK, 1);

    int plot_x = 70;
    int plot_y = 200;
    int plot_w = 260;
    int plot_h = 150;

    draw_line_thick(plot_x, plot_y, plot_x + plot_w, plot_y, COLOR_BLACK, axis_thick);
    draw_line_thick(plot_x, plot_y, plot_x, plot_y + plot_h, COLOR_BLACK, axis_thick);

    float tmin = minmax->temperature_min;
    float tmax = minmax->temperature_max;
    float hmin = minmax->humidity_min;
    float hmax = minmax->humidity_max;

    for (size_t i = 1; i < count; i++) {
        float t0 = (float)points[i - 1].age_ms / (float)(SCD30_HISTORY_WINDOW_SEC * 1000U);
        float t1 = (float)points[i].age_ms / (float)(SCD30_HISTORY_WINDOW_SEC * 1000U);
        int x0 = plot_x + (int)roundf((1.0f - t0) * plot_w);
        int x1 = plot_x + (int)roundf((1.0f - t1) * plot_w);

        float c0 = map_value(points[i - 1].co2_ppm, minmax->co2_min, minmax->co2_max);
        float c1 = map_value(points[i].co2_ppm, minmax->co2_min, minmax->co2_max);
        int y0 = plot_y + plot_h - (int)roundf(c0 * plot_h);
        int y1 = plot_y + plot_h - (int)roundf(c1 * plot_h);
        draw_line_thick(x0, y0, x1, y1, COLOR_RED, series_thick);

        float tt0 = map_value(points[i - 1].temperature_c, tmin, tmax);
        float tt1 = map_value(points[i].temperature_c, tmin, tmax);
        y0 = plot_y + plot_h - (int)roundf(tt0 * plot_h);
        y1 = plot_y + plot_h - (int)roundf(tt1 * plot_h);
        draw_line_thick(x0, y0, x1, y1, COLOR_BLUE, series_thick);

        float rh0 = map_value(points[i - 1].humidity_rh, hmin, hmax);
        float rh1 = map_value(points[i].humidity_rh, hmin, hmax);
        y0 = plot_y + plot_h - (int)roundf(rh0 * plot_h);
        y1 = plot_y + plot_h - (int)roundf(rh1 * plot_h);
        draw_line_thick(x0, y0, x1, y1, COLOR_GREEN, series_thick);
    }

    draw_text(plot_x, plot_y + plot_h + 8, "CO2", COLOR_RED, 1);
    draw_text(plot_x + 60, plot_y + plot_h + 8, "T", COLOR_BLUE, 1);
    draw_text(plot_x + 90, plot_y + plot_h + 8, "RH", COLOR_GREEN, 1);

    epd_show_image(s_sp6, sp6_size);
}

bool scd30_display_begin(uint32_t timeout_ms)
{
    if (!power_domain_take(timeout_ms)) {
        return false;
    }
    power_domain_set(false);
    if (SCD30_PRE_EPD_OFF_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(SCD30_PRE_EPD_OFF_MS));
    }
    return true;
}

void scd30_display_end(void)
{
    if (SCD30_POST_EPD_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(SCD30_POST_EPD_DELAY_MS));
    }
    if (SCD30_RESTORE_POWER_AFTER_EPD) {
        power_domain_set(true);
    }
    power_domain_give();
}

static void scd30_task(void *arg) {
    (void)arg;
    sensirion_i2c_hal_init();

    for (;;) {
        uint32_t loop_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        float co2 = 0.0f;
        float temperature = 0.0f;
        float humidity = 0.0f;

        int16_t err = NO_ERROR;
        if (!scd30_sensor_begin(30000)) {
            ESP_LOGW(TAG, "Power domain busy, skipping SCD30 read");
            vTaskDelay(pdMS_TO_TICKS(SCD30_READ_INTERVAL_SEC * 1000U));
            continue;
        }

        scd30_init(SCD30_I2C_ADDR);
        scd30_stop_periodic_measurement();
        scd30_soft_reset();
        sensirion_i2c_hal_sleep_usec(2000000);

        err = scd30_set_measurement_interval(SCD30_MEASUREMENT_INTERVAL_SEC);
        if (err == NO_ERROR) {
            err = scd30_start_periodic_measurement(0);
        }
        if (err == NO_ERROR) {
            err = scd30_blocking_read_measurement_data(&co2, &temperature, &humidity);
        }
        scd30_stop_periodic_measurement();
        scd30_sensor_end();

        if (err == NO_ERROR) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            bool should_render = false;
            portENTER_CRITICAL(&s_data_lock);
            s_latest.co2_ppm = co2;
            s_latest.temperature_c = temperature;
            s_latest.humidity_rh = humidity;
            s_latest.age_ms = now_ms;
            s_latest.valid = true;
            history_add(co2, temperature, humidity, now_ms);
            if (s_auto_render_enabled) {
                if (s_last_render_ms == 0 || (now_ms - s_last_render_ms) >= s_auto_render_interval_ms) {
                    should_render = true;
                }
            }
            portEXIT_CRITICAL(&s_data_lock);

            ESP_LOGI(TAG, "CO2 %.2f ppm, T %.2f C, RH %.2f %%", co2, temperature, humidity);
            history_maybe_save_to_nvs();
            if (should_render) {
                scd30_render_graph_now();
            }
        } else {
            ESP_LOGW(TAG, "SCD30 read error: %d", err);
        }

        uint32_t loop_end_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        uint32_t period_ms = SCD30_READ_INTERVAL_SEC * 1000U;
        uint32_t elapsed_ms = loop_end_ms - loop_start_ms;
        if (elapsed_ms < period_ms) {
            vTaskDelay(pdMS_TO_TICKS(period_ms - elapsed_ms));
        }
    }
}

static void scd30_uart_task(void *arg)
{
    (void)arg;
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_param_config(UART_NUM_0, &cfg);
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    const char *prompt = "Type 'graph' to render\r\n";
    uart_write_bytes(UART_NUM_0, prompt, strlen(prompt));

    char line[64];
    size_t len = 0;

    for (;;) {
        uint8_t ch;
        int n = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (n > 0) {
            if (ch == '\n' || ch == '\r') {
                line[len] = '\0';
                if (len > 0) {
                    if (strstr(line, "graph") || strstr(line, "render")) {
                        scd30_render_graph_now();
                    }
                    len = 0;
                }
            } else if (len + 1 < sizeof(line)) {
                line[len++] = (char)ch;
            }
        }
    }
}

void scd30_app_start(void) {
    history_restore_from_nvs();
    xTaskCreate(scd30_task, "scd30_task", 4096, NULL, 5, NULL);
    xTaskCreate(scd30_uart_task, "scd30_uart", 2048, NULL, 4, NULL);
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

size_t scd30_get_history(scd30_history_point_t *out, size_t max, scd30_minmax_t *out_minmax)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    size_t count;

    portENTER_CRITICAL(&s_data_lock);
    count = copy_history(now_ms, out, max, out_minmax);
    portEXIT_CRITICAL(&s_data_lock);

    return count;
}

void scd30_render_graph_now(void)
{
    scd30_history_point_t points[SCD30_HISTORY_MAX_SAMPLES];
    scd30_minmax_t minmax;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    size_t count;
    
    if (!scd30_display_begin(60000)) {
        ESP_LOGW(TAG, "Power domain busy, skipping display render");
        return;
    }

    portENTER_CRITICAL(&s_data_lock);
    count = copy_history(now_ms, points, SCD30_HISTORY_MAX_SAMPLES, &minmax);
    portEXIT_CRITICAL(&s_data_lock);

    if (count == 0) {
        ESP_LOGW(TAG, "No SCD30 history to render");
        scd30_display_end();
        return;
    }

    render_graph(points, count, &minmax);
    portENTER_CRITICAL(&s_data_lock);
    s_last_render_ms = now_ms;
    portEXIT_CRITICAL(&s_data_lock);
    scd30_display_end();
}

void scd30_set_auto_render(bool enabled, uint32_t interval_sec)
{
    if (interval_sec < 60U) {
        interval_sec = 60U;
    }
    if (interval_sec > 86400U) {
        interval_sec = 86400U;
    }

    portENTER_CRITICAL(&s_data_lock);
    s_auto_render_enabled = enabled;
    if (enabled) {
        s_auto_render_interval_ms = interval_sec * 1000U;
    }
    if (!enabled) {
        s_last_render_ms = 0;
    }
    portEXIT_CRITICAL(&s_data_lock);
}
