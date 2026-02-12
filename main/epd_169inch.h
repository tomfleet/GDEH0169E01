#ifndef EPD_169INCH_H
#define EPD_169INCH_H

#include <stddef.h>
#include <stdint.h>

void epd_demo_run(void);
void epd_setup(void);
void epd_show_image(const uint8_t *image_data, size_t length);

#endif
