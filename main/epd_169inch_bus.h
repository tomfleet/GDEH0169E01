#ifndef EPD_169INCH_BUS_H
#define EPD_169INCH_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EPD_MASTER_ONLY = 0,
    EPD_SLAVE_ONLY = 1,
    EPD_MASTER_SLAVE = 2,
} epd_ms_target_t;

void epd_bus_init(void);
void epd_bus_reset(void);
void epd_bus_wait_busy(void);
void epd_bus_delay_ms(uint32_t time_ms);
void epd_bus_delay_s(uint32_t time_s);
void epd_bus_set_master_mode(bool high);

void epd_bus_write_cmd(epd_ms_target_t target, uint8_t cmd);
void epd_bus_write_data(epd_ms_target_t target, uint8_t data);
uint8_t epd_bus_read_data(epd_ms_target_t target);

#endif
