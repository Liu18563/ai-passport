// main/ruwords_logic.c —— 见 ruwords_logic.h。
#include "ruwords_logic.h"

#include <string.h>

void ruw_progress_reset(ruw_progress_t *p) {
    p->level = 0;
    memset(p->stage, 0, p->count);
    memset(p->last_seen, 0, p->count * sizeof(p->last_seen[0]));
}

uint8_t ruw_stage_next(uint8_t stage, bool correct, bool requeued) {
    if (requeued) return stage;  // 重排题不改状态
    if (correct) {
        switch (stage) {
        case RUW_STAGE_NEW:    return RUW_STAGE_DONE;
        case RUW_STAGE_WRONG2: return RUW_STAGE_WRONG1;
        case RUW_STAGE_WRONG1: return RUW_STAGE_DONE;
        case RUW_STAGE_DONE:   return RUW_STAGE_DONE;
        }
    } else {
        switch (stage) {
        case RUW_STAGE_NEW:    return RUW_STAGE_WRONG2;
        case RUW_STAGE_WRONG1:
        case RUW_STAGE_WRONG2: return RUW_STAGE_WRONG2;
        case RUW_STAGE_DONE:   return RUW_STAGE_DONE;
        }
    }
    return stage;
}

uint16_t ruw_count_mastered(const ruw_progress_t *p) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->count; i++) n += p->stage[i] == RUW_STAGE_DONE;
    return n;
}

uint16_t ruw_count_new(const ruw_progress_t *p) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->count; i++) n += p->stage[i] == RUW_STAGE_NEW;
    return n;
}

uint16_t ruw_count_wrong(const ruw_progress_t *p) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < p->count; i++)
        n += p->stage[i] == RUW_STAGE_WRONG1 || p->stage[i] == RUW_STAGE_WRONG2;
    return n;
}

static bool in_queue(const ruw_session_t *s, uint16_t w) {
    for (uint8_t i = 0; i < s->len; i++)
        if (s->queue[i] == w) return true;
    return false;
}

// 找一个未入队的错词,last_seen 最小(最久没考)的优先。
static int pick_wrong(const ruw_session_t *s, const ruw_progress_t *p,
                      const uint8_t *eligible) {
    int best = -1;
    for (uint16_t i = 0; i < p->count; i++) {
        if (eligible && !eligible[i]) continue;
        if (p->stage[i] != RUW_STAGE_WRONG1 && p->stage[i] != RUW_STAGE_WRONG2) continue;
        if (in_queue(s, i)) continue;
        if (best < 0 || p->last_seen[i] < p->last_seen[best]) best = i;
    }
    return best;
}

uint8_t ruw_session_build(ruw_session_t *s, const ruw_progress_t *p,
                          const uint8_t *eligible) {
    memset(s, 0, sizeof(*s));
    s->current_level = (p->level < UINT16_MAX) ? (uint16_t)(p->level + 1) : UINT16_MAX;

    // 1. 先放错词,最多 5 个,给新词留位置。
    while (s->len < RUW_WRONG_MAX) {
        int w = pick_wrong(s, p, eligible);
        if (w < 0) break;
        s->queue[s->len++] = (uint16_t)w;
        s->wrong_words++;
    }
    // 2. 用 NEW 补满 10。
    for (uint16_t i = 0; i < p->count && s->len < RUW_LEVEL_SIZE; i++) {
        if (p->stage[i] == RUW_STAGE_NEW && (!eligible || eligible[i])
            && !in_queue(s, i)) {
            s->queue[s->len++] = i;
            s->new_words++;
        }
    }
    // 3. NEW 用完后,剩余错词占满 10。
    while (s->len < RUW_LEVEL_SIZE) {
        int w = pick_wrong(s, p, eligible);
        if (w < 0) break;
        s->queue[s->len++] = (uint16_t)w;
        s->wrong_words++;
    }
    return s->len;
}

bool ruw_session_answer(ruw_session_t *s, ruw_progress_t *p, bool correct) {
    if (s->pos >= s->len) return false;
    uint16_t w = s->queue[s->pos];
    bool requeued = s->requeued[s->pos];

    if (correct) s->correct++;
    else s->wrong++;

    p->stage[w] = ruw_stage_next(p->stage[w], correct, requeued);
    p->last_seen[w] = s->current_level;

    // 非重排题答错:关尾重排再考一次。
    if (!correct && !requeued && s->len < RUW_SESSION_CAP) {
        s->queue[s->len] = w;
        s->requeued[s->len] = 1;
        s->len++;
    }

    s->pos++;
    return s->pos < s->len;
}

void ruw_session_finish(ruw_progress_t *p) {
    if (p->level < UINT16_MAX) p->level++;
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
    for (uint8_t i = 0; i < n; i++)
        if (q->word[i] == w) return true;
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
    for (uint8_t i = 0; i < RUW_OPTIONS; i++)
        if (q->word[i] == target) q->answer = i;
}
