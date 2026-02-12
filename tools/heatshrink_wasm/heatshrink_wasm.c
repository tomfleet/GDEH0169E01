#include <stdint.h>
#include <stdlib.h>
#include "heatshrink_encoder.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define HS_KEEP EMSCRIPTEN_KEEPALIVE
#else
#define HS_KEEP
#endif

HS_KEEP void *hs_alloc(uint32_t size) {
    return malloc(size);
}

HS_KEEP void hs_free(void *ptr) {
    free(ptr);
}

HS_KEEP uint32_t hs_encode(const uint8_t *input, uint32_t input_len,
                           uint8_t *output, uint32_t output_cap,
                           uint8_t window_bits, uint8_t lookahead_bits) {
    if (!input || !output || output_cap == 0) {
        return 0;
    }

    heatshrink_encoder *enc = heatshrink_encoder_alloc(window_bits, lookahead_bits);
    if (!enc) {
        return 0;
    }

    uint32_t out_pos = 0;
    uint32_t in_pos = 0;

    while (in_pos < input_len) {
        size_t sunk = 0;
        HSE_sink_res sink_res = heatshrink_encoder_sink(enc,
            (uint8_t *)(input + in_pos), input_len - in_pos, &sunk);
        if (sink_res < 0) {
            heatshrink_encoder_free(enc);
            return 0;
        }
        in_pos += (uint32_t)sunk;

        while (1) {
            size_t polled = 0;
            if (out_pos >= output_cap) {
                heatshrink_encoder_free(enc);
                return 0;
            }
            HSE_poll_res poll_res = heatshrink_encoder_poll(enc, output + out_pos,
                output_cap - out_pos, &polled);
            out_pos += (uint32_t)polled;
            if (poll_res == HSER_POLL_MORE) {
                continue;
            }
            if (poll_res == HSER_POLL_EMPTY) {
                break;
            }
            if (poll_res < 0) {
                heatshrink_encoder_free(enc);
                return 0;
            }
        }
    }

    while (1) {
        HSE_finish_res finish_res = heatshrink_encoder_finish(enc);
        if (finish_res == HSER_FINISH_DONE) {
            break;
        }
        if (finish_res < 0) {
            heatshrink_encoder_free(enc);
            return 0;
        }

        while (1) {
            size_t polled = 0;
            if (out_pos >= output_cap) {
                heatshrink_encoder_free(enc);
                return 0;
            }
            HSE_poll_res poll_res = heatshrink_encoder_poll(enc, output + out_pos,
                output_cap - out_pos, &polled);
            out_pos += (uint32_t)polled;
            if (poll_res == HSER_POLL_MORE) {
                continue;
            }
            if (poll_res == HSER_POLL_EMPTY) {
                break;
            }
            if (poll_res < 0) {
                heatshrink_encoder_free(enc);
                return 0;
            }
        }
    }

    heatshrink_encoder_free(enc);
    return out_pos;
}
