#include <stdlib.h>
#include <string.h>
#include "heatshrink_decoder.h"

typedef enum {
    HSDS_TAG_BIT,
    HSDS_YIELD_LITERAL,
    HSDS_BACKREF_INDEX_MSB,
    HSDS_BACKREF_INDEX_LSB,
    HSDS_BACKREF_COUNT_MSB,
    HSDS_BACKREF_COUNT_LSB,
    HSDS_YIELD_BACKREF,
} HSD_state;

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "tag_bit",
    "yield_literal",
    "backref_index_msb",
    "backref_index_lsb",
    "backref_count_msb",
    "backref_count_lsb",
    "yield_backref",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

typedef struct {
    uint8_t *buf;
    size_t buf_size;
    size_t *output_size;
} output_info;

#define NO_BITS ((uint16_t)-1)

static uint16_t get_bits(heatshrink_decoder *hsd, uint8_t count);
static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint8_t byte);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_decoder *heatshrink_decoder_alloc(uint16_t input_buffer_size,
                                             uint8_t window_sz2,
                                             uint8_t lookahead_sz2)
{
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (input_buffer_size == 0) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 >= window_sz2)) {
        return NULL;
    }
    size_t buffers_sz = (1 << window_sz2) + input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    heatshrink_decoder *hsd = HEATSHRINK_MALLOC(sz);
    if (hsd == NULL) { return NULL; }
    hsd->input_buffer_size = input_buffer_size;
    hsd->window_sz2 = window_sz2;
    hsd->lookahead_sz2 = lookahead_sz2;
    heatshrink_decoder_reset(hsd);
    LOG("-- allocated decoder with buffer size of %zu (%zu + %u + %u)\n",
        sz, sizeof(heatshrink_decoder), (1 << window_sz2), input_buffer_size);
    return hsd;
}

void heatshrink_decoder_free(heatshrink_decoder *hsd) {
    size_t buffers_sz = (1 << hsd->window_sz2) + hsd->input_buffer_size;
    size_t sz = sizeof(heatshrink_decoder) + buffers_sz;
    HEATSHRINK_FREE(hsd, sz);
    (void)sz;
}
#endif

void heatshrink_decoder_reset(heatshrink_decoder *hsd) {
    size_t buf_sz = 1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd);
    size_t input_sz = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd);
    memset(hsd->buffers, 0, buf_sz + input_sz);
    hsd->state = HSDS_TAG_BIT;
    hsd->input_size = 0;
    hsd->input_index = 0;
    hsd->bit_index = 0x00;
    hsd->current_byte = 0x00;
    hsd->output_count = 0;
    hsd->output_index = 0;
    hsd->head_index = 0;
}

HSD_sink_res heatshrink_decoder_sink(heatshrink_decoder *hsd,
        uint8_t *in_buf, size_t size, size_t *input_size) {
    if ((hsd == NULL) || (in_buf == NULL) || (input_size == NULL)) {
        return HSDR_SINK_ERROR_NULL;
    }

    size_t rem = HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd) - hsd->input_size;
    if (rem == 0) {
        *input_size = 0;
        return HSDR_SINK_FULL;
    }

    size = rem < size ? rem : size;
    LOG("-- sinking %zd bytes\n", size);
    memcpy(&hsd->buffers[hsd->input_size], in_buf, size);
    hsd->input_size += size;
    *input_size = size;
    return HSDR_SINK_OK;
}

#define BACKREF_COUNT_BITS(HSD) (HEATSHRINK_DECODER_LOOKAHEAD_BITS(HSD))
#define BACKREF_INDEX_BITS(HSD) (HEATSHRINK_DECODER_WINDOW_BITS(HSD))

static HSD_state st_tag_bit(heatshrink_decoder *hsd);
static HSD_state st_yield_literal(heatshrink_decoder *hsd, output_info *oi);
static HSD_state st_backref_index_msb(heatshrink_decoder *hsd);
static HSD_state st_backref_index_lsb(heatshrink_decoder *hsd);
static HSD_state st_backref_count_msb(heatshrink_decoder *hsd);
static HSD_state st_backref_count_lsb(heatshrink_decoder *hsd);
static HSD_state st_yield_backref(heatshrink_decoder *hsd, output_info *oi);

HSD_poll_res heatshrink_decoder_poll(heatshrink_decoder *hsd,
        uint8_t *out_buf, size_t out_buf_size, size_t *output_size) {
    if ((hsd == NULL) || (out_buf == NULL) || (output_size == NULL)) {
        return HSDR_POLL_ERROR_NULL;
    }
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        LOG("-- poll, state is %d (%s), input_size %d\n",
            hsd->state, state_names[hsd->state], hsd->input_size);
        uint8_t in_state = hsd->state;
        switch (in_state) {
        case HSDS_TAG_BIT:
            hsd->state = st_tag_bit(hsd);
            break;
        case HSDS_YIELD_LITERAL:
            hsd->state = st_yield_literal(hsd, &oi);
            break;
        case HSDS_BACKREF_INDEX_MSB:
            hsd->state = st_backref_index_msb(hsd);
            break;
        case HSDS_BACKREF_INDEX_LSB:
            hsd->state = st_backref_index_lsb(hsd);
            break;
        case HSDS_BACKREF_COUNT_MSB:
            hsd->state = st_backref_count_msb(hsd);
            break;
        case HSDS_BACKREF_COUNT_LSB:
            hsd->state = st_backref_count_lsb(hsd);
            break;
        case HSDS_YIELD_BACKREF:
            hsd->state = st_yield_backref(hsd, &oi);
            break;
        default:
            return HSDR_POLL_ERROR_UNKNOWN;
        }

        if (hsd->state == in_state) {
            if (*output_size == out_buf_size) { return HSDR_POLL_MORE; }
            return HSDR_POLL_EMPTY;
        }
    }
}

static HSD_state st_tag_bit(heatshrink_decoder *hsd) {
    uint32_t bits = get_bits(hsd, 1);
    if (bits == NO_BITS) {
        return HSDS_TAG_BIT;
    } else if (bits) {
        return HSDS_YIELD_LITERAL;
    } else if (HEATSHRINK_DECODER_WINDOW_BITS(hsd) > 8) {
        return HSDS_BACKREF_INDEX_MSB;
    } else {
        hsd->output_index = 0;
        return HSDS_BACKREF_INDEX_LSB;
    }
}

static HSD_state st_yield_literal(heatshrink_decoder *hsd, output_info *oi) {
    if (*oi->output_size < oi->buf_size) {
        uint16_t byte = get_bits(hsd, 8);
        if (byte == NO_BITS) { return HSDS_YIELD_LITERAL; }
        uint8_t *buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        uint16_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd)) - 1;
        uint8_t c = byte & 0xFF;
        buf[hsd->head_index++ & mask] = c;
        push_byte(hsd, oi, c);
        return HSDS_TAG_BIT;
    } else {
        return HSDS_YIELD_LITERAL;
    }
}

static HSD_state st_backref_index_msb(heatshrink_decoder *hsd) {
    uint8_t bit_ct = BACKREF_INDEX_BITS(hsd);
    ASSERT(bit_ct > 8);
    uint16_t bits = get_bits(hsd, bit_ct - 8);
    if (bits == NO_BITS) { return HSDS_BACKREF_INDEX_MSB; }
    hsd->output_index = bits << 8;
    return HSDS_BACKREF_INDEX_LSB;
}

static HSD_state st_backref_index_lsb(heatshrink_decoder *hsd) {
    uint8_t bit_ct = BACKREF_INDEX_BITS(hsd);
    uint16_t bits = get_bits(hsd, bit_ct < 8 ? bit_ct : 8);
    if (bits == NO_BITS) { return HSDS_BACKREF_INDEX_LSB; }
    hsd->output_index |= bits;
    hsd->output_index++;
    uint8_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    hsd->output_count = 0;
    return (br_bit_ct > 8) ? HSDS_BACKREF_COUNT_MSB : HSDS_BACKREF_COUNT_LSB;
}

static HSD_state st_backref_count_msb(heatshrink_decoder *hsd) {
    uint8_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    ASSERT(br_bit_ct > 8);
    uint16_t bits = get_bits(hsd, br_bit_ct - 8);
    if (bits == NO_BITS) { return HSDS_BACKREF_COUNT_MSB; }
    hsd->output_count = bits << 8;
    return HSDS_BACKREF_COUNT_LSB;
}

static HSD_state st_backref_count_lsb(heatshrink_decoder *hsd) {
    uint8_t br_bit_ct = BACKREF_COUNT_BITS(hsd);
    uint16_t bits = get_bits(hsd, br_bit_ct < 8 ? br_bit_ct : 8);
    if (bits == NO_BITS) { return HSDS_BACKREF_COUNT_LSB; }
    hsd->output_count |= bits;
    hsd->output_count++;
    return HSDS_YIELD_BACKREF;
}

static HSD_state st_yield_backref(heatshrink_decoder *hsd, output_info *oi) {
    size_t count = oi->buf_size - *oi->output_size;
    if (count > 0) {
        size_t i = 0;
        if (hsd->output_count < count) count = hsd->output_count;
        uint8_t *buf = &hsd->buffers[HEATSHRINK_DECODER_INPUT_BUFFER_SIZE(hsd)];
        uint16_t mask = (1 << HEATSHRINK_DECODER_WINDOW_BITS(hsd)) - 1;
        uint16_t neg_offset = hsd->output_index;
        ASSERT(neg_offset <= mask + 1);
        ASSERT(count <= (size_t)(1 << BACKREF_COUNT_BITS(hsd)));

        for (i = 0; i < count; i++) {
            uint8_t c = buf[(hsd->head_index - neg_offset) & mask];
            push_byte(hsd, oi, c);
            buf[hsd->head_index & mask] = c;
            hsd->head_index++;
        }
        hsd->output_count -= count;
        if (hsd->output_count == 0) { return HSDS_TAG_BIT; }
    }
    return HSDS_YIELD_BACKREF;
}

static uint16_t get_bits(heatshrink_decoder *hsd, uint8_t count) {
    uint16_t accumulator = 0;
    int i = 0;
    if (count > 15) { return NO_BITS; }

    if (hsd->input_size == 0) {
        if (hsd->bit_index < (1 << (count - 1))) { return NO_BITS; }
    }

    for (i = 0; i < count; i++) {
        if (hsd->bit_index == 0x00) {
            if (hsd->input_size == 0) {
                return NO_BITS;
            }
            hsd->current_byte = hsd->buffers[hsd->input_index++];
            if (hsd->input_index == hsd->input_size) {
                hsd->input_index = 0;
                hsd->input_size = 0;
            }
            hsd->bit_index = 0x80;
        }
        accumulator <<= 1;
        if (hsd->current_byte & hsd->bit_index) {
            accumulator |= 0x01;
        }
        hsd->bit_index >>= 1;
    }

    return accumulator;
}

HSD_finish_res heatshrink_decoder_finish(heatshrink_decoder *hsd) {
    if (hsd == NULL) { return HSDR_FINISH_ERROR_NULL; }
    switch (hsd->state) {
    case HSDS_TAG_BIT:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;
    case HSDS_BACKREF_INDEX_LSB:
    case HSDS_BACKREF_INDEX_MSB:
    case HSDS_BACKREF_COUNT_LSB:
    case HSDS_BACKREF_COUNT_MSB:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;
    case HSDS_YIELD_LITERAL:
        return hsd->input_size == 0 ? HSDR_FINISH_DONE : HSDR_FINISH_MORE;
    default:
        return HSDR_FINISH_MORE;
    }
}

static void push_byte(heatshrink_decoder *hsd, output_info *oi, uint8_t byte) {
    oi->buf[(*oi->output_size)++] = byte;
    (void)hsd;
}
