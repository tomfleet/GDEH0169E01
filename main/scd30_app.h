#ifndef SCD30_APP_H
#define SCD30_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_rh;
    uint32_t age_ms;
    bool valid;
} scd30_reading_t;

typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_rh;
    uint32_t age_ms;
} scd30_history_point_t;

typedef struct {
    float co2_min;
    float co2_max;
    float temperature_min;
    float temperature_max;
    float humidity_min;
    float humidity_max;
} scd30_minmax_t;

void scd30_app_start(void);
bool scd30_get_latest(scd30_reading_t *out);
size_t scd30_get_history(scd30_history_point_t *out, size_t max, scd30_minmax_t *out_minmax);
void scd30_render_graph_now(void);
void scd30_set_auto_render(bool enabled, uint32_t interval_sec);
bool scd30_display_begin(uint32_t timeout_ms);
void scd30_display_end(void);

#endif
