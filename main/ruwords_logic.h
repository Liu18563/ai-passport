// main/ruwords_logic.h —— 俄语单词斩的纯逻辑:间隔复习(Leitner 盒)、出题、选项生成。
// 不依赖 ESP-IDF / LVGL,由 tests/test_ruwords_logic.c 在主机上覆盖。
//
// 设备没有可靠的实时时钟,所以"到期"按轮次计算:每完成一轮学习 round 加一,
// 单词在 due_round 到达后才进入复习。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RUW_MAX_LEVEL 5          // 0 = 未学;1..5 = 记忆强度
#define RUW_SESSION_SIZE 10      // 每轮题数(不含答错重排的)
#define RUW_SESSION_NEW_MAX 5    // 每轮最多引入的新词
#define RUW_SESSION_CAP 16       // 队列容量(含答错重排)
#define RUW_OPTIONS 4

typedef struct {
    uint16_t count;              // 词数,<= 容量
    uint16_t round;              // 已完成的轮数
    uint8_t *level;              // [count]
    uint16_t *due_round;         // [count]
} ruw_progress_t;

typedef struct {
    uint16_t queue[RUW_SESSION_CAP];
    uint8_t requeued[RUW_SESSION_CAP];  // 该位置是否为答错后重排的题
    uint8_t len;
    uint8_t pos;
    uint8_t correct;
    uint8_t wrong;
    uint8_t new_words;
} ruw_session_t;

typedef struct {
    uint16_t word[RUW_OPTIONS];  // 四个选项对应的词下标
    uint8_t answer;              // 正确选项位置 0..3
} ruw_question_t;

// 轮次间隔:答对后 level 对应的间隔轮数。
uint16_t ruw_interval(uint8_t level);

void ruw_progress_reset(ruw_progress_t *p);

// 统计:已学(level>0)、已掌握(level==MAX)、当前到期待复习数。
uint16_t ruw_count_learned(const ruw_progress_t *p);
uint16_t ruw_count_mastered(const ruw_progress_t *p);
uint16_t ruw_count_due(const ruw_progress_t *p);

// 组一轮题:先放到期复习(最早到期优先),再补新词,不足时提前复习。
// eligible 为 NULL 时全部词可选;否则只选 eligible[i] 非零的词(分类、错题本)。
// 返回题数;0 表示范围内没有可学内容。
uint8_t ruw_session_build(ruw_session_t *s, const ruw_progress_t *p, const uint8_t *eligible);

// 记录当前题的作答结果并前进。答错的题(非重排题)会追加到队尾再考一次。
// 返回 true 表示本轮还有下一题。
bool ruw_session_answer(ruw_session_t *s, ruw_progress_t *p, bool correct);

// 一轮结束时调用:round 加一。
void ruw_session_finish(ruw_progress_t *p);

// 生成四选一。category 为每个词的分类数组;优先选同类干扰项。
// seed 由调用方提供(xorshift),同一 seed 结果确定,便于测试。
void ruw_question_make(ruw_question_t *q, uint16_t target, uint16_t count,
                       const uint8_t *category, uint32_t *seed);
