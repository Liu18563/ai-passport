// main/ruwords_app.c —— 俄语单词斩:看俄语(重音标红+拉丁转写)、听发音、四选一中文释义。
//
// 页面:首页菜单 → 分类选择 / 答题(作答后显示词性与完整释义)→ 本轮完成 → 首页。
// 按键:
//   首页      上/下 = 移动菜单;确定 = 执行(开始学习 / 听音选义 / 选分类 / 错题本 / 切换音量)
//   分类      上/下 = 移动;确定 = 选定并回首页;确定 长按 = 不改回首页
//   答题      上/下 = 移动选项;确定 = 提交;上 长按 = 重听;确定 长按 = 回首页
//             (听音模式:答题前只显示大"听"区,作答后才揭晓俄语/转写/释义)
//   作答后    上/下 = 重听;确定 = 下一题;确定 长按 = 回首页
//   本轮完成  确定 = 回首页
// 任意键在息屏时只负责唤醒,不触发操作。
//
// 线程:按键回调只入队;input 任务持 LVGL 锁改界面并写 NVS;audio 任务解码播放。
#include "ruwords_app.h"

#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ruwords_adpcm.h"
#include "ruwords_data.h"
#include "ruwords_logic.h"

static const char *TAG = "ruwords";

LV_FONT_DECLARE(ruw_font_20);
LV_FONT_DECLARE(ruw_font_28);
LV_FONT_DECLARE(ruw_font_36);

extern const uint8_t s_voice_start[] asm("_binary_ruwords_voice_adpcm_start");
extern const uint8_t s_voice_end[] asm("_binary_ruwords_voice_adpcm_end");

// ---- 视觉 ----
#define COL_BG      0xF6F1E7
#define COL_CARD    0xFFFFFF
#define COL_INK     0x1F2A44
#define COL_MUTED   0x8A8F9C
#define COL_ACCENT  0xC8102E   // 与 tools/ruwords/gen_assets.py 的 STRESS_COLOR 一致
#define COL_OK      0x2E8B57
#define COL_OK_BG   0xDDF1E4
#define COL_BAD_BG  0xF8DADF

#define IDLE_DIM_MS     30000
#define IDLE_OFF_MS     60000
#define BATTERY_POLL_MS 30000

// 音量档位:codec 的 0..100 是分贝刻度,低于 60 基本听不清。
static const uint8_t VOLUME_STEPS[] = { 60, 70, 80, 90, 100 };
#define VOLUME_LEVELS (sizeof(VOLUME_STEPS) / sizeof(VOLUME_STEPS[0]))
#define VOLUME_DEFAULT (VOLUME_LEVELS - 1)

// ---- 状态 ----
typedef enum { PAGE_HOME, PAGE_CATEGORY, PAGE_QUIZ, PAGE_DONE } page_t;
typedef enum { MENU_START, MENU_LISTEN, MENU_CATEGORY, MENU_WRONG, MENU_VOLUME, MENU_COUNT } menu_t;
typedef enum { MODE_STUDY, MODE_WRONG, MODE_LISTEN } study_mode_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

static uint8_t s_level[RUW_WORD_COUNT];
static uint16_t s_due[RUW_WORD_COUNT];
static uint8_t s_wrong[RUW_WORD_COUNT];       // 答错次数;>0 即在错题本里
static uint8_t s_category[RUW_WORD_COUNT];
static uint8_t s_eligible[RUW_WORD_COUNT];
static ruw_progress_t s_progress = {
    .count = RUW_WORD_COUNT, .level = s_level, .due_round = s_due,
};
static uint8_t s_volume_level = VOLUME_DEFAULT;
static uint8_t s_filter;                       // 0 = 全部;n = 第 n-1 个分类

static ruw_session_t s_session;
static ruw_question_t s_question;
static uint32_t s_seed;
static study_mode_t s_mode;

static page_t s_page;
static uint8_t s_menu;
static uint8_t s_cat_cursor;
static uint8_t s_selected;
static bool s_answered;
static bool s_audio_ok;
static bool s_screen_off;
static int64_t s_last_input_ms;

static QueueHandle_t s_input_queue;
static QueueHandle_t s_audio_queue;
static volatile bool s_audio_abort;

// 当前页的控件(换页时整屏删除后置空)
static lv_obj_t *s_scr;
static lv_obj_t *s_battery;
static lv_obj_t *s_rows[RUW_CATEGORY_COUNT + 1];
static lv_obj_t *s_row_labels[RUW_CATEGORY_COUNT + 1];
static uint8_t s_row_count;
static lv_obj_t *s_hint;
static lv_obj_t *s_hear_prompt;   // 听音模式:作答前的大"听"提示区(作答后删除)

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---------------------------------------------------------------- 存储

#define NVS_NS "ruwords"
#define NVS_KEY "prog"
#define PROG_VERSION 2

typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t round;
    uint8_t volume_level;
    uint8_t filter;
    uint8_t level[RUW_WORD_COUNT];
    uint8_t wrong[RUW_WORD_COUNT];
    uint16_t due[RUW_WORD_COUNT];
} saved_progress_t;

// v1(首版)只有 level/due,升级时保留学习进度。
typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t round;
    uint8_t level[RUW_WORD_COUNT];
    uint16_t due[RUW_WORD_COUNT];
} saved_progress_v1_t;

static union {
    saved_progress_t v2;
    saved_progress_v1_t v1;
} s_blob;  // 静态:避免占 input 任务栈

static void progress_load(void) {
    ruw_progress_reset(&s_progress);
    memset(s_wrong, 0, sizeof(s_wrong));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &s_blob, &len);
    nvs_close(h);
    if (err != ESP_OK) return;

    // 词表变化后旧进度对不上下标,直接从头开始。
    if (len == sizeof(saved_progress_t) && s_blob.v2.version == PROG_VERSION
        && s_blob.v2.count == RUW_WORD_COUNT) {
        s_progress.round = s_blob.v2.round;
        memcpy(s_level, s_blob.v2.level, sizeof(s_level));
        memcpy(s_due, s_blob.v2.due, sizeof(s_due));
        memcpy(s_wrong, s_blob.v2.wrong, sizeof(s_wrong));
        if (s_blob.v2.volume_level < VOLUME_LEVELS) s_volume_level = s_blob.v2.volume_level;
        if (s_blob.v2.filter <= RUW_CATEGORY_COUNT) s_filter = s_blob.v2.filter;
    } else if (len == sizeof(saved_progress_v1_t) && s_blob.v1.version == 1
               && s_blob.v1.count == RUW_WORD_COUNT) {
        s_progress.round = s_blob.v1.round;
        memcpy(s_level, s_blob.v1.level, sizeof(s_level));
        memcpy(s_due, s_blob.v1.due, sizeof(s_due));
        ESP_LOGI(TAG, "已从 v1 进度升级");
    } else {
        ESP_LOGI(TAG, "进度格式不匹配,从头开始");
        return;
    }
    for (int i = 0; i < RUW_WORD_COUNT; i++) {
        if (s_level[i] > RUW_MAX_LEVEL) s_level[i] = RUW_MAX_LEVEL;
    }
}

static void progress_save(void) {
    s_blob.v2.version = PROG_VERSION;
    s_blob.v2.count = RUW_WORD_COUNT;
    s_blob.v2.round = s_progress.round;
    s_blob.v2.volume_level = s_volume_level;
    s_blob.v2.filter = s_filter;
    memcpy(s_blob.v2.level, s_level, sizeof(s_level));
    memcpy(s_blob.v2.wrong, s_wrong, sizeof(s_wrong));
    memcpy(s_blob.v2.due, s_due, sizeof(s_due));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_blob.v2, sizeof(s_blob.v2));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "保存进度失败: %s", esp_err_to_name(err));
}

static uint16_t count_wrong(void) {
    uint16_t n = 0;
    for (int i = 0; i < RUW_WORD_COUNT; i++) n += s_wrong[i] > 0;
    return n;
}

// ---------------------------------------------------------------- 音频

#define AUDIO_CHUNK 512

static void audio_task(void *arg) {
    (void)arg;
    static int16_t pcm[AUDIO_CHUNK];
    uint16_t word;
    for (;;) {
        if (xQueueReceive(s_audio_queue, &word, portMAX_DELAY) != pdTRUE) continue;
        s_audio_abort = false;
        const ruw_word_t *w = &RUW_WORDS[word];
        const uint8_t *src = s_voice_start + w->audio_offset;
        if (src + (w->audio_samples + 1) / 2 > s_voice_end) {
            ESP_LOGE(TAG, "音频越界: %u", word);
            continue;
        }
        ruw_adpcm_t st;
        ruw_adpcm_init(&st);
        size_t left = w->audio_samples & ~(size_t)1;
        while (left > 0 && !s_audio_abort) {
            size_t n = left < AUDIO_CHUNK ? left : AUDIO_CHUNK;
            ruw_adpcm_decode(&st, src, n, pcm);
            src += n / 2;
            left -= n;
            if (bsp_audio_write(pcm, n * sizeof(int16_t)) != ESP_OK) break;
        }
        // 尾部补静音,冲掉 DMA 里残留的最后几个采样,避免下一次播放前出现爆音。
        memset(pcm, 0, sizeof(pcm));
        bsp_audio_write(pcm, sizeof(pcm));
    }
}

static void play_word(uint16_t word) {
    if (!s_audio_ok) return;
    s_audio_abort = true;           // 打断正在播放的上一个词
    xQueueReset(s_audio_queue);
    xQueueSend(s_audio_queue, &word, 0);
}

static void apply_volume(void) {
    if (s_audio_ok) bsp_audio_set_volume(VOLUME_STEPS[s_volume_level]);
}

// ---------------------------------------------------------------- 界面工具

static size_t utf8_len(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += ((uint8_t)*s & 0xC0) != 0x80;
    return n;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// 列表行:白底圆角,选中时深色描边。文字左对齐时 align_left=true。
static lv_obj_t *list_row(lv_obj_t *parent, int y, int h, const char *text, bool align_left) {
    uint8_t i = s_row_count++;
    s_rows[i] = box(parent, 18, y, 204, h, COL_CARD, 10);
    lv_obj_set_style_border_width(s_rows[i], 2, 0);
    s_row_labels[i] = label(s_rows[i], &ruw_font_20, COL_INK, text);
    lv_label_set_long_mode(s_row_labels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_row_labels[i], 184);
    lv_obj_set_style_text_align(s_row_labels[i],
        align_left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_row_labels[i]);
    return s_rows[i];
}

static void list_select(uint8_t selected) {
    for (uint8_t i = 0; i < s_row_count; i++) {
        lv_obj_set_style_border_color(s_rows[i],
            lv_color_hex(i == selected ? COL_INK : COL_CARD), 0);
    }
}

static void battery_refresh(void) {
    if (!s_battery) return;
    int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_battery, "");
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", soc);
        lv_obj_set_style_text_color(s_battery,
            lv_color_hex(soc <= 15 ? COL_ACCENT : COL_MUTED), 0);
    }
}

// 新建空白页:背景 + 右上角电量。旧页面在此删除。
static lv_obj_t *page_begin(page_t page) {
    s_page = page;
    lv_obj_t *old = s_scr;
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_row_labels, 0, sizeof(s_row_labels));
    s_row_count = 0;
    s_hint = NULL;

    // 屏幕四角有 30px 圆角遮罩,电量放在右上角但向内收。
    s_battery = label(s_scr, &ruw_font_20, COL_MUTED, "");
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -30, 8);
    battery_refresh();

    lv_screen_load(s_scr);
    if (old) lv_obj_delete(old);
    return s_scr;
}

static const char *filter_name(void) {
    return s_filter == 0 ? "全部" : RUW_CATEGORIES[s_filter - 1];
}

// ---------------------------------------------------------------- 首页

static void home_menu_text(void) {
    lv_label_set_text_fmt(s_row_labels[MENU_START], "开始学习 · %s", filter_name());
    lv_label_set_text_fmt(s_row_labels[MENU_LISTEN], "听音选义 · %s", filter_name());
    lv_label_set_text_fmt(s_row_labels[MENU_CATEGORY], "选择分类：%s", filter_name());
    lv_label_set_text_fmt(s_row_labels[MENU_WRONG], "错题本 · %u 个", count_wrong());
    static const char *const BARS[VOLUME_LEVELS] = {
        "●○○○○", "●●○○○", "●●●○○", "●●●●○", "●●●●●",
    };
    lv_label_set_text_fmt(s_row_labels[MENU_VOLUME], "音量  %s", BARS[s_volume_level]);
}

static void show_home(void) {
    lv_obj_t *scr = page_begin(PAGE_HOME);

    box(scr, 30, 14, 6, 22, COL_ACCENT, 2);
    lv_obj_t *title = label(scr, &ruw_font_20, COL_INK, "俄语单词斩");
    lv_obj_set_pos(title, 42, 10);

    uint16_t learned = ruw_count_learned(&s_progress);
    lv_obj_t *card = box(scr, 16, 44, 208, 86, COL_CARD, 16);
    lv_obj_t *big = label(card, &ruw_font_36, COL_INK, "");
    lv_label_set_text_fmt(big, "%u", learned);
    lv_obj_set_pos(big, 16, 4);
    lv_obj_t *sub = label(card, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(sub, "/ %d 已学", RUW_WORD_COUNT);
    lv_obj_align_to(sub, big, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 176, 6);
    lv_obj_set_pos(bar, 16, 54);
    lv_bar_set_range(bar, 0, RUW_WORD_COUNT);
    lv_bar_set_value(bar, learned, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    lv_obj_t *stats = label(card, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(stats, "复习 %u · 掌握 %u", ruw_count_due(&s_progress),
                          ruw_count_mastered(&s_progress));
    lv_obj_set_pos(stats, 16, 60);

    // 5 个菜单项在 320 高屏内压缩排布:首行 y=134,行距 37,行高 34,末行底边 316。
    for (int i = 0; i < MENU_COUNT; i++) list_row(scr, 134 + i * 37, 34, "", true);
    home_menu_text();
    list_select(s_menu);
}

// ---------------------------------------------------------------- 分类

#define CAT_VISIBLE 6

static void category_refresh(void) {
    // 一屏 6 行,光标超出时整体滚动。
    uint8_t total = RUW_CATEGORY_COUNT + 1;
    uint8_t first = s_cat_cursor < CAT_VISIBLE ? 0 : s_cat_cursor - CAT_VISIBLE + 1;
    for (uint8_t r = 0; r < CAT_VISIBLE; r++) {
        uint8_t item = first + r;
        if (item >= total) {
            lv_obj_add_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[r], LV_OBJ_FLAG_HIDDEN);
        const char *name = item == 0 ? "全部" : RUW_CATEGORIES[item - 1];
        uint16_t words = 0, learned = 0;
        for (int i = 0; i < RUW_WORD_COUNT; i++) {
            if (item == 0 || s_category[i] == item - 1) {
                words++;
                learned += s_level[i] > 0;
            }
        }
        lv_label_set_text_fmt(s_row_labels[r], "%s%s  %u/%u", item == s_filter ? "● " : "",
                              name, learned, words);
    }
    list_select(s_cat_cursor - first);
}

static void show_category(void) {
    lv_obj_t *scr = page_begin(PAGE_CATEGORY);
    lv_obj_t *title = label(scr, &ruw_font_20, COL_INK, "选择分类");
    lv_obj_set_pos(title, 30, 10);
    for (int r = 0; r < CAT_VISIBLE; r++) list_row(scr, 44 + r * 42, 36, "", true);
    s_cat_cursor = s_filter;
    category_refresh();
}

// ---------------------------------------------------------------- 答题

static void quiz_rows_refresh(void) {
    for (int i = 0; i < RUW_OPTIONS; i++) {
        lv_obj_set_style_border_color(s_rows[i],
            lv_color_hex(i == s_selected ? COL_INK : COL_CARD), 0);
    }
}

static uint16_t current_word(void) {
    return s_session.queue[s_answered ? s_session.pos - 1 : s_session.pos];
}

// 俄语单词块:重读元音标红(ru_marked + recolor)+ 拉丁转写。
// 看词模式在出题页就画;听音模式作答前不画,作答后才揭晓。
static void create_ru_block(lv_obj_t *scr, const ruw_word_t *w) {
    bool long_word = utf8_len(w->ru) > 8;
    lv_obj_t *ru = label(scr, long_word ? &ruw_font_28 : &ruw_font_36, COL_INK, "");
    lv_label_set_recolor(ru, true);
    lv_label_set_text(ru, w->ru_marked);
    lv_obj_set_width(ru, 216);
    lv_label_set_long_mode(ru, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ru, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ru, LV_ALIGN_TOP_MID, 0, long_word ? 70 : 66);

    lv_obj_t *tr = label(scr, &ruw_font_20, COL_MUTED, w->translit);
    lv_obj_align(tr, LV_ALIGN_TOP_MID, 0, long_word ? 106 : 112);
}

static void show_question(void) {
    s_answered = false;
    s_selected = 0;
    s_hear_prompt = NULL;
    uint16_t target = s_session.queue[s_session.pos];
    ruw_question_make(&s_question, target, RUW_WORD_COUNT, s_category, &s_seed);

    lv_obj_t *scr = page_begin(PAGE_QUIZ);
    const ruw_word_t *w = &RUW_WORDS[target];

    lv_obj_t *count = label(scr, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(count, "%s %u/%u", s_mode == MODE_WRONG ? "错题" : "",
                          s_session.pos + 1, s_session.len);
    lv_obj_set_pos(count, 30, 8);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 180, 4);
    lv_obj_set_pos(bar, 30, 38);
    lv_bar_set_range(bar, 0, s_session.len);
    lv_bar_set_value(bar, s_session.pos, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    if (s_mode == MODE_LISTEN) {
        // 听音模式:作答前不露俄语/转写,只放一个大"听"提示区。
        s_hear_prompt = box(scr, 16, 62, 208, 60, COL_CARD, 14);
        lv_obj_t *hear = label(s_hear_prompt, &ruw_font_20, COL_ACCENT, "听");
        lv_obj_center(hear);
    } else {
        const char *tag = s_session.requeued[s_session.pos] ? "再考一次"
                        : (s_level[target] == 0 ? "新词" : RUW_CATEGORIES[w->category]);
        lv_obj_t *chip = label(scr, &ruw_font_20, COL_ACCENT, tag);
        lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 44);
        create_ru_block(scr, w);
    }

    for (int i = 0; i < RUW_OPTIONS; i++) {
        list_row(scr, 140 + i * 38, 34, RUW_WORDS[s_question.word[i]].zh, false);
    }
    s_hint = label(scr, &ruw_font_20, COL_MUTED, "长按↑重听");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 292);
    quiz_rows_refresh();
    play_word(target);
}

// 作答后:选项区换成详情卡(对错、词性、完整释义)。
static void show_answer_detail(bool ok) {
    uint16_t target = s_question.word[s_question.answer];
    const ruw_word_t *w = &RUW_WORDS[target];
    for (int i = 0; i < s_row_count; i++) lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = box(s_scr, 16, 138, 208, 150, ok ? COL_OK_BG : COL_BAD_BG, 14);
    lv_obj_t *verdict = label(card, &ruw_font_20, ok ? COL_OK : COL_ACCENT,
                              ok ? "答对了！" : "答错了，已加入错题本");
    lv_obj_set_pos(verdict, 12, 8);

    lv_obj_t *pos = label(card, &ruw_font_20, COL_MUTED, w->pos);
    lv_obj_set_pos(pos, 12, 36);

    lv_obj_t *full = label(card, &ruw_font_20, COL_INK, w->full);
    lv_obj_set_width(full, 184);
    lv_label_set_long_mode(full, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(full, 12, 62);

    if (!ok) {
        lv_obj_t *mine = label(card, &ruw_font_20, COL_MUTED, "");
        lv_label_set_text_fmt(mine, "你选了：%s", RUW_WORDS[s_question.word[s_selected]].zh);
        lv_label_set_long_mode(mine, LV_LABEL_LONG_DOT);
        lv_obj_set_width(mine, 184);
        lv_obj_set_pos(mine, 12, 118);
    }
}

static void submit_answer(void) {
    s_answered = true;
    bool ok = s_selected == s_question.answer;
    uint16_t target = s_session.queue[s_session.pos];

    if (!ok) {
        if (s_wrong[target] < UINT8_MAX) s_wrong[target]++;
    } else if (s_mode == MODE_WRONG && !s_session.requeued[s_session.pos]) {
        s_wrong[target] = 0;  // 错题本里一次答对即移出
    }
    ruw_session_answer(&s_session, &s_progress, ok);
    progress_save();  // 随时关机也不丢进度

    // 听音模式:作答后把大"听"换成俄语单词(重音标红)+转写。
    if (s_mode == MODE_LISTEN) {
        lv_obj_delete(s_hear_prompt);
        s_hear_prompt = NULL;
        create_ru_block(s_scr, &RUW_WORDS[target]);
    }

    lv_label_set_text(s_hint, "↑↓重听 · 确定下一题");
    show_answer_detail(ok);
    play_word(target);
}

// ---------------------------------------------------------------- 本轮完成

static void show_done(void) {
    ruw_session_finish(&s_progress);
    progress_save();

    lv_obj_t *scr = page_begin(PAGE_DONE);
    lv_obj_t *title = label(scr, &ruw_font_20, COL_ACCENT,
                            s_mode == MODE_WRONG ? "错题复习完成" : "本轮完成");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *card = box(scr, 16, 84, 208, 130, COL_CARD, 16);
    uint8_t total = s_session.correct + s_session.wrong;
    lv_obj_t *big = label(card, &ruw_font_36, COL_INK, "");
    lv_label_set_text_fmt(big, "%u/%u", s_session.correct, total);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *sub = label(card, &ruw_font_20, COL_MUTED, "答对");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *nw = label(card, &ruw_font_20, COL_INK, "");
    if (s_mode == MODE_WRONG) {
        lv_label_set_text_fmt(nw, "错题本剩 %u 个", count_wrong());
    } else {
        lv_label_set_text_fmt(nw, "新词 %u · 已学 %u", s_session.new_words,
                              ruw_count_learned(&s_progress));
    }
    lv_obj_align(nw, LV_ALIGN_TOP_MID, 0, 94);

    lv_obj_t *btn = box(scr, 36, 236, 168, 50, COL_INK, 25);
    lv_obj_t *bl = label(btn, &ruw_font_20, COL_CARD, "按确定继续");
    lv_obj_center(bl);
}

static bool start_session(study_mode_t mode) {
    for (int i = 0; i < RUW_WORD_COUNT; i++) {
        s_eligible[i] = mode == MODE_WRONG ? s_wrong[i] > 0
                      : (s_filter == 0 || s_category[i] == s_filter - 1);
    }
    if (ruw_session_build(&s_session, &s_progress, s_eligible) == 0) return false;
    s_mode = mode;
    show_question();
    return true;
}

// ---------------------------------------------------------------- 输入

static void home_input(const input_event_t *in) {
    if (in->ev != BSP_BTN_CLICK) return;
    if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        s_menu = (uint8_t)((s_menu + (in->btn == BSP_BTN_UP ? MENU_COUNT - 1 : 1)) % MENU_COUNT);
        list_select(s_menu);
        return;
    }
    switch (s_menu) {
    case MENU_START:
        start_session(MODE_STUDY);
        break;
    case MENU_LISTEN:
        start_session(MODE_LISTEN);
        break;
    case MENU_CATEGORY:
        show_category();
        break;
    case MENU_WRONG:
        if (!start_session(MODE_WRONG)) {
            lv_label_set_text(s_row_labels[MENU_WRONG], "错题本是空的");
        }
        break;
    case MENU_VOLUME:
        s_volume_level = (uint8_t)((s_volume_level + 1) % VOLUME_LEVELS);
        apply_volume();
        home_menu_text();
        progress_save();
        play_word(0);  // 试听
        break;
    }
}

static void category_input(const input_event_t *in) {
    uint8_t total = RUW_CATEGORY_COUNT + 1;
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        show_home();
    } else if (in->ev != BSP_BTN_CLICK) {
        return;
    } else if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        s_cat_cursor = (uint8_t)((s_cat_cursor + (in->btn == BSP_BTN_UP ? total - 1 : 1)) % total);
        category_refresh();
    } else {
        s_filter = s_cat_cursor;
        progress_save();
        s_menu = MENU_START;
        show_home();
    }
}

static void quiz_input(const input_event_t *in) {
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        show_home();  // 已作答的词进度已保存;本轮不计入轮次
    } else if (in->btn == BSP_BTN_UP && in->ev == BSP_BTN_LONG) {
        play_word(current_word());
    } else if (in->ev != BSP_BTN_CLICK) {
        return;
    } else if (s_answered) {
        if (in->btn == BSP_BTN_OK) {
            if (s_session.pos < s_session.len) show_question();
            else show_done();
        } else {
            play_word(current_word());
        }
    } else if (in->btn == BSP_BTN_UP) {
        s_selected = (uint8_t)((s_selected + RUW_OPTIONS - 1) % RUW_OPTIONS);
        quiz_rows_refresh();
    } else if (in->btn == BSP_BTN_DOWN) {
        s_selected = (uint8_t)((s_selected + 1) % RUW_OPTIONS);
        quiz_rows_refresh();
    } else {
        submit_answer();
    }
}

static void handle_input(const input_event_t *in) {
    switch (s_page) {
    case PAGE_HOME:     home_input(in); break;
    case PAGE_CATEGORY: category_input(in); break;
    case PAGE_QUIZ:     quiz_input(in); break;
    case PAGE_DONE:
        if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_CLICK) show_home();
        break;
    }
}

static void set_screen_on(bool on) {
    s_screen_off = !on;
    bsp_display_backlight(on ? 100 : 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t in;
    int64_t last_battery = now_ms();
    for (;;) {
        bool got = xQueueReceive(s_input_queue, &in, pdMS_TO_TICKS(1000)) == pdTRUE;
        int64_t t = now_ms();
        if (got) {
            bool was_off = s_screen_off;
            s_last_input_ms = t;
            set_screen_on(true);
            if (was_off) continue;  // 唤醒键不触发操作
            if (bsp_lvgl_lock(500)) {
                handle_input(&in);
                bsp_lvgl_unlock();
            }
            continue;
        }
        int64_t idle = t - s_last_input_ms;
        if (!s_screen_off && idle > IDLE_OFF_MS) set_screen_on(false);
        else if (!s_screen_off && idle > IDLE_DIM_MS) bsp_display_backlight(20);
        if (t - last_battery > BATTERY_POLL_MS && bsp_lvgl_lock(200)) {
            battery_refresh();
            bsp_lvgl_unlock();
            last_battery = t;
        }
    }
}

// 按键回调运行在 esp_timer 任务:只入队。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (ev == BSP_BTN_PRESS || ev == BSP_BTN_DOUBLE) return;
    const input_event_t in = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_input_queue, &in, 0);
}

// ---------------------------------------------------------------- 启动

esp_err_t ruwords_app_start(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败,进度不会保存: %s", esp_err_to_name(err));

    for (int i = 0; i < RUW_WORD_COUNT; i++) s_category[i] = RUW_WORDS[i].category;
    s_seed = esp_random();
    progress_load();

    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用,隐藏电量");

    s_audio_ok = bsp_audio_init() == ESP_OK
              && bsp_audio_set_format(RUW_AUDIO_RATE, 16, 1) == ESP_OK;
    if (s_audio_ok) {
        s_audio_queue = xQueueCreate(1, sizeof(uint16_t));
        if (!s_audio_queue
            || xTaskCreate(audio_task, "ruw_audio", 3072, NULL, 4, NULL) != pdPASS) {
            s_audio_ok = false;
        }
    }
    apply_volume();
    if (!s_audio_ok) ESP_LOGE(TAG, "音频不可用,静音运行");

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    show_home();
    bsp_lvgl_unlock();

    s_last_input_ms = now_ms();
    if (xTaskCreate(input_task, "ruw_input", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    return err;
}
