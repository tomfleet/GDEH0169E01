#ifndef HEATSHRINK_ENCODER_H
#define HEATSHRINK_ENCODER_H

#include <stdint.h>
#include <stddef.h>
#include "heatshrink_common.h"
#include "heatshrink_config.h"

typedef enum {
    HSER_SINK_OK,
    HSER_SINK_ERROR_NULL=-1,
    HSER_SINK_ERROR_MISUSE=-2,
} HSE_sink_res;

typedef enum {
    HSER_POLL_EMPTY,
    HSER_POLL_MORE,
    HSER_POLL_ERROR_NULL=-1,
    HSER_POLL_ERROR_MISUSE=-2,
} HSE_poll_res;

typedef enum {
    HSER_FINISH_DONE,
    HSER_FINISH_MORE,
    HSER_FINISH_ERROR_NULL=-1,
} HSE_finish_res;

#if HEATSHRINK_DYNAMIC_ALLOC
#define HEATSHRINK_ENCODER_WINDOW_BITS(HSE) ((HSE)->window_sz2)
#define HEATSHRINK_ENCODER_LOOKAHEAD_BITS(HSE) ((HSE)->lookahead_sz2)
#define HEATSHRINK_ENCODER_INDEX(HSE) (&(HSE)->search_index)
struct hs_index {
    uint16_t size;
    int16_t index[];
};
#else
#define HEATSHRINK_ENCODER_WINDOW_BITS(_) (HEATSHRINK_STATIC_WINDOW_BITS)
#define HEATSHRINK_ENCODER_LOOKAHEAD_BITS(_) (HEATSHRINK_STATIC_LOOKAHEAD_BITS)
#define HEATSHRINK_ENCODER_INDEX(HSE) (&(HSE)->search_index)
struct hs_index {
    uint16_t size;
    int16_t index[2 << HEATSHRINK_STATIC_WINDOW_BITS];
};
#endif

typedef struct {
    uint16_t input_size;
    uint16_t match_scan_index;
    uint16_t match_length;
    uint16_t match_pos;
    uint16_t outgoing_bits;
    uint8_t outgoing_bits_count;
    uint8_t flags;
    uint8_t state;
    uint8_t current_byte;
    uint8_t bit_index;
#if HEATSHRINK_DYNAMIC_ALLOC
    uint8_t window_sz2;
    uint8_t lookahead_sz2;
#if HEATSHRINK_USE_INDEX
    struct hs_index *search_index;
#endif
    uint8_t buffer[];
#else
#if HEATSHRINK_USE_INDEX
    struct hs_index search_index;
#endif
    uint8_t buffer[2 << HEATSHRINK_ENCODER_WINDOW_BITS(_)];
#endif
} heatshrink_encoder;

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_encoder *heatshrink_encoder_alloc(uint8_t window_sz2,
    uint8_t lookahead_sz2);
void heatshrink_encoder_free(heatshrink_encoder *hse);
#endif

void heatshrink_encoder_reset(heatshrink_encoder *hse);

HSE_sink_res heatshrink_encoder_sink(heatshrink_encoder *hse,
    uint8_t *in_buf, size_t size, size_t *input_size);

HSE_poll_res heatshrink_encoder_poll(heatshrink_encoder *hse,
    uint8_t *out_buf, size_t out_buf_size, size_t *output_size);

HSE_finish_res heatshrink_encoder_finish(heatshrink_encoder *hse);

#endif
