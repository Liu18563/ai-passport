// main/ruwords_data.h —— 俄语词表与发音数据(由 tools/ruwords/gen_assets.py 生成)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ruwords_count.h"

typedef struct {
    const char *ru;          // UTF-8 俄语
    const char *ru_marked;   // 重读元音带 LVGL recolor 标记,需 lv_label_set_recolor
    const char *translit;    // 拉丁转写,重读元音带重音符,如 privét
    const char *zh;          // 简短中文释义(选项用),在词表内唯一
    const char *pos;         // 词性,如「名词·阴性」
    const char *full;        // 完整中文释义
    uint8_t category;        // RUW_CATEGORIES 下标
    uint8_t gender;          // 性别,见 RUW_GENDER_*; 非名词为 RUW_GENDER_NONE
    uint32_t audio_offset;   // 在 ADPCM blob 中的字节偏移
    uint32_t audio_samples;  // 12 kHz 采样数(每字节两个采样)
} ruw_word_t;

extern const ruw_word_t RUW_WORDS[RUW_WORD_COUNT];
extern const char *const RUW_CATEGORIES[];

#define RUW_AUDIO_RATE 12000

// 名词性别(与 gen_assets.py 的 GENDER_CODE 一致)
#define RUW_GENDER_M    0   // 阳性
#define RUW_GENDER_F    1   // 阴性
#define RUW_GENDER_N    2   // 中性
#define RUW_GENDER_PL   3   // 只有复数
#define RUW_GENDER_NONE 255 // 非名词
