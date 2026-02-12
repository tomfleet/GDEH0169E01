#ifndef IMAGE_UPLOAD_H
#define IMAGE_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

typedef void (*image_upload_handler_t)(const uint8_t *data, size_t length);

void image_upload_start(image_upload_handler_t handler, size_t expected_size);

#endif
