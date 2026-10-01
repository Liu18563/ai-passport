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
typedef enum {
    MENU_START, MENU_LISTEN, MENU_GENDER, MENU_WRONG, MENU_CATEGORY, MENU_VOLUME,
    MENU_COUNT,
} menu_t;
typedef enum { MODE_STUDY, MODE_WRONG, MODE_LISTEN, MODE_GENDER } study_mode_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

static uint8_t s_stage[RUW_WORD_COUNT];       // 词义闯关状态,见 RUW_STAGE_*
static uint16_t s_last_seen[RUW_WORD_COUNT];  // 最后一次考到的关数
static uint8_t s_category[RUW_WORD_COUNT];
static uint8_t s_eligible[RUW_WORD_COUNT];
static ruw_progress_t s_progress = {
    .count = RUW_WORD_COUNT, .level = 0,
    .stage = s_stage, .last_seen = s_last_seen,
};
// 阴阳中性:独立的一套状态,与词义闯关互不影响(只考名词)。
static uint8_t s_gender_stage[RUW_WORD_COUNT];
static uint16_t s_gender_seen[RUW_WORD_COUNT];
static ruw_progress_t s_gender_progress = {
    .count = RUW_WORD_COUNT, .level = 0,
    .stage = s_gender_stage, .last_seen = s_gender_seen,
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
#define PROG_VERSION 3

// v3 存档:词义 + 性别两套状态。
typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t level;            // 词义:已打完的关数
    uint16_t gender_level;     // 性别:已打完的关数
    uint8_t volume_level;
    uint8_t filter;
    uint8_t stage[RUW_WORD_COUNT];
    uint16_t last_seen[RUW_WORD_COUNT];
    uint8_t gender_stage[RUW_WORD_COUNT];
    uint16_t gender_seen[RUW_WORD_COUNT];
} saved_progress_t;

// v2(旧 Leitner 存档):保留定义,供「存储迁移」块做 v2→v3 迁移。
typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t round;
    uint8_t volume_level;
    uint8_t filter;
    uint8_t level[RUW_WORD_COUNT];
    uint8_t wrong[RUW_WORD_COUNT];
    uint16_t due[RUW_WORD_COUNT];
} saved_progress_v2_t;

static union {
    saved_progress_t v3;
    saved_progress_v2_t v2;
} s_blob;  // 静态:避免占 input 任务栈

static void progress_load(void) {
    ruw_progress_reset(&s_progress);
    ruw_progress_reset(&s_gender_progress);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &s_blob, &len);
    nvs_close(h);
    if (err != ESP_OK) return;

    // v2→v3 按原文迁移在「存储迁移」块实现;本块只认 v3,旧格式从头开始。
    if (len == sizeof(saved_progress_t) && s_blob.v3.version == PROG_VERSION
        && s_blob.v3.count == RUW_WORD_COUNT) {
        s_progress.level = s_blob.v3.level;
        s_gender_progress.level = s_blob.v3.gender_level;
        memcpy(s_stage, s_blob.v3.stage, sizeof(s_stage));
        memcpy(s_last_seen, s_blob.v3.last_seen, sizeof(s_last_seen));
        memcpy(s_gender_stage, s_blob.v3.gender_stage, sizeof(s_gender_stage));
        memcpy(s_gender_seen, s_blob.v3.gender_seen, sizeof(s_gender_seen));
        if (s_blob.v3.volume_level < VOLUME_LEVELS) s_volume_level = s_blob.v3.volume_level;
        if (s_blob.v3.filter <= RUW_CATEGORY_COUNT) s_filter = s_blob.v3.filter;
    } else {
        ESP_LOGI(TAG, "进度格式不匹配,从头开始");
        return;
    }
    for (int i = 0; i < RUW_WORD_COUNT; i++) {
        if (s_stage[i] > RUW_STAGE_WRONG1) s_stage[i] = RUW_STAGE_NEW;
        if (s_gender_stage[i] > RUW_STAGE_WRONG1) s_gender_stage[i] = RUW_STAGE_NEW;
    }
}

static void progress_save(void) {
    s_blob.v3.version = PROG_VERSION;
    s_blob.v3.count = RUW_WORD_COUNT;
    s_blob.v3.level = s_progress.level;
    s_blob.v3.gender_level = s_gender_progress.level;
    s_blob.v3.volume_level = s_volume_level;
    s_blob.v3.filter = s_filter;
    memcpy(s_blob.v3.stage, s_stage, sizeof(s_stage));
    memcpy(s_blob.v3.last_seen, s_last_seen, sizeof(s_last_seen));
    memcpy(s_blob.v3.gender_stage, s_gender_stage, sizeof(s_gender_stage));
    memcpy(s_blob.v3.gender_seen, s_gender_seen, sizeof(s_gender_seen));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_blob.v3, sizeof(s_blob.v3));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "保存进度失败: %s", esp_err_to_name(err));
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
    lv_label_set_text_fmt(s_row_labels[MENU_START], "闯关学习 · %s", filter_name());
    lv_label_set_text_fmt(s_row_labels[MENU_LISTEN], "听音选义 · %s", filter_name());
    lv_label_set_text_fmt(s_row_labels[MENU_GENDER], "阴阳中性 · 已掌握 %u",
                          ruw_count_mastered(&s_gender_progress));
    lv_label_set_text_fmt(s_row_labels[MENU_WRONG], "错题本 · %u 个",
                          ruw_count_wrong(&s_progress));
    lv_label_set_text_fmt(s_row_labels[MENU_CATEGORY], "选择分类：%s", filter_name());
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

    uint16_t mastered = ruw_count_mastered(&s_progress);
    lv_obj_t *card = box(scr, 16, 44, 208, 86, COL_CARD, 16);
    lv_obj_t *big = label(card, &ruw_font_36, COL_INK, "");
    lv_label_set_text_fmt(big, "%u", mastered);
    lv_obj_set_pos(big, 16, 4);
    lv_obj_t *sub = label(card, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(sub, "/ %d 已掌握", RUW_WORD_COUNT);
    lv_obj_align_to(sub, big, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -6);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 176, 6);
    lv_obj_set_pos(bar, 16, 54);
    lv_bar_set_range(bar, 0, RUW_WORD_COUNT);
    lv_bar_set_value(bar, mastered, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    lv_obj_t *stats = label(card, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(stats, "新词 %u · 错词 %u", ruw_count_new(&s_progress),
                          ruw_count_wrong(&s_progress));
    lv_obj_set_pos(stats, 16, 60);

    // 6 个菜单项压缩排布:首行 y=132,行距 30,行高 28,末行底边 310。
    for (int i = 0; i < MENU_COUNT; i++) list_row(scr, 132 + i * 30, 28, "", true);
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
                learned += s_stage[i] != RUW_STAGE_NEW;
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

// 阴阳中性题面:白色大卡片,名词(重音标红)+ 转写 + 一行中文小字,右上角小标签。
static void create_gender_block(lv_obj_t *scr, const ruw_word_t *w, const char *tag) {
    bool long_word = utf8_len(w->ru) > 8;
    lv_obj_t *card = box(scr, 16, 44, 208, 108, COL_CARD, 14);

    lv_obj_t *chip = label(card, &ruw_font_20, COL_ACCENT, tag);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -8, 6);

    lv_obj_t *ru = label(card, long_word ? &ruw_font_28 : &ruw_font_36, COL_INK, "");
    lv_label_set_recolor(ru, true);
    lv_label_set_text(ru, w->ru_marked);
    lv_obj_set_width(ru, 190);
    lv_label_set_long_mode(ru, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ru, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ru, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *tr = label(card, &ruw_font_20, COL_MUTED, w->translit);
    lv_obj_align(tr, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *zh = label(card, &ruw_font_20, COL_MUTED, w->zh);
    lv_obj_align(zh, LV_ALIGN_TOP_MID, 0, 82);
}

static void show_question(void) {
    s_answered = false;
    s_selected = 0;
    s_hear_prompt = NULL;
    uint16_t target = s_session.queue[s_session.pos];
    bool gender_mode = s_mode == MODE_GENDER;

    // 性别模式选项固定顺序,正确位置即性别编码;其余模式随机抽干扰项。
    if (gender_mode) s_question.answer = RUW_WORDS[target].gender;
    else ruw_question_make(&s_question, target, RUW_WORD_COUNT, s_category, &s_seed);

    lv_obj_t *scr = page_begin(PAGE_QUIZ);
    const ruw_word_t *w = &RUW_WORDS[target];

    static const char *const MODE_NAME[4] = { "闯关", "错题", "听音", "阴阳中性" };
    lv_obj_t *count = label(scr, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(count, "%s %u/%u", MODE_NAME[s_mode],
                          s_session.pos + 1, s_session.len);
    lv_obj_set_pos(count, 30, 8);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 180, 4);
    lv_obj_set_pos(bar, 30, 34);
    lv_bar_set_range(bar, 0, s_session.len);
    lv_bar_set_value(bar, s_session.pos, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    // 小标签:新词 / 错词 / 再考一次。
    uint8_t tag_stage = gender_mode ? s_gender_stage[target] : s_stage[target];
    const char *tag = s_session.requeued[s_session.pos] ? "再考一次"
                    : (tag_stage == RUW_STAGE_NEW ? "新词" : "错词");

    if (gender_mode) {
        create_gender_block(scr, w, tag);
    } else if (s_mode == MODE_LISTEN) {
        // 听音模式:作答前不露俄语/转写,只放一个大"听"提示区。
        lv_obj_t *chip = label(scr, &ruw_font_20, COL_ACCENT, tag);
        lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 44);
        s_hear_prompt = box(scr, 16, 62, 208, 60, COL_CARD, 14);
        lv_obj_t *hear = label(s_hear_prompt, &ruw_font_20, COL_ACCENT, "听");
        lv_obj_center(hear);
    } else {
        lv_obj_t *chip = label(scr, &ruw_font_20, COL_ACCENT, tag);
        lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 44);
        create_ru_block(scr, w);
    }

    static const char *const GENDER_OPT[RUW_OPTIONS] = {
        "阳性", "阴性", "中性", "只有复数",
    };
    for (int i = 0; i < RUW_OPTIONS; i++) {
        const char *text = gender_mode ? GENDER_OPT[i]
                           : RUW_WORDS[s_question.word[i]].zh;
        list_row(scr, 158 + i * 33, 30, text, false);
    }
    s_hint = label(scr, &ruw_font_20, COL_MUTED,
                   gender_mode ? "▲▼ 选择 · ● 确定" : "长按↑重听");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 294);
    quiz_rows_refresh();
    play_word(target);
}

// 作答后:选项区换成详情卡。词义模式显示词性+完整释义;
// 性别模式显示 ruw_gender_hint 的判断提示。
static void show_answer_detail(bool ok) {
    bool gender_mode = s_mode == MODE_GENDER;
    uint16_t target = current_word();
    const ruw_word_t *w = &RUW_WORDS[target];
    for (int i = 0; i < s_row_count; i++) lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = box(s_scr, 16, 138, 208, 150, ok ? COL_OK_BG : COL_BAD_BG, 14);
    lv_obj_t *verdict = label(card, &ruw_font_20, ok ? COL_OK : COL_ACCENT,
                              ok ? "答对了！" : "答错了，已加入错题本");
    lv_obj_set_pos(verdict, 12, 8);

    if (gender_mode) {
        const char *hint = ruw_gender_hint(w->ru, w->gender);
        lv_obj_t *h = label(card, &ruw_font_20, COL_INK, hint);
        lv_obj_set_width(h, 184);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(h, 12, 36);
    } else {
        lv_obj_t *pos = label(card, &ruw_font_20, COL_MUTED, w->pos);
        lv_obj_set_pos(pos, 12, 36);

        lv_obj_t *full = label(card, &ruw_font_20, COL_INK, w->full);
        lv_obj_set_width(full, 184);
        lv_label_set_long_mode(full, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(full, 12, 62);
    }

    if (!ok) {
        static const char *const GENDER_OPT[RUW_OPTIONS] = {
            "阳性", "阴性", "中性", "只有复数",
        };
        lv_obj_t *mine = label(card, &ruw_font_20, COL_MUTED, "");
        if (gender_mode)
            lv_label_set_text_fmt(mine, "你选了：%s", GENDER_OPT[s_selected]);
        else
            lv_label_set_text_fmt(mine, "你选了：%s",
                                  RUW_WORDS[s_question.word[s_selected]].zh);
        lv_label_set_long_mode(mine, LV_LABEL_LONG_DOT);
        lv_obj_set_width(mine, 184);
        lv_obj_set_pos(mine, 12, 118);
    }
}

static void submit_answer(void) {
    s_answered = true;
    bool ok = s_selected == s_question.answer;
    uint16_t target = s_session.queue[s_session.pos];

    // 状态转移完全由纯逻辑处理(含重排题不改状态、答错重排)。
    // 性别模式写独立的一套状态,与词义互不影响。
    ruw_progress_t *prog = s_mode == MODE_GENDER ? &s_gender_progress : &s_progress;
    ruw_session_answer(&s_session, prog, ok);
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
    bool gender_mode = s_mode == MODE_GENDER;
    ruw_progress_t *prog = gender_mode ? &s_gender_progress : &s_progress;
    ruw_session_finish(prog);
    progress_save();

    lv_obj_t *scr = page_begin(PAGE_DONE);
    static const char *const DONE_TITLE[4] = {
        "本关完成", "错题复习完成", "本关完成", "阴阳中性本关完成",
    };
    lv_obj_t *title = label(scr, &ruw_font_20, COL_ACCENT, DONE_TITLE[s_mode]);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

    lv_obj_t *card = box(scr, 16, 84, 208, 130, COL_CARD, 16);
    uint8_t total = s_session.correct + s_session.wrong;
    lv_obj_t *big = label(card, &ruw_font_36, COL_INK, "");
    lv_label_set_text_fmt(big, "%u/%u", s_session.correct, total);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *sub = label(card, &ruw_font_20, COL_MUTED, "答对");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *nw = label(card, &ruw_font_20, COL_INK, "");
    lv_label_set_text_fmt(nw, "新词 %u · 已掌握 %u", s_session.new_words,
                          ruw_count_mastered(prog));
    lv_obj_align(nw, LV_ALIGN_TOP_MID, 0, 94);

    lv_obj_t *btn = box(scr, 36, 236, 168, 50, COL_INK, 25);
    lv_obj_t *bl = label(btn, &ruw_font_20, COL_CARD, "按确定继续");
    lv_obj_center(bl);
}

static bool start_session(study_mode_t mode) {
    for (int i = 0; i < RUW_WORD_COUNT; i++) {
        if (mode == MODE_WRONG) {
            s_eligible[i] = s_stage[i] == RUW_STAGE_WRONG1
                         || s_stage[i] == RUW_STAGE_WRONG2;
        } else if (mode == MODE_GENDER) {
            // 只考名词(性别非空),并按分类筛选。
            s_eligible[i] = RUW_WORDS[i].gender != RUW_GENDER_NONE
                         && (s_filter == 0 || s_category[i] == s_filter - 1);
        } else {
            s_eligible[i] = s_filter == 0 || s_category[i] == s_filter - 1;
        }
    }
    ruw_progress_t *prog = mode == MODE_GENDER ? &s_gender_progress : &s_progress;
    if (ruw_session_build(&s_session, prog, s_eligible) == 0) return false;
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
    case MENU_GENDER:
        if (!start_session(MODE_GENDER)) {
            lv_label_set_text(s_row_labels[MENU_GENDER], "阴阳中性已通关");
        }
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
