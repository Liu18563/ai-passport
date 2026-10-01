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

void ruw_category_stats(ruw_category_stats_t *out, uint8_t category_filter,
                        uint16_t count, const uint8_t *category, const uint8_t *gender,
                        const uint8_t *stage, const uint8_t *gender_stage) {
    memset(out, 0, sizeof(*out));
    for (uint16_t i = 0; i < count; i++) {
        if (category_filter != 0 && category[i] != category_filter - 1) continue;
        out->words++;
        switch (stage[i]) {
        case RUW_STAGE_DONE: out->mastered++; break;
        case RUW_STAGE_NEW:  out->fresh++; break;
        case RUW_STAGE_WRONG1:
        case RUW_STAGE_WRONG2: out->wrong++; break;
        }
        if (gender[i] != RUW_GENDER_NONE) {
            out->nouns++;
            if (gender_stage[i] == RUW_STAGE_DONE) out->nouns_mastered++;
        }
    }
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

bool ruw_session_valid(const ruw_session_t *s, uint16_t word_count) {
    if (s->len > RUW_SESSION_CAP) return false;
    if (s->pos > s->len) return false;
    for (uint8_t i = 0; i < s->len; i++) {
        if (s->queue[i] >= word_count) return false;
        if (s->requeued[i] > 1) return false;
    }
    return true;
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

// 取 UTF-8 字符串最后一个 Unicode 码点。
static uint32_t last_codepoint(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp = 0;
    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) { cp = c; p++; }
        else if (c < 0xE0) { cp = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
        else if (c < 0xF0) {
            cp = ((c & 0x0Fu) << 10) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            p += 3;
        } else {
            cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12)
               | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            p += 4;
        }
    }
    return cp;
}

const char *ruw_gender_hint(const char *ru, uint8_t gender) {
    if (gender == RUW_GENDER_PL) return "只有复数形式";

    const uint32_t A = 0x0430, YA = 0x044F, O = 0x043E, E = 0x0435, YO = 0x0451,
                   SOFT = 0x044C, Y = 0x0439, I = 0x0438, YERY = 0x044B;
    uint32_t c = last_codepoint(ru);

    if (c == SOFT) return "以-ь结尾，阳、阴性都有，需单独记";
    if (c == A || c == YA) {
        if (gender == RUW_GENDER_M) return "例外：以-а/-я结尾，但该词是阳性";
        if (gender == RUW_GENDER_N) return "例外：以-а/-я结尾，但该词是中性（特殊词）";
        return "以-а/-я结尾，通常阴性";
    }
    if (c == O || c == E || c == YO) {
        if (gender == RUW_GENDER_M) return "例外：以-о/-е/-ё结尾，但该词是阳性";
        if (gender == RUW_GENDER_F) return "例外：以-о/-е/-ё结尾，但该词是阴性";
        return "以-о/-е/-ё结尾，通常中性";
    }
    if (c == I) {
        if (gender == RUW_GENDER_N) return "例外：以-и结尾的外来词，通常中性";
        if (gender == RUW_GENDER_M) return "例外：以-и结尾，但该词是阳性";
        return "例外：以-и结尾，需单独记";
    }
    if (c == YERY) {
        if (gender == RUW_GENDER_M) return "例外：以-ы结尾，但该词是阳性（有单数形式）";
        return "例外：以-ы结尾，需单独记";
    }
    // 辅音或 -й 结尾。
    if (gender == RUW_GENDER_F) return "例外：以辅音/-й结尾，但该词是阴性";
    if (gender == RUW_GENDER_N) return "例外：以辅音/-й结尾，但该词是中性";
    if (c == Y) return "以-й结尾，通常阳性";
    return "以辅音结尾，通常阳性";
}

// ---------------------------------------------------------------- v2→v3 迁移

uint8_t ruw_migrate_stage(uint8_t old_level, uint8_t old_wrong) {
    if (old_wrong > 0) return RUW_STAGE_WRONG2;
    if (old_level > 0) return RUW_STAGE_DONE;
    return RUW_STAGE_NEW;
}

void ruw_migrate_v2(uint8_t *out_stage, uint16_t new_count,
                    const uint16_t *map, uint16_t old_count,
                    const uint8_t *old_level, const uint8_t *old_wrong) {
    for (uint16_t i = 0; i < new_count; i++) out_stage[i] = RUW_STAGE_NEW;
    for (uint16_t i = 0; i < old_count; i++) {
        uint16_t ni = map[i];
        if (ni < new_count)
            out_stage[ni] = ruw_migrate_stage(old_level[i], old_wrong[i]);
    }
}
