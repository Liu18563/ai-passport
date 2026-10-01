// main/ruwords_logic.h —— 俄语单词斩的纯逻辑:闯关制、出题、选项生成。
// 不依赖 ESP-IDF / LVGL,由 tests/test_ruwords_logic.c 在主机上覆盖。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 每个词的闯关状态(1 字节)。
#define RUW_STAGE_NEW    0   // 没学过
#define RUW_STAGE_DONE   1   // 已掌握,不再出现在闯关和听音里
#define RUW_STAGE_WRONG2 2   // 错词,还需答对 2 次
#define RUW_STAGE_WRONG1 3   // 错词,还需答对 1 次

#define RUW_LEVEL_SIZE   10  // 一关 10 题(不含答错重排的)
#define RUW_WRONG_MAX    5   // 一关先放错词的上限
#define RUW_SESSION_CAP  20  // 队列容量(含答错重排)
#define RUW_OPTIONS      4

// 名词性别编码(与 gen_assets.py 的 GENDER_CODE 一致;非名词为 NONE)。
#define RUW_GENDER_M    0   // 阳性
#define RUW_GENDER_F    1   // 阴性
#define RUW_GENDER_N    2   // 中性
#define RUW_GENDER_PL   3   // 只有复数
#define RUW_GENDER_NONE 255

typedef struct {
    uint16_t count;              // 词数
    uint16_t level;              // 已打完的关数
    uint8_t *stage;              // [count] 词义状态
    uint16_t *last_seen;         // [count] 最后一次考到的关数
} ruw_progress_t;

typedef struct {
    uint16_t queue[RUW_SESSION_CAP];
    uint8_t requeued[RUW_SESSION_CAP];  // 该位置是否为答错后重排的题
    uint16_t current_level;      // 本关序号(考到时写进 last_seen)
    uint8_t len;
    uint8_t pos;
    uint8_t correct;
    uint8_t wrong;
    uint8_t new_words;
    uint8_t wrong_words;
} ruw_session_t;

typedef struct {
    uint16_t word[RUW_OPTIONS];  // 四个选项对应的词下标
    uint8_t answer;              // 正确选项位置 0..3
} ruw_question_t;

void ruw_progress_reset(ruw_progress_t *p);

// 状态转移:返回作答后的新状态。requeued(重排题)时原样返回,不改状态。
uint8_t ruw_stage_next(uint8_t stage, bool correct, bool requeued);

// 统计:DONE 数、NEW 数、错词(WRONG1/WRONG2)数。
uint16_t ruw_count_mastered(const ruw_progress_t *p);
uint16_t ruw_count_new(const ruw_progress_t *p);
uint16_t ruw_count_wrong(const ruw_progress_t *p);

// 组一关:先错词(最多 5,按 last_seen 最久没考优先),再 NEW 补满;
// NEW 用完后错词可占满 10。eligible 为 NULL 时全部可选,否则只选非零项。
// 返回题数;0 表示范围内没有可学内容。
uint8_t ruw_session_build(ruw_session_t *s, const ruw_progress_t *p, const uint8_t *eligible);

// 记录当前题作答并前进。答错的非重排题追加到队尾再考一次(重排题不计状态)。
// 返回 true 表示本关还有下一题。
bool ruw_session_answer(ruw_session_t *s, ruw_progress_t *p, bool correct);

// 一关打完:关数 +1。
void ruw_session_finish(ruw_progress_t *p);

// 生成四选一。category 为每个词的分类数组;优先选同类干扰项。
// seed 由调用方提供(xorshift),同一 seed 结果确定,便于测试。
void ruw_question_make(ruw_question_t *q, uint16_t target, uint16_t count,
                       const uint8_t *category, uint32_t *seed);

// 阴阳中性:根据词尾与实际性别生成「判断提示」。
// gender 取 RUW_GENDER_*;pl 返回「只有复数形式」。
const char *ruw_gender_hint(const char *ru, uint8_t gender);

// v2→v3 迁移。旧 Leitner 单状态:wrong>0→WRONG2;level>0→DONE;其余 NEW。
uint8_t ruw_migrate_stage(uint8_t old_level, uint8_t old_wrong);

// 按「旧下标→新下标」映射表把旧存档迁移到新状态数组。
// 映射表按俄语原文构建(下标已变,不能直接按下标复制);
// 新数组先全部置 NEW,未匹配到的新词保持 NEW。
void ruw_migrate_v2(uint8_t *out_stage, uint16_t new_count,
                    const uint16_t *map, uint16_t old_count,
                    const uint8_t *old_level, const uint8_t *old_wrong);
