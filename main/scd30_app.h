#ifndef SCD30_APP_H
#define SCD30_APP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_rh;
    uint32_t age_ms;
    bool valid;
} scd30_reading_t;

void scd30_app_start(void);
bool scd30_get_latest(scd30_reading_t *out);

#endif
