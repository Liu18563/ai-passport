// main/ruwords_adpcm.h —— IMA-ADPCM 4-bit 解码,与 tools/ruwords/gen_assets.py 的编码器配对。
// 每个单词从 predictor=0、index=0 开始;低半字节先解。纯逻辑,可主机测试。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t predictor;
    int8_t index;
} ruw_adpcm_t;

void ruw_adpcm_init(ruw_adpcm_t *st);

// 解码 nibbles 个半字节(从 src 的第 0 个字节低半字节开始,nibbles 必须为偶数,
// 最后一个奇数采样由调用方截掉)。out 至少有 nibbles 个元素。
void ruw_adpcm_decode(ruw_adpcm_t *st, const uint8_t *src, size_t nibbles, int16_t *out);
