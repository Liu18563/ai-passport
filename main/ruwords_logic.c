// main/ruwords_logic.c —— 见 ruwords_logic.h。
#include "ruwords_logic.h"

#include <string.h>

static const uint16_t INTERVALS[RUW_MAX_LEVEL + 1] = { 0, 1, 2, 4, 8, 16 };

uint16_t ruw_interval(uint8_t level) {
    if (level > RUW_MAX_LEVEL) level = RUW_MAX_LEVEL;
    return INTERVALS[level];
}

void ruw_progress_reset(ruw_progress_t *p) {
    p->round = 0;
    memset(p->level, 0, p->count);
    memset(p->due_round, 0, p->count * sizeof(p->due_round[0]));
}

uint16_t ruw_count_learned(const ruw_progress_t *p) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->count; i++) n += p->level[i] > 0;
    return n;
}

uint16_t ruw_count_mastered(const ruw_progress_t *p) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->count; i++) n += p->level[i] >= RUW_MAX_LEVEL;
    return n;
}

static bool is_due(const ruw_progress_t *p, uint16_t i) {
    return p->level[i] > 0 && p->due_round[i] <= p->round;
}

uint16_t ruw_count_due(const ruw_progress_t *p) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->count; i++) n += is_due(p, i);
    return n;
}

static bool in_session(const ruw_session_t *s, uint16_t w) {
    for (uint8_t i = 0; i < s->len; i++) {
        if (s->queue[i] == w) return true;
    }
    return false;
}

// 从已学词里挑 due_round 最小、且未入队的一个;only_due 时要求已到期。
static int pick_earliest(const ruw_session_t *s, const ruw_progress_t *p,
                         const uint8_t *eligible, bool only_due) {
    int best = -1;
    for (uint16_t i = 0; i < p->count; i++) {
        if (eligible && !eligible[i]) continue;
        if (p->level[i] == 0 || in_session(s, i)) continue;
        if (only_due && !is_due(p, i)) continue;
        if (best < 0 || p->due_round[i] < p->due_round[best]) best = i;
    }
    return best;
}

uint8_t ruw_session_build(ruw_session_t *s, const ruw_progress_t *p, const uint8_t *eligible) {
    memset(s, 0, sizeof(*s));

    // 复习给新词留出位置,避免复习堆积后永远学不到新词。
    uint8_t review_cap = RUW_SESSION_SIZE - RUW_SESSION_NEW_MAX / 2;
    while (s->len < review_cap) {
        int w = pick_earliest(s, p, eligible, true);
        if (w < 0) break;
        s->queue[s->len++] = (uint16_t)w;
    }
    for (uint16_t i = 0; i < p->count && s->len < RUW_SESSION_SIZE
                         && s->new_words < RUW_SESSION_NEW_MAX; i++) {
        if (p->level[i] == 0 && (!eligible || eligible[i])) {
            s->queue[s->len++] = i;
            s->new_words++;
        }
    }
    // 剩余到期复习,其次提前复习,把一轮补满。
    while (s->len < RUW_SESSION_SIZE) {
        int w = pick_earliest(s, p, eligible, true);
        if (w < 0) w = pick_earliest(s, p, eligible, false);
        if (w < 0) break;
        s->queue[s->len++] = (uint16_t)w;
    }
    return s->len;
}

bool ruw_session_answer(ruw_session_t *s, ruw_progress_t *p, bool correct) {
    if (s->pos >= s->len) return false;
    uint16_t w = s->queue[s->pos];
    bool requeued = s->requeued[s->pos];

    if (correct) {
        s->correct++;
        // 重排题答对只确认记住,不再升级,防止一轮内连升两级。
        if (!requeued && p->level[w] < RUW_MAX_LEVEL) p->level[w]++;
        if (p->level[w] == 0) p->level[w] = 1;
    } else {
        s->wrong++;
        p->level[w] = 1;
        if (!requeued && s->len < RUW_SESSION_CAP) {
            s->queue[s->len] = w;
            s->requeued[s->len] = 1;
            s->len++;
        }
    }
    // 本轮结束时 round+1,所以间隔从下一轮开始算。
    p->due_round[w] = (uint16_t)(p->round + ruw_interval(p->level[w]));

    s->pos++;
    return s->pos < s->len;
}

void ruw_session_finish(ruw_progress_t *p) {
    if (p->round < UINT16_MAX) p->round++;
}

static uint32_t next_rand(uint32_t *seed) {
    uint32_t x = *seed ? *seed : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

static bool chosen(const ruw_question_t *q, uint8_t n, uint16_t w) {
    for (uint8_t i = 0; i < n; i++) {
        if (q->word[i] == w) return true;
    }
    return false;
}

void ruw_question_make(ruw_question_t *q, uint16_t target, uint16_t count,
                       const uint8_t *category, uint32_t *seed) {
    memset(q, 0, sizeof(*q));
    q->word[0] = target;
    uint8_t n = 1;

    // 先随机抽同类干扰项,抽样次数有上限;同类不够时由下面的任意词补齐。
    for (int tries = 0; tries < 400 && n < RUW_OPTIONS; tries++) {
        uint16_t i = (uint16_t)(next_rand(seed) % count);
        if (category[i] == category[target] && !chosen(q, n, i)) q->word[n++] = i;
    }
    while (n < RUW_OPTIONS && n < count) {
        uint16_t i = (uint16_t)(next_rand(seed) % count);
        if (!chosen(q, n, i)) q->word[n++] = i;
    }

    // 打乱,并记下正确答案的位置。
    for (uint8_t i = RUW_OPTIONS - 1; i > 0; i--) {
        uint8_t j = (uint8_t)(next_rand(seed) % (i + 1u));
        uint16_t t = q->word[i];
        q->word[i] = q->word[j];
        q->word[j] = t;
    }
    for (uint8_t i = 0; i < RUW_OPTIONS; i++) {
        if (q->word[i] == target) q->answer = i;
    }
}
