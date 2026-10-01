#include <assert.h>
#include <string.h>

#include "ruwords_adpcm.h"
#include "ruwords_logic.h"

#define N 40

static uint8_t s_stage[N];
static uint16_t s_seen[N];
static uint8_t s_cat[N];

static ruw_progress_t make_progress(void) {
    ruw_progress_t p = { .count = N, .level = 0, .stage = s_stage, .last_seen = s_seen };
    ruw_progress_reset(&p);
    return p;
}

// 2.2 节每一条状态转移。
static void test_stage_transitions(void) {
    assert(ruw_stage_next(RUW_STAGE_NEW, true, false) == RUW_STAGE_DONE);
    assert(ruw_stage_next(RUW_STAGE_NEW, false, false) == RUW_STAGE_WRONG2);
    assert(ruw_stage_next(RUW_STAGE_WRONG2, true, false) == RUW_STAGE_WRONG1);
    assert(ruw_stage_next(RUW_STAGE_WRONG2, false, false) == RUW_STAGE_WRONG2);
    assert(ruw_stage_next(RUW_STAGE_WRONG1, true, false) == RUW_STAGE_DONE);
    assert(ruw_stage_next(RUW_STAGE_WRONG1, false, false) == RUW_STAGE_WRONG2);
    assert(ruw_stage_next(RUW_STAGE_DONE, true, false) == RUW_STAGE_DONE);
    assert(ruw_stage_next(RUW_STAGE_DONE, false, false) == RUW_STAGE_DONE);
}

// 重排题答对、答错都不改变状态。
static void test_requeued_keeps_stage(void) {
    assert(ruw_stage_next(RUW_STAGE_NEW, true, true) == RUW_STAGE_NEW);
    assert(ruw_stage_next(RUW_STAGE_WRONG2, true, true) == RUW_STAGE_WRONG2);
    assert(ruw_stage_next(RUW_STAGE_WRONG1, false, true) == RUW_STAGE_WRONG1);
    assert(ruw_stage_next(RUW_STAGE_WRONG2, false, true) == RUW_STAGE_WRONG2);
}

// 第一关全是 NEW,一关 10 题。
static void test_first_level_new_words(void) {
    ruw_progress_t p = make_progress();
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, NULL);
    assert(n == RUW_LEVEL_SIZE);
    assert(s.new_words == 10 && s.wrong_words == 0);
    assert(s.current_level == 1);
    for (uint8_t i = 0; i < n; i++) assert(s.queue[i] == i);
}

// 答错的非重排题重排到队尾;重排题答错不改状态、不再追加。
static void test_wrong_answer_requeued(void) {
    ruw_progress_t p = make_progress();
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, NULL);
    assert(ruw_session_answer(&s, &p, false));  // 词0 NEW 答错
    assert(s_stage[0] == RUW_STAGE_WRONG2);
    assert(s.len == n + 1 && s.queue[n] == 0 && s.requeued[n]);
    assert(s_seen[0] == 1);
    for (uint8_t i = 1; i < n; i++) assert(ruw_session_answer(&s, &p, true));
    assert(!ruw_session_answer(&s, &p, false));  // 重排题再答错
    assert(s_stage[0] == RUW_STAGE_WRONG2);
    assert(s.len == n + 1);
}

// 一关先放错词最多 5 个,按最久没考优先,再用 NEW 补满。
static void test_level_has_at_most_five_wrong(void) {
    ruw_progress_t p = make_progress();
    for (int i = 0; i < 8; i++) {
        s_stage[i] = RUW_STAGE_WRONG2;
        s_seen[i] = (uint16_t)(i + 1);
    }
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, NULL);
    assert(n == RUW_LEVEL_SIZE);
    assert(s.wrong_words == 5 && s.new_words == 5);
    assert(s.queue[0] == 0);  // 最久没考(last_seen=1)
    for (int i = 0; i < 5; i++) assert(s.queue[i] == i);
    for (int i = 5; i < 10; i++) assert(s.queue[i] >= 8);
}

// NEW 用完后,错词可以占满 10 个。
static void test_wrong_fills_when_no_new(void) {
    ruw_progress_t p = make_progress();
    for (int i = 0; i < 30; i++) s_stage[i] = RUW_STAGE_DONE;
    for (int i = 30; i < N; i++) s_stage[i] = RUW_STAGE_WRONG2;
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, NULL);
    assert(n == RUW_LEVEL_SIZE);
    assert(s.wrong_words == 10 && s.new_words == 0);
    for (int i = 0; i < 10; i++) assert(s.queue[i] >= 30);
}

// 全部 DONE 时组关返回 0。
static void test_all_done_returns_zero(void) {
    ruw_progress_t p = make_progress();
    for (int i = 0; i < N; i++) s_stage[i] = RUW_STAGE_DONE;
    ruw_session_t s;
    assert(ruw_session_build(&s, &p, NULL) == 0);
}

// 一关打完关数 +1,打完前不加。
static void test_finish_increments_level(void) {
    ruw_progress_t p = make_progress();
    ruw_session_t s;
    ruw_session_build(&s, &p, NULL);
    while (ruw_session_answer(&s, &p, true)) {}
    assert(p.level == 0);
    ruw_session_finish(&p);
    assert(p.level == 1);
}

// 分类筛选、错题本筛选下,组出的题全在范围内。
static void test_eligible_filter(void) {
    ruw_progress_t p = make_progress();
    uint8_t only[N] = {0};
    only[3] = only[17] = only[30] = 1;
    ruw_session_t s;
    assert(ruw_session_build(&s, &p, only) == 3);
    for (uint8_t i = 0; i < s.len; i++) assert(only[s.queue[i]]);

    uint8_t wrong[N] = {0};
    s_stage[5] = RUW_STAGE_WRONG2; s_seen[5] = 3;
    s_stage[6] = RUW_STAGE_WRONG1; s_seen[6] = 1;
    wrong[5] = wrong[6] = 1;
    assert(ruw_session_build(&s, &p, wrong) == 2);
    assert(s.queue[0] == 6);  // 最久没考
    assert(s.new_words == 0);
    uint8_t none[N] = {0};
    assert(ruw_session_build(&s, &p, none) == 0);
}

// 筛选范围内答错重排,重排进来的也必须在范围内。
static void test_filtered_requeue_in_range(void) {
    ruw_progress_t p = make_progress();
    uint8_t elig[N] = {0};
    for (int i = 0; i < 10; i++) elig[i] = 1;
    ruw_session_t s;
    uint8_t n = ruw_session_build(&s, &p, elig);
    assert(n > 0);
    (void)ruw_session_answer(&s, &p, false);
    while (ruw_session_answer(&s, &p, true)) {}
    for (uint16_t i = 0; i < s.len; i++) assert(elig[s.queue[i]]);
}

static void test_counts(void) {
    ruw_progress_t p = make_progress();
    s_stage[0] = s_stage[1] = RUW_STAGE_DONE;
    s_stage[2] = RUW_STAGE_WRONG1; s_stage[3] = RUW_STAGE_WRONG2;
    assert(ruw_count_mastered(&p) == 2);
    assert(ruw_count_wrong(&p) == 2);
    assert(ruw_count_new(&p) == N - 4);
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

// 阴阳中性判断提示:规律、例外、-ь、复数各至少一例。
static void test_gender_hint(void) {
    const char *h;
    // 规律:辅音→阳性、-а→阴性、-о→中性、-й→阳性。
    h = ruw_gender_hint("стол", RUW_GENDER_M);
    assert(strstr(h, "阳性") && !strstr(h, "例外"));
    h = ruw_gender_hint("книга", RUW_GENDER_F);
    assert(strstr(h, "阴性") && !strstr(h, "例外"));
    h = ruw_gender_hint("письмо", RUW_GENDER_N);
    assert(strstr(h, "中性") && !strstr(h, "例外"));
    h = ruw_gender_hint("музей", RUW_GENDER_M);
    assert(strstr(h, "阳性") && !strstr(h, "例外"));
    // -ь 结尾阳阴都有。
    h = ruw_gender_hint("дождь", RUW_GENDER_M);
    assert(strstr(h, "-ь"));
    h = ruw_gender_hint("тетрадь", RUW_GENDER_F);
    assert(strstr(h, "-ь"));
    // 例外:папа(-а但阳性)、кофе(-е但阳性)、время(-я但中性)、такси(-и但中性)。
    assert(strstr(ruw_gender_hint("папа", RUW_GENDER_M), "例外"));
    assert(strstr(ruw_gender_hint("кофе", RUW_GENDER_M), "例外"));
    assert(strstr(ruw_gender_hint("время", RUW_GENDER_N), "例外"));
    assert(strstr(ruw_gender_hint("такси", RUW_GENDER_N), "例外"));
    // 只有复数。
    h = ruw_gender_hint("деньги", RUW_GENDER_PL);
    assert(strstr(h, "复数") && !strstr(h, "例外"));
}

// 性别模式:只考名词(范围掩码),全 DONE 后返回 0;与词义 progress 独立。
static void test_gender_mode_flow(void) {
    static uint8_t gst[N];
    static uint16_t gseen[N];
    ruw_progress_t gp = { .count = N, .level = 0, .stage = gst, .last_seen = gseen };
    ruw_progress_reset(&gp);

    uint8_t nouns[N] = {0};
    for (int i = 0; i < 10; i++) nouns[i] = 1;  // 只有词0..9 是名词

    ruw_session_t s;
    assert(ruw_session_build(&s, &gp, nouns) == 10);
    for (uint8_t i = 0; i < s.len; i++) assert(nouns[s.queue[i]]);
    while (ruw_session_answer(&s, &gp, true)) {}
    ruw_session_finish(&gp);
    assert(ruw_count_mastered(&gp) == 10);
    assert(ruw_session_build(&s, &gp, nouns) == 0);  // 名词全 DONE
}

// v2 单状态迁移:wrong>0→WRONG2;level>0→DONE;其余 NEW。
static void test_migrate_stage(void) {
    assert(ruw_migrate_stage(1, 0) == RUW_STAGE_DONE);
    assert(ruw_migrate_stage(3, 0) == RUW_STAGE_DONE);
    assert(ruw_migrate_stage(0, 1) == RUW_STAGE_WRONG2);
    assert(ruw_migrate_stage(2, 1) == RUW_STAGE_WRONG2);  // wrong 优先
    assert(ruw_migrate_stage(0, 0) == RUW_STAGE_NEW);
}

// v2→v3:词表重排后按原文映射(旧下标→新下标),三种结果 + 未匹配新词保持 NEW。
static void test_migrate_v2(void) {
    enum { NEW_C = 8, OLD_C = 5 };
    // 旧词顺序与新词不同:旧0→新5,旧1→新2,旧2→新7,旧3→新0,旧4→新3。
    const uint16_t map[OLD_C] = { 5, 2, 7, 0, 3 };
    const uint8_t old_level[OLD_C] = { 1, 0, 0, 2, 0 };
    const uint8_t old_wrong[OLD_C] = { 0, 1, 0, 0, 3 };
    uint8_t out[NEW_C];
    ruw_migrate_v2(out, NEW_C, map, OLD_C, old_level, old_wrong);
    // 旧0→新5 DONE;旧1→新2 WRONG2;旧2→新7 NEW;旧3→新0 DONE;旧4→新3 WRONG2。
    assert(out[0] == RUW_STAGE_DONE);
    assert(out[1] == RUW_STAGE_NEW);   // 未匹配新词
    assert(out[2] == RUW_STAGE_WRONG2);
    assert(out[3] == RUW_STAGE_WRONG2);
    assert(out[4] == RUW_STAGE_NEW);   // 未匹配新词
    assert(out[5] == RUW_STAGE_DONE);
    assert(out[6] == RUW_STAGE_NEW);   // 未匹配新词
    assert(out[7] == RUW_STAGE_NEW);
}

// 1300 词规模:每个词都能生成 4 个不重复、唯一正确的选项。
static void test_question_large_scale(void) {
    const uint16_t M = 1300;
    static uint8_t cat[1300];
    for (uint16_t i = 0; i < M; i++) cat[i] = (uint8_t)(i % 18);
    uint32_t seed = 0xABCD;
    for (uint16_t t = 0; t < M; t++) {
        ruw_question_t q;
        ruw_question_make(&q, t, M, cat, &seed);
        assert(q.answer < RUW_OPTIONS);
        assert(q.word[q.answer] == t);          // 唯一正确
        for (int a = 0; a < RUW_OPTIONS; a++) {
            assert(q.word[a] < M);
            assert(cat[q.word[a]] == cat[t]);   // 每类约72词,全部同类
            for (int b = a + 1; b < RUW_OPTIONS; b++)
                assert(q.word[a] != q.word[b]); // 不重复
        }
    }
    // 极端:某类只有 1 个词,其余全不同类,仍能跨类补齐 4 个不重复选项。
    static uint8_t cat2[1300];
    memset(cat2, 0, sizeof(cat2));
    for (uint16_t i = 1; i < M; i++) cat2[i] = (uint8_t)(i % 17) + 1;
    ruw_question_t q;
    ruw_question_make(&q, 0, M, cat2, &seed);
    assert(q.word[q.answer] == 0);
    for (int a = 0; a < RUW_OPTIONS; a++)
        for (int b = a + 1; b < RUW_OPTIONS; b++)
            assert(q.word[a] != q.word[b]);
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
    test_stage_transitions();
    test_requeued_keeps_stage();
    test_first_level_new_words();
    test_wrong_answer_requeued();
    test_level_has_at_most_five_wrong();
    test_wrong_fills_when_no_new();
    test_all_done_returns_zero();
    test_finish_increments_level();
    test_eligible_filter();
    test_filtered_requeue_in_range();
    test_counts();
    test_question_options();
    test_gender_hint();
    test_gender_mode_flow();
    test_migrate_stage();
    test_migrate_v2();
    test_question_large_scale();
    test_adpcm_decode();
    return 0;
}
