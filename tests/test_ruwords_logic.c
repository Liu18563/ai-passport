#include <assert.h>
#include <string.h>

#include "ruwords_adpcm.h"
#include "ruwords_logic.h"

#define N 40

static uint8_t s_level[N];
static uint16_t s_due[N];
static uint8_t s_cat[N];

static ruw_progress_t make_progress(void) {
    ruw_progress_t p = { .count = N, .level = s_level, .due_round = s_due };
    ruw_progress_reset(&p);
    return p;
}

static void test_first_session_is_new_words(void) {
    ruw_progress_t p = make_progress();
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, NULL);
    // 没有已学词时只能引入新词,上限 RUW_SESSION_NEW_MAX。
    assert(n == RUW_SESSION_NEW_MAX);
    assert(s.new_words == RUW_SESSION_NEW_MAX);
    for (uint8_t i = 0; i < n; i++) assert(s.queue[i] == i);
}

static void test_wrong_answer_is_requeued_once(void) {
    ruw_progress_t p = make_progress();
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, NULL);
    assert(ruw_session_answer(&s, &p, false));
    assert(s.len == n + 1);
    assert(s.queue[n] == 0 && s.requeued[n]);
    assert(p.level[0] == 1);
    for (uint8_t i = 1; i < n; i++) assert(ruw_session_answer(&s, &p, true));
    // 重排题再答错也不再追加。
    assert(!ruw_session_answer(&s, &p, false));
    assert(s.len == n + 1);
    assert(s.correct == n - 1 && s.wrong == 2);
}

static void test_levels_and_due_rounds(void) {
    ruw_progress_t p = make_progress();
    ruw_session_t s;
    ruw_session_build(&s, &p, NULL);
    while (ruw_session_answer(&s, &p, true)) {}
    ruw_session_finish(&p);
    assert(p.round == 1);
    assert(ruw_count_learned(&p) == RUW_SESSION_NEW_MAX);
    // level 1 间隔 1 轮,下一轮全部到期。
    assert(ruw_count_due(&p) == RUW_SESSION_NEW_MAX);

    uint8_t n = ruw_session_build(&s, &p, NULL);
    assert(n == RUW_SESSION_SIZE);
    assert(s.new_words == RUW_SESSION_NEW_MAX);
    for (uint8_t i = 0; i < RUW_SESSION_NEW_MAX; i++) assert(s.queue[i] < RUW_SESSION_NEW_MAX);
}

static void test_mastery_caps_level(void) {
    ruw_progress_t p = make_progress();
    for (int round = 0; round < 60; round++) {
        ruw_session_t s;
        ruw_session_build(&s, &p, NULL);
        while (ruw_session_answer(&s, &p, true)) {}
        ruw_session_finish(&p);
    }
    assert(ruw_count_mastered(&p) > 0);
    for (int i = 0; i < N; i++) assert(p.level[i] <= RUW_MAX_LEVEL);
    assert(ruw_count_learned(&p) == N);
}

static void test_review_backlog_still_admits_new_words(void) {
    ruw_progress_t p = make_progress();
    for (int i = 0; i < 30; i++) { p.level[i] = 2; p.due_round[i] = 0; }
    p.round = 5;
    ruw_session_t s;
    ruw_session_build(&s, &p, NULL);
    assert(s.len == RUW_SESSION_SIZE);
    assert(s.new_words >= 1);
}

static void test_eligible_filter(void) {
    ruw_progress_t p = make_progress();
    uint8_t only[N] = {0};
    only[3] = only[17] = only[30] = 1;
    ruw_session_t s;
    // 分类范围内只有 3 个新词。
    assert(ruw_session_build(&s, &p, only) == 3);
    for (uint8_t i = 0; i < s.len; i++) assert(only[s.queue[i]]);
    // 错题本:全是已学词,未到期也要能考(提前复习)。
    uint8_t wrong[N] = {0};
    p.level[5] = 1; p.due_round[5] = 9;
    p.level[6] = 3; p.due_round[6] = 0;
    wrong[5] = wrong[6] = 1;
    assert(ruw_session_build(&s, &p, wrong) == 2);
    assert(s.queue[0] == 6);  // 已到期的排前面
    assert(s.new_words == 0);
    uint8_t none[N] = {0};
    assert(ruw_session_build(&s, &p, none) == 0);
}

static void test_filtered_session_stays_eligible(void) {
    // 开始学习 / 听音选义都按分类筛选用同一 eligible 掩码:
    // 一轮里即使答错重排到队尾,队列里也不能出现范围外的词。
    ruw_progress_t p = make_progress();
    uint8_t elig[N] = {0};
    for (int i = 0; i < 10; i++) elig[i] = 1;
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, elig);
    assert(n > 0);
    for (uint16_t i = 0; i < n; i++) assert(elig[s.queue[i]]);
    // 当前题答错会重排到队尾;重排进来的也必须在范围内。
    (void)ruw_session_answer(&s, &p, false);
    while (ruw_session_answer(&s, &p, true)) {}
    for (uint16_t i = 0; i < s.len; i++) assert(elig[s.queue[i]]);
}

static void test_question_options(void) {
    for (int i = 0; i < N; i++) s_cat[i] = (uint8_t)(i / 10);
    uint32_t seed = 12345;
    for (int t = 0; t < 200; t++) {
        uint16_t target = (uint16_t)(t % N);
        ruw_question_t q;
        ruw_question_make(&q, target, N, s_cat, &seed);
        assert(q.answer < RUW_OPTIONS);
        assert(q.word[q.answer] == target);
        for (int a = 0; a < RUW_OPTIONS; a++) {
            assert(q.word[a] < N);
            assert(s_cat[q.word[a]] == s_cat[target]);  // 同类足够时全部同类
            for (int b = a + 1; b < RUW_OPTIONS; b++) assert(q.word[a] != q.word[b]);
        }
    }
    // 同类不足时用其他类补齐,仍不重复。
    uint8_t cat2[N];
    memset(cat2, 0, sizeof(cat2));
    cat2[7] = 9;
    ruw_question_t q;
    ruw_question_make(&q, 7, N, cat2, &seed);
    assert(q.word[q.answer] == 7);
    for (int a = 0; a < RUW_OPTIONS; a++)
        for (int b = a + 1; b < RUW_OPTIONS; b++) assert(q.word[a] != q.word[b]);
}

static void test_adpcm_decode(void) {
    ruw_adpcm_t st;
    ruw_adpcm_init(&st);
    // 0x07: step 7 → delta = 0 + 7 + 3 + 1 = 11;index 0 + 8 = 8。
    // 0x0F: step 16 → delta = 2 + 16 + 8 + 4 = 30;11 - 30 = -19。
    const uint8_t src[] = { 0xF7 };
    int16_t out[2];
    ruw_adpcm_decode(&st, src, 2, out);
    assert(out[0] == 11);
    assert(out[1] == -19);
    assert(st.index == 16);
}

int main(void) {
    test_first_session_is_new_words();
    test_wrong_answer_is_requeued_once();
    test_levels_and_due_rounds();
    test_mastery_caps_level();
    test_review_backlog_still_admits_new_words();
    test_eligible_filter();
    test_filtered_session_stays_eligible();
    test_question_options();
    test_adpcm_decode();
    return 0;
}
