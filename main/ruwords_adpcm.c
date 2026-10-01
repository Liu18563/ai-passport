// main/ruwords_adpcm.c —— 见 ruwords_adpcm.h。
#include "ruwords_adpcm.h"

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767,
};
static const int8_t INDEX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

void ruw_adpcm_init(ruw_adpcm_t *st) {
    st->predictor = 0;
    st->index = 0;
}

static int16_t decode_nibble(ruw_adpcm_t *st, uint8_t code) {
    int32_t step = STEP[st->index];
    int32_t delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    st->predictor += (code & 8) ? -delta : delta;
    if (st->predictor > 32767) st->predictor = 32767;
    if (st->predictor < -32768) st->predictor = -32768;
    int idx = st->index + INDEX[code & 7];
    st->index = (int8_t)(idx < 0 ? 0 : (idx > 88 ? 88 : idx));
    return (int16_t)st->predictor;
}

void ruw_adpcm_decode(ruw_adpcm_t *st, const uint8_t *src, size_t nibbles, int16_t *out) {
    for (size_t i = 0; i < nibbles / 2; i++) {
        out[2 * i] = decode_nibble(st, src[i] & 0x0F);
        out[2 * i + 1] = decode_nibble(st, src[i] >> 4);
    }
}
