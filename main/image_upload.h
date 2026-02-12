#ifndef IMAGE_UPLOAD_H
#define IMAGE_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

typedef void (*image_upload_handler_t)(const uint8_t *data, size_t length);

typedef enum {
	IMAGE_UPLOAD_STATUS_BOOT = 0,
	IMAGE_UPLOAD_STATUS_CONNECTING,
	IMAGE_UPLOAD_STATUS_CONNECTED,
	IMAGE_UPLOAD_STATUS_IDLE,
	IMAGE_UPLOAD_STATUS_UPLOADING,
	IMAGE_UPLOAD_STATUS_WIFI_FAILED,
} image_upload_status_t;

typedef void (*image_upload_status_cb_t)(image_upload_status_t status, void *ctx);

void image_upload_start(image_upload_handler_t handler, size_t expected_size);
void image_upload_set_status_callback(image_upload_status_cb_t cb, void *ctx);

#endif
