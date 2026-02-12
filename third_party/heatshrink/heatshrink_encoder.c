#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "heatshrink_encoder.h"

typedef enum {
    HSES_NOT_FULL,
    HSES_FILLED,
    HSES_SEARCH,
    HSES_YIELD_TAG_BIT,
    HSES_YIELD_LITERAL,
    HSES_YIELD_BR_INDEX,
    HSES_YIELD_BR_LENGTH,
    HSES_SAVE_BACKLOG,
    HSES_FLUSH_BITS,
    HSES_DONE,
} HSE_state;

#if HEATSHRINK_DEBUGGING_LOGS
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define ASSERT(X) assert(X)
static const char *state_names[] = {
    "not_full",
    "filled",
    "search",
    "yield_tag_bit",
    "yield_literal",
    "yield_br_index",
    "yield_br_length",
    "save_backlog",
    "flush_bits",
    "done",
};
#else
#define LOG(...) /* no-op */
#define ASSERT(X) /* no-op */
#endif

enum {
    FLAG_IS_FINISHING = 0x01,
};

typedef struct {
    uint8_t *buf;
    size_t buf_size;
    size_t *output_size;
} output_info;

#define MATCH_NOT_FOUND ((uint16_t)-1)

static uint16_t get_input_offset(heatshrink_encoder *hse);
static uint16_t get_input_buffer_size(heatshrink_encoder *hse);
static uint16_t get_lookahead_size(heatshrink_encoder *hse);
static void add_tag_bit(heatshrink_encoder *hse, output_info *oi, uint8_t tag);
static int can_take_byte(output_info *oi);
static int is_finishing(heatshrink_encoder *hse);
static void save_backlog(heatshrink_encoder *hse);
static void push_bits(heatshrink_encoder *hse, uint8_t count, uint8_t bits,
    output_info *oi);
static uint8_t push_outgoing_bits(heatshrink_encoder *hse, output_info *oi);
static void push_literal_byte(heatshrink_encoder *hse, output_info *oi);

#if HEATSHRINK_DYNAMIC_ALLOC
heatshrink_encoder *heatshrink_encoder_alloc(uint8_t window_sz2,
        uint8_t lookahead_sz2) {
    if ((window_sz2 < HEATSHRINK_MIN_WINDOW_BITS) ||
        (window_sz2 > HEATSHRINK_MAX_WINDOW_BITS) ||
        (lookahead_sz2 < HEATSHRINK_MIN_LOOKAHEAD_BITS) ||
        (lookahead_sz2 >= window_sz2)) {
        return NULL;
    }

    size_t buf_sz = (2 << window_sz2);
    heatshrink_encoder *hse = HEATSHRINK_MALLOC(sizeof(*hse) + buf_sz);
    if (hse == NULL) { return NULL; }
    hse->window_sz2 = window_sz2;
    hse->lookahead_sz2 = lookahead_sz2;
    heatshrink_encoder_reset(hse);

#if HEATSHRINK_USE_INDEX
    size_t index_sz = buf_sz * sizeof(uint16_t);
    hse->search_index = HEATSHRINK_MALLOC(index_sz + sizeof(struct hs_index));
    if (hse->search_index == NULL) {
        HEATSHRINK_FREE(hse, sizeof(*hse) + buf_sz);
        return NULL;
    }
    hse->search_index->size = index_sz;
#endif

    return hse;
}

void heatshrink_encoder_free(heatshrink_encoder *hse) {
    size_t buf_sz = (2 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
#if HEATSHRINK_USE_INDEX
    size_t index_sz = sizeof(struct hs_index) + hse->search_index->size;
    HEATSHRINK_FREE(hse->search_index, index_sz);
    (void)index_sz;
#endif
    HEATSHRINK_FREE(hse, sizeof(heatshrink_encoder) + buf_sz);
    (void)buf_sz;
}
#endif

void heatshrink_encoder_reset(heatshrink_encoder *hse) {
    size_t buf_sz = (2 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
    memset(hse->buffer, 0, buf_sz);
    hse->input_size = 0;
    hse->state = HSES_NOT_FULL;
    hse->match_scan_index = 0;
    hse->flags = 0;
    hse->bit_index = 0x80;
    hse->current_byte = 0x00;
    hse->match_length = 0;
    hse->outgoing_bits = 0x0000;
    hse->outgoing_bits_count = 0;
}

HSE_sink_res heatshrink_encoder_sink(heatshrink_encoder *hse,
        uint8_t *in_buf, size_t size, size_t *input_size) {
    if ((hse == NULL) || (in_buf == NULL) || (input_size == NULL)) {
        return HSER_SINK_ERROR_NULL;
    }

    if (is_finishing(hse)) { return HSER_SINK_ERROR_MISUSE; }
    if (hse->state != HSES_NOT_FULL) { return HSER_SINK_ERROR_MISUSE; }

    uint16_t write_offset = get_input_offset(hse) + hse->input_size;
    uint16_t ibs = get_input_buffer_size(hse);
    uint16_t rem = ibs - hse->input_size;
    uint16_t cp_sz = rem < size ? rem : size;

    memcpy(&hse->buffer[write_offset], in_buf, cp_sz);
    *input_size = cp_sz;
    hse->input_size += cp_sz;

    if (cp_sz == rem) {
        hse->state = HSES_FILLED;
    }

    return HSER_SINK_OK;
}

static uint16_t find_longest_match(heatshrink_encoder *hse, uint16_t start,
    uint16_t end, const uint16_t maxlen, uint16_t *match_length);
static void do_indexing(heatshrink_encoder *hse);

static HSE_state st_step_search(heatshrink_encoder *hse);
static HSE_state st_yield_tag_bit(heatshrink_encoder *hse, output_info *oi);
static HSE_state st_yield_literal(heatshrink_encoder *hse, output_info *oi);
static HSE_state st_yield_br_index(heatshrink_encoder *hse, output_info *oi);
static HSE_state st_yield_br_length(heatshrink_encoder *hse, output_info *oi);
static HSE_state st_save_backlog(heatshrink_encoder *hse);
static HSE_state st_flush_bit_buffer(heatshrink_encoder *hse, output_info *oi);

HSE_poll_res heatshrink_encoder_poll(heatshrink_encoder *hse,
        uint8_t *out_buf, size_t out_buf_size, size_t *output_size) {
    if ((hse == NULL) || (out_buf == NULL) || (output_size == NULL)) {
        return HSER_POLL_ERROR_NULL;
    }
    if (out_buf_size == 0) {
        return HSER_POLL_ERROR_MISUSE;
    }
    *output_size = 0;

    output_info oi;
    oi.buf = out_buf;
    oi.buf_size = out_buf_size;
    oi.output_size = output_size;

    while (1) {
        uint8_t in_state = hse->state;
        switch (in_state) {
        case HSES_NOT_FULL:
            return HSER_POLL_EMPTY;
        case HSES_FILLED:
            do_indexing(hse);
            hse->state = HSES_SEARCH;
            break;
        case HSES_SEARCH:
            hse->state = st_step_search(hse);
            break;
        case HSES_YIELD_TAG_BIT:
            hse->state = st_yield_tag_bit(hse, &oi);
            break;
        case HSES_YIELD_LITERAL:
            hse->state = st_yield_literal(hse, &oi);
            break;
        case HSES_YIELD_BR_INDEX:
            hse->state = st_yield_br_index(hse, &oi);
            break;
        case HSES_YIELD_BR_LENGTH:
            hse->state = st_yield_br_length(hse, &oi);
            break;
        case HSES_SAVE_BACKLOG:
            hse->state = st_save_backlog(hse);
            break;
        case HSES_FLUSH_BITS:
            hse->state = st_flush_bit_buffer(hse, &oi);
            break;
        case HSES_DONE:
            return HSER_POLL_EMPTY;
        default:
            return HSER_POLL_ERROR_MISUSE;
        }

        if (hse->state == in_state) {
            if (*output_size == out_buf_size) return HSER_POLL_MORE;
        }
    }
}

HSE_finish_res heatshrink_encoder_finish(heatshrink_encoder *hse) {
    if (hse == NULL) { return HSER_FINISH_ERROR_NULL; }
    hse->flags |= FLAG_IS_FINISHING;
    if (hse->state == HSES_NOT_FULL) { hse->state = HSES_FILLED; }
    return hse->state == HSES_DONE ? HSER_FINISH_DONE : HSER_FINISH_MORE;
}

static HSE_state st_step_search(heatshrink_encoder *hse) {
    uint16_t window_length = get_input_buffer_size(hse);
    uint16_t lookahead_sz = get_lookahead_size(hse);
    uint16_t msi = hse->match_scan_index;
    bool fin = is_finishing(hse);
    if (msi > hse->input_size - (fin ? 1 : lookahead_sz)) {
        return fin ? HSES_FLUSH_BITS : HSES_SAVE_BACKLOG;
    }

    uint16_t input_offset = get_input_offset(hse);
    uint16_t end = input_offset + msi;
    uint16_t start = end - window_length;

    uint16_t max_possible = lookahead_sz;
    if (hse->input_size - msi < lookahead_sz) {
        max_possible = hse->input_size - msi;
    }

    uint16_t match_length = 0;
    uint16_t match_pos = find_longest_match(hse, start, end, max_possible,
        &match_length);

    if (match_pos == MATCH_NOT_FOUND) {
        hse->match_scan_index++;
        hse->match_length = 0;
        return HSES_YIELD_TAG_BIT;
    } else {
        hse->match_pos = match_pos;
        hse->match_length = match_length;
        return HSES_YIELD_TAG_BIT;
    }
}

static HSE_state st_yield_tag_bit(heatshrink_encoder *hse, output_info *oi) {
    if (can_take_byte(oi)) {
        if (hse->match_length == 0) {
            add_tag_bit(hse, oi, HEATSHRINK_LITERAL_MARKER);
            return HSES_YIELD_LITERAL;
        } else {
            add_tag_bit(hse, oi, HEATSHRINK_BACKREF_MARKER);
            hse->outgoing_bits = hse->match_pos - 1;
            hse->outgoing_bits_count = HEATSHRINK_ENCODER_WINDOW_BITS(hse);
            return HSES_YIELD_BR_INDEX;
        }
    } else {
        return HSES_YIELD_TAG_BIT;
    }
}

static HSE_state st_yield_literal(heatshrink_encoder *hse, output_info *oi) {
    if (can_take_byte(oi)) {
        push_literal_byte(hse, oi);
        return HSES_SEARCH;
    } else {
        return HSES_YIELD_LITERAL;
    }
}

static HSE_state st_yield_br_index(heatshrink_encoder *hse, output_info *oi) {
    if (can_take_byte(oi)) {
        if (push_outgoing_bits(hse, oi) > 0) {
            return HSES_YIELD_BR_INDEX;
        } else {
            hse->outgoing_bits = hse->match_length - 1;
            hse->outgoing_bits_count = HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse);
            return HSES_YIELD_BR_LENGTH;
        }
    } else {
        return HSES_YIELD_BR_INDEX;
    }
}

static HSE_state st_yield_br_length(heatshrink_encoder *hse, output_info *oi) {
    if (can_take_byte(oi)) {
        if (push_outgoing_bits(hse, oi) > 0) {
            return HSES_YIELD_BR_LENGTH;
        } else {
            hse->match_scan_index += hse->match_length;
            hse->match_length = 0;
            return HSES_SEARCH;
        }
    } else {
        return HSES_YIELD_BR_LENGTH;
    }
}

static HSE_state st_save_backlog(heatshrink_encoder *hse) {
    save_backlog(hse);
    return HSES_NOT_FULL;
}

static HSE_state st_flush_bit_buffer(heatshrink_encoder *hse, output_info *oi) {
    if (hse->bit_index == 0x80) {
        return HSES_DONE;
    } else if (can_take_byte(oi)) {
        oi->buf[(*oi->output_size)++] = hse->current_byte;
        return HSES_DONE;
    } else {
        return HSES_FLUSH_BITS;
    }
}

static void add_tag_bit(heatshrink_encoder *hse, output_info *oi, uint8_t tag) {
    push_bits(hse, 1, tag, oi);
}

static uint16_t get_input_offset(heatshrink_encoder *hse) {
    return get_input_buffer_size(hse);
}

static uint16_t get_input_buffer_size(heatshrink_encoder *hse) {
    return (1 << HEATSHRINK_ENCODER_WINDOW_BITS(hse));
    (void)hse;
}

static uint16_t get_lookahead_size(heatshrink_encoder *hse) {
    return (1 << HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse));
    (void)hse;
}

static void do_indexing(heatshrink_encoder *hse) {
#if HEATSHRINK_USE_INDEX
    struct hs_index *hsi = HEATSHRINK_ENCODER_INDEX(hse);
    int16_t last[256];
    memset(last, 0xFF, sizeof(last));

    uint8_t * const data = hse->buffer;
    int16_t * const index = hsi->index;

    const uint16_t input_offset = get_input_offset(hse);
    const uint16_t end = input_offset + hse->input_size;

    for (uint16_t i = 0; i < end; i++) {
        uint8_t v = data[i];
        int16_t lv = last[v];
        index[i] = lv;
        last[v] = i;
    }
#else
    (void)hse;
#endif
}

static int is_finishing(heatshrink_encoder *hse) {
    return hse->flags & FLAG_IS_FINISHING;
}

static int can_take_byte(output_info *oi) {
    return *oi->output_size < oi->buf_size;
}

static uint16_t find_longest_match(heatshrink_encoder *hse, uint16_t start,
        uint16_t end, const uint16_t maxlen, uint16_t *match_length) {
    uint8_t *buf = hse->buffer;

    uint16_t match_maxlen = 0;
    uint16_t match_index = MATCH_NOT_FOUND;

    uint16_t len = 0;
    uint8_t * const needlepoint = &buf[end];

    for (int16_t pos = end - 1; pos - (int16_t)start >= 0; pos--) {
        uint8_t * const pospoint = &buf[pos];
        if ((pospoint[match_maxlen] == needlepoint[match_maxlen])
            && (*pospoint == *needlepoint)) {
            for (len = 1; len < maxlen; len++) {
                if (pospoint[len] != needlepoint[len]) { break; }
            }
            if (len > match_maxlen) {
                match_maxlen = len;
                match_index = pos;
                if (len == maxlen) { break; }
            }
        }
    }

    const size_t break_even_point =
      (1 + HEATSHRINK_ENCODER_WINDOW_BITS(hse) +
         HEATSHRINK_ENCODER_LOOKAHEAD_BITS(hse));

    if (match_maxlen > (break_even_point / 8)) {
        *match_length = match_maxlen;
        return end - match_index;
    }

    return MATCH_NOT_FOUND;
}

static uint8_t push_outgoing_bits(heatshrink_encoder *hse, output_info *oi) {
    uint8_t count = 0;
    uint8_t bits = 0;
    if (hse->outgoing_bits_count > 8) {
        count = 8;
        bits = hse->outgoing_bits >> (hse->outgoing_bits_count - 8);
    } else {
        count = hse->outgoing_bits_count;
        bits = hse->outgoing_bits;
    }

    if (count > 0) {
        push_bits(hse, count, bits, oi);
        hse->outgoing_bits_count -= count;
    }
    return count;
}

static void push_bits(heatshrink_encoder *hse, uint8_t count, uint8_t bits,
        output_info *oi) {
    ASSERT(count <= 8);

    if (count == 8 && hse->bit_index == 0x80) {
        oi->buf[(*oi->output_size)++] = bits;
    } else {
        for (int i = count - 1; i >= 0; i--) {
            bool bit = bits & (1 << i);
            if (bit) { hse->current_byte |= hse->bit_index; }
            hse->bit_index >>= 1;
            if (hse->bit_index == 0x00) {
                hse->bit_index = 0x80;
                oi->buf[(*oi->output_size)++] = hse->current_byte;
                hse->current_byte = 0x00;
            }
        }
    }
}

static void push_literal_byte(heatshrink_encoder *hse, output_info *oi) {
    uint16_t processed_offset = hse->match_scan_index - 1;
    uint16_t input_offset = get_input_offset(hse) + processed_offset;
    uint8_t c = hse->buffer[input_offset];
    push_bits(hse, 8, c, oi);
}

static void save_backlog(heatshrink_encoder *hse) {
    size_t input_buf_sz = get_input_buffer_size(hse);
    uint16_t msi = hse->match_scan_index;
    uint16_t rem = input_buf_sz - msi;
    uint16_t shift_sz = input_buf_sz + rem;

    memmove(&hse->buffer[0], &hse->buffer[input_buf_sz - rem], shift_sz);
    hse->match_scan_index = 0;
    hse->input_size -= input_buf_sz - rem;
}
