// main/ruwords_app.c —— 俄语单词斩:看俄语(重音标红+拉丁转写)、听发音、四选一中文释义。
//
// 页面(三级菜单):首页(继续挑战/新挑战/错题本/设置)→ 分类页 → 模式页 → 答题 → 本关完成。
// 按键:
//   首页      上/下 = 移动(循环);确定 = 进入;确定 长按 = 设置页
//   分类页    上/下 = 移动(循环);确定 = 选定分类进模式页;确定 长按 = 回首页
//   模式页    上/下 = 移动(循环);确定 = 开始该模式;确定 长按 = 回分类页
//   设置页    上/下 = 移动;确定 = 执行(音量/重置);确定 长按 = 回首页
//   答题      上/下 = 移动选项;确定 = 提交;上 长按 = 重听;确定 长按 = 回模式页(错题本回首页)
//             (听音模式:答题前只显示大"听"区,作答后才揭晓俄语/转写/释义)
//   作答后    上/下 = 重听;确定 = 下一题;确定 长按 = 回模式页(错题本回首页)
//   本轮完成  确定 = 回模式页(错题本回首页)
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
LV_FONT_DECLARE(ruw_font_48);
LV_IMAGE_DECLARE(ruw_logo_f0);
LV_IMAGE_DECLARE(ruw_logo_f1);
LV_IMAGE_DECLARE(ruw_logo_f2);
LV_IMAGE_DECLARE(ruw_logo_f3);
LV_IMAGE_DECLARE(ruw_logo_f4);
LV_IMAGE_DECLARE(ruw_logo_f5);
LV_IMAGE_DECLARE(ruw_logo_f6);
LV_IMAGE_DECLARE(ruw_logo_f7);
LV_IMAGE_DECLARE(ruw_logo_f8);
LV_IMAGE_DECLARE(ruw_logo_f9);
LV_IMAGE_DECLARE(ruw_logo_f10);
LV_IMAGE_DECLARE(ruw_logo_f11);

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
typedef enum { PAGE_HOME, PAGE_CATEGORY, PAGE_MODES, PAGE_QUIZ, PAGE_DONE, PAGE_SETTINGS } page_t;
typedef enum {
    SET_VOLUME, SET_RESET, SET_COUNT,
} settings_t;
typedef enum { MODE_STUDY, MODE_WRONG, MODE_LISTEN, MODE_GENDER } study_mode_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

static uint8_t s_stage[RUW_WORD_COUNT];       // 词义闯关状态,见 RUW_STAGE_*
static uint16_t s_last_seen[RUW_WORD_COUNT];  // 最后一次考到的关数
static uint8_t s_category[RUW_WORD_COUNT];
static uint8_t s_gender[RUW_WORD_COUNT];      // 每词性别 RUW_GENDER_*,供纯统计函数用
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
static uint8_t s_settings_cursor;
static uint8_t s_reset_confirm;  // 重置进度第一次确定后置 1
static uint8_t s_home_cursor;    // 首页光标(继续挑战/新挑战/错题本)
static uint8_t s_cat_cursor;     // 分类页光标
static uint8_t s_mode_cursor;    // 模式页光标
static uint8_t s_mode_items[4];  // 模式页实际显示的模式(本类无名词时辨性别不出现)
static uint8_t s_mode_item_count;
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
static bool s_ans_ok;             // 作答卡分页:本轮答对/错
static uint8_t s_example_page;    // 作答卡分页:0=释义,1=例句
static lv_obj_t *s_ans_card;      // 作答详情卡(翻页时重建)

// 首页 Logo / 开机动画 / 眨眼。
static lv_obj_t *s_home_logo;
static lv_obj_t *s_home_title;    // 应用名「SamL」
static lv_obj_t *s_home_sub;      // 副标题
static lv_timer_t *s_intro_timer;
static lv_timer_t *s_blink_timer;
static bool s_intro_played;
static int64_t s_intro_start_ms;
static uint8_t s_intro_frame;
static uint8_t s_blink_step;      // 眨眼 timer 进行到第几步,见 BLINK_SEQ

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

// v2(旧 Leitner 存档,301 词):保留定义,供 v2→v3 迁移。
// 数组长度必须用旧词数 301,不能用当前 RUW_WORD_COUNT,否则 blob 大小对不上。
#define RUW_LEGACY_COUNT 301
typedef struct {
    uint16_t version;
    uint16_t count;
    uint16_t round;
    uint8_t volume_level;
    uint8_t filter;
    uint8_t level[RUW_LEGACY_COUNT];
    uint8_t wrong[RUW_LEGACY_COUNT];
    uint16_t due[RUW_LEGACY_COUNT];
} saved_progress_v2_t;

static union {
    saved_progress_t v3;
    saved_progress_v2_t v2;
} s_blob;  // 静态:避免占 input 任务栈

// ---- 断点续挑战:独立 NVS key "sess",不改主存档结构与 PROG_VERSION ----
#define NVS_SESS_KEY "sess"
#define SESS_VERSION 1

// v4.4:词表修正版本。第 916 个词替换后,用它把该位置的旧学习状态清零,只做一次。
#define NVS_WLVER_KEY "wlver"
#define WLVER_CURRENT 1
#define WLVER_FIXED_IDX 916

typedef struct {
    uint16_t version;       // 1
    uint16_t word_count;     // = RUW_WORD_COUNT,不一致视为无效
    uint8_t  valid;          // 1 = 有一关打到一半
    uint8_t  filter;         // 分类(valid==0 也保留)
    uint8_t  mode;           // study_mode_t(valid==0 也保留)
    ruw_session_t session;   // 整个当前关的队列与进度
} saved_session_t;

static saved_session_t s_sess;
static bool s_sess_present;  // NVS 里确实有一份版本/词数匹配的 sess

// 外部映射表(ruwords_legacy_map.c):旧 v2 下标 → 新 v3 下标,按原文构建。
extern const uint16_t RUW_LEGACY_MAP[RUW_LEGACY_COUNT];

static void progress_save(void);  // 迁移后立即落盘 v3
static void sess_load(void);
static void sess_save(void);
static void show_category(void);
static void show_modes(bool resume_cursor);
static void show_question(void);
static bool start_session(study_mode_t mode);

static void progress_load(void) {
    ruw_progress_reset(&s_progress);
    ruw_progress_reset(&s_gender_progress);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &s_blob, &len);
    nvs_close(h);
    if (err != ESP_OK) return;

    if (len == sizeof(saved_progress_t) && s_blob.v3.version == PROG_VERSION
        && s_blob.v3.count == RUW_WORD_COUNT) {
        // 当前 v3 存档。
        s_progress.level = s_blob.v3.level;
        s_gender_progress.level = s_blob.v3.gender_level;
        memcpy(s_stage, s_blob.v3.stage, sizeof(s_stage));
        memcpy(s_last_seen, s_blob.v3.last_seen, sizeof(s_last_seen));
        memcpy(s_gender_stage, s_blob.v3.gender_stage, sizeof(s_gender_stage));
        memcpy(s_gender_seen, s_blob.v3.gender_seen, sizeof(s_gender_seen));
        if (s_blob.v3.volume_level < VOLUME_LEVELS) s_volume_level = s_blob.v3.volume_level;
        if (s_blob.v3.filter <= RUW_CATEGORY_COUNT) s_filter = s_blob.v3.filter;
        for (int i = 0; i < RUW_WORD_COUNT; i++) {
            if (s_stage[i] > RUW_STAGE_WRONG1) s_stage[i] = RUW_STAGE_NEW;
            if (s_gender_stage[i] > RUW_STAGE_WRONG1) s_gender_stage[i] = RUW_STAGE_NEW;
        }
    } else if (len == sizeof(saved_progress_v2_t) && s_blob.v2.version == 2
               && s_blob.v2.count == RUW_LEGACY_COUNT) {
        // v2 旧存档:按俄语原文迁移词义状态(性别无,保持 NEW),随后落盘为 v3。
        ESP_LOGI(TAG, "检测到 v2 存档,按原文迁移到 v3");
        ruw_migrate_v2(s_stage, RUW_WORD_COUNT, RUW_LEGACY_MAP, RUW_LEGACY_COUNT,
                       s_blob.v2.level, s_blob.v2.wrong);
        if (s_blob.v2.volume_level < VOLUME_LEVELS) s_volume_level = s_blob.v2.volume_level;
        // v2 分类编号与 v3 顺序不同,旧 filter 指错分类,迁移后一律从「全部」开始。
        s_filter = 0;
        progress_save();
    } else {
        // 未知版本/大小:从头开始,不崩溃。
        ESP_LOGI(TAG, "进度格式不匹配(version=%u,len=%u),从头开始",
                 s_blob.v3.version, (unsigned)len);
        return;
    }

    // v4.4:第 916 个词(原 прийтись/落到)换成 подождать。旧存档里这个位置的词义/性别
    // 状态会落到新词上,读到 wlver<1 时把它重置为初值,并写 wlver=1,只做一次。
    {
        nvs_handle_t hw;
        uint16_t wlver = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &hw) == ESP_OK) {
            if (nvs_get_u16(hw, NVS_WLVER_KEY, &wlver) != ESP_OK) wlver = 0;
            nvs_close(hw);
        }
        if (wlver < WLVER_CURRENT) {
            s_stage[WLVER_FIXED_IDX] = RUW_STAGE_NEW;
            s_last_seen[WLVER_FIXED_IDX] = 0;
            s_gender_stage[WLVER_FIXED_IDX] = RUW_STAGE_NEW;
            s_gender_seen[WLVER_FIXED_IDX] = 0;
            if (nvs_open(NVS_NS, NVS_READWRITE, &hw) == ESP_OK) {
                nvs_set_u16(hw, NVS_WLVER_KEY, WLVER_CURRENT);
                nvs_commit(hw);
                nvs_close(hw);
            }
        }
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

static void sess_load(void) {
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess_present = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_sess);
    esp_err_t err = nvs_get_blob(h, NVS_SESS_KEY, &s_sess, &len);
    nvs_close(h);
    if (err != ESP_OK) return;
    if (len == sizeof(s_sess) && s_sess.version == SESS_VERSION
        && s_sess.word_count == RUW_WORD_COUNT) {
        s_sess_present = true;
    } else {
        memset(&s_sess, 0, sizeof(s_sess));  // 版本/词数不符,当作无记录
    }
}

static void sess_save(void) {
    s_sess.version = SESS_VERSION;
    s_sess.word_count = RUW_WORD_COUNT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_SESS_KEY, &s_sess, sizeof(s_sess));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "保存断点失败: %s", esp_err_to_name(err));
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
    // 换屏前先停首页的动画/眨眼 timer,防止回调访问已删除对象。
    if (s_intro_timer) { lv_timer_delete(s_intro_timer); s_intro_timer = NULL; }
    if (s_blink_timer) { lv_timer_delete(s_blink_timer); s_blink_timer = NULL; }
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

// ---------------------------------------------------------------- 一级:首页(开机落地页)

#define HOME_VISIBLE 3

static void home_rows_text(void) {
    ruw_category_stats_t all;
    ruw_category_stats(&all, 0, RUW_WORD_COUNT, s_category, s_gender,
                       s_stage, s_gender_stage);
    bool can_resume = s_sess_present && s_sess.valid == 1
                   && ruw_session_valid(&s_sess.session, RUW_WORD_COUNT);
    lv_label_set_text(s_row_labels[0], can_resume ? "继续挑战" : "继续挑战（无记录）");
    lv_label_set_text(s_row_labels[1], "新挑战");
    if (all.wrong > 0)
        lv_label_set_text_fmt(s_row_labels[2], "错题本 · %u 个", all.wrong);
    else
        lv_label_set_text(s_row_labels[2], "错题本 · 没有错题");
}

// 继续挑战按确定:有效断点就恢复;否则用上次的分类/方向开新关;都没有则进分类页。
static void continue_challenge(void) {
    if (s_sess_present && s_sess.valid == 1
        && ruw_session_valid(&s_sess.session, RUW_WORD_COUNT)) {
        s_filter = s_sess.filter;
        s_mode = (study_mode_t)s_sess.mode;
        memcpy(&s_session, &s_sess.session, sizeof(s_session));
        show_question();
        return;
    }
    if (s_sess_present) {
        s_filter = s_sess.filter;
        if (start_session((study_mode_t)s_sess.mode)) return;
    }
    s_cat_cursor = s_filter;
    show_category();
}

static const lv_image_dsc_t *const LOGO_FRAMES[RUW_LOGO_FRAMES] = {
    &ruw_logo_f0, &ruw_logo_f1, &ruw_logo_f2, &ruw_logo_f3, &ruw_logo_f4, &ruw_logo_f5,
    &ruw_logo_f6, &ruw_logo_f7, &ruw_logo_f8, &ruw_logo_f9, &ruw_logo_f10, &ruw_logo_f11,
};

// Logo 72x96。首页最终位置与开机动画起点(屏幕正中)。
#define LOGO_W        72
#define LOGO_H        96
#define LOGO_X        84
#define LOGO_Y        8
#define LOGO_INTRO_Y  112
#define TITLE_Y       104
#define TITLE_INTRO_Y 212
#define SUB_Y         150

// 开机动画时间线(ms):12 帧播完后 SamL 淡入,再一起上移,最后其余元素淡入。
#define INTRO_TITLE_FADE_AT  1100
#define INTRO_MOVE_AT        1400
#define INTRO_REST_FADE_AT   1750
#define INTRO_END_AT         2100

// 位移动画回调:把 anim 值写到对象 y。
static void anim_y_cb(void *var, int32_t v) { lv_obj_set_y(var, v); }
// 淡入动画回调:把 anim 值写到对象不透明度。
static void anim_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa(var, v, 0);
}

// 首页最终版面:Logo(84,8)、SamL 居中 y=104、副标题 y=150、菜单 y=180。
static void home_finish_layout(void) {
    lv_image_set_src(s_home_logo, LOGO_FRAMES[RUW_LOGO_REST]);
    lv_obj_set_pos(s_home_logo, LOGO_X, LOGO_Y);
    lv_obj_set_y(s_home_title, TITLE_Y);
    lv_obj_set_y(s_home_sub, SUB_Y);
    lv_obj_set_style_opa(s_home_title, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_home_sub, LV_OPA_COVER, 0);
    for (uint8_t i = 0; i < s_row_count; i++) {
        lv_obj_set_style_opa(s_rows[i], LV_OPA_COVER, 0);
        lv_obj_set_style_opa(s_row_labels[i], LV_OPA_COVER, 0);
    }
    if (s_battery) lv_obj_set_style_opa(s_battery, LV_OPA_COVER, 0);
}

// 眨眼:半闭 → 闭 → 半闭 → 睁开,每步停留时长见 BLINK_MS;做完等 5-8 秒再来。
static const uint8_t BLINK_SEQ[] = {
    RUW_LOGO_HALF_BLINK, RUW_LOGO_BLINK, RUW_LOGO_HALF_BLINK, RUW_LOGO_REST,
};
static const uint16_t BLINK_MS[] = { 60, 110, 60 };
#define BLINK_STEPS (sizeof(BLINK_SEQ) / sizeof(BLINK_SEQ[0]))

static void blink_timer_cb(lv_timer_t *t) {
    if (s_screen_off) { lv_timer_set_period(t, 1000); return; }
    lv_image_set_src(s_home_logo, LOGO_FRAMES[BLINK_SEQ[s_blink_step]]);
    if (s_blink_step + 1 < BLINK_STEPS) {
        lv_timer_set_period(t, BLINK_MS[s_blink_step]);
        s_blink_step++;
    } else {
        s_blink_step = 0;
        lv_timer_set_period(t, 5000 + esp_random() % 3001);
    }
}

static void home_blink_start(void) {
    if (s_blink_timer) lv_timer_delete(s_blink_timer);
    s_blink_step = 0;
    s_blink_timer = lv_timer_create(blink_timer_cb, 300, NULL);  // 进门 300ms 先眨一次
}

// 开机动画 timer:按纯时间线切 Logo 帧,到 INTRO_END_AT 收尾。
static void intro_timer_cb(lv_timer_t *t) {
    uint32_t ms = (uint32_t)(now_ms() - s_intro_start_ms);
    uint8_t f = ruw_intro_frame_at(ms);
    if (f != s_intro_frame) {
        s_intro_frame = f;
        lv_image_set_src(s_home_logo, LOGO_FRAMES[f]);
    }
    if (ms >= INTRO_END_AT) {
        lv_timer_delete(t);
        s_intro_timer = NULL;
        home_finish_layout();
        s_intro_played = true;
        home_blink_start();
    }
}

// 开机第一次进首页:播放开机动画。
static void home_play_intro(lv_obj_t *scr) {
    (void)scr;
    // 只显示 Logo 在屏幕正中,其余元素透明。
    lv_image_set_src(s_home_logo, LOGO_FRAMES[0]);
    lv_obj_set_pos(s_home_logo, LOGO_X, LOGO_INTRO_Y);
    lv_obj_set_y(s_home_title, TITLE_INTRO_Y);
    lv_obj_set_style_opa(s_home_title, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_home_sub, LV_OPA_TRANSP, 0);
    for (uint8_t i = 0; i < s_row_count; i++) {
        lv_obj_set_style_opa(s_rows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(s_row_labels[i], LV_OPA_TRANSP, 0);
    }
    if (s_battery) lv_obj_set_style_opa(s_battery, LV_OPA_TRANSP, 0);

    // 12 帧播完后,SamL 在 Logo 下方淡入。
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_home_title);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, INTRO_MOVE_AT - INTRO_TITLE_FADE_AT - 50);
    lv_anim_set_delay(&a, INTRO_TITLE_FADE_AT);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_start(&a);

    // Logo 与 SamL 一起上移到最终位置,ease_out。
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_home_logo);
    lv_anim_set_values(&a, LOGO_INTRO_Y, LOGO_Y);
    lv_anim_set_time(&a, INTRO_REST_FADE_AT - INTRO_MOVE_AT);
    lv_anim_set_delay(&a, INTRO_MOVE_AT);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s_home_title);
    lv_anim_set_values(&a, TITLE_INTRO_Y, TITLE_Y);
    lv_anim_set_time(&a, INTRO_REST_FADE_AT - INTRO_MOVE_AT);
    lv_anim_set_delay(&a, INTRO_MOVE_AT);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_start(&a);

    // 最后:副标题、菜单、电量淡入。
    lv_obj_t *fade_objs[8];
    fade_objs[0] = s_home_sub;
    uint8_t n = 1;
    for (uint8_t i = 0; i < s_row_count; i++) {
        fade_objs[n++] = s_rows[i];
        fade_objs[n++] = s_row_labels[i];
    }
    if (s_battery) fade_objs[n++] = s_battery;
    for (uint8_t i = 0; i < n; i++) {
        lv_anim_init(&a);
        lv_anim_set_var(&a, fade_objs[i]);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, INTRO_END_AT - INTRO_REST_FADE_AT);
        lv_anim_set_delay(&a, INTRO_REST_FADE_AT);
        lv_anim_set_exec_cb(&a, anim_opa_cb);
        lv_anim_start(&a);
    }

    s_intro_frame = 255;
    s_intro_start_ms = now_ms();
    s_intro_timer = lv_timer_create(intro_timer_cb, 30, NULL);
}

static void show_home(void) {
    lv_obj_t *scr = page_begin(PAGE_HOME);

    s_home_logo = lv_image_create(scr);
    lv_obj_set_size(s_home_logo, LOGO_W, LOGO_H);

    // 应用名「SamL」ruw_font_36 水平居中。
    s_home_title = label(scr, &ruw_font_36, COL_INK, "SamL");
    lv_obj_align(s_home_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    s_home_sub = label(scr, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(s_home_sub, "俄语单词斩 · %u/%u",
                          ruw_count_mastered(&s_progress), RUW_WORD_COUNT);
    lv_obj_align(s_home_sub, LV_ALIGN_TOP_MID, 0, SUB_Y);

    // 3 行菜单:首行 y=180,行距 42,行高 36。
    for (int r = 0; r < HOME_VISIBLE; r++) list_row(scr, 180 + r * 42, 36, "", true);
    home_rows_text();
    list_select(s_home_cursor);

    if (!s_intro_played) home_play_intro(scr);
    else home_blink_start();
}

// ---------------------------------------------------------------- 二级:分类页

#define CAT_VISIBLE 5

static void category_refresh(void) {
    // 一屏 5 行,光标超出时整体滚动。
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
        ruw_category_stats_t st;
        ruw_category_stats(&st, item, RUW_WORD_COUNT, s_category, s_gender,
                           s_stage, s_gender_stage);
        lv_label_set_text_fmt(s_row_labels[r], "%s  %u/%u", name, st.mastered, st.words);
    }
    list_select(s_cat_cursor - first);
}

static void show_category(void) {
    lv_obj_t *scr = page_begin(PAGE_CATEGORY);

    // 顶部:红色竖条 + 标题(保持原位置),右上角电量。
    box(scr, 30, 14, 6, 22, COL_ACCENT, 2);
    lv_obj_t *title = label(scr, &ruw_font_20, COL_INK, "俄语单词斩");
    lv_obj_set_pos(title, 42, 10);

    // 标题下小字:已掌握 X/1300 · 错词 Y。
    ruw_category_stats_t all;
    ruw_category_stats(&all, 0, RUW_WORD_COUNT, s_category, s_gender,
                       s_stage, s_gender_stage);
    lv_obj_t *sub = label(scr, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(sub, "已掌握 %u/%d · 错词 %u",
                          all.mastered, RUW_WORD_COUNT, all.wrong);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 40);

    // 19 项分类,首行 y=72、行距 44、行高 38,一屏 5 行。
    for (int r = 0; r < CAT_VISIBLE; r++) list_row(scr, 72 + r * 44, 38, "", true);
    s_cat_cursor = s_filter;  // 停在上次选过的分类
    category_refresh();

    lv_obj_t *hint = label(scr, &ruw_font_20, COL_MUTED, "长按确定：返回");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 292);
}

// ---------------------------------------------------------------- 二级:模式页(只学当前分类)

static void modes_rows_text(void) {
    ruw_category_stats_t st;
    ruw_category_stats(&st, s_filter, RUW_WORD_COUNT, s_category, s_gender,
                       s_stage, s_gender_stage);
    for (uint8_t i = 0; i < s_mode_item_count; i++) {
        switch (s_mode_items[i]) {
        case MODE_STUDY:
            lv_label_set_text(s_row_labels[i], "闯关学习");
            break;
        case MODE_LISTEN:
            lv_label_set_text(s_row_labels[i], "听音选义");
            break;
        case MODE_GENDER:
            lv_label_set_text_fmt(s_row_labels[i], "辨性别 %u/%u",
                                  st.nouns_mastered, st.nouns);
            break;
        }
    }
}

static void show_modes(bool resume_cursor) {
    ruw_category_stats_t st;
    ruw_category_stats(&st, s_filter, RUW_WORD_COUNT, s_category, s_gender,
                       s_stage, s_gender_stage);

    lv_obj_t *scr = page_begin(PAGE_MODES);
    lv_obj_t *title = label(scr, &ruw_font_20, COL_INK, filter_name());
    lv_obj_set_pos(title, 30, 10);

    // 信息卡:两行小字。
    lv_obj_t *card = box(scr, 16, 40, 208, 76, COL_CARD, 14);
    lv_obj_t *l1 = label(card, &ruw_font_20, COL_INK, "");
    lv_label_set_text_fmt(l1, "已掌握 %u/%u", st.mastered, st.words);
    lv_obj_set_pos(l1, 12, 12);
    lv_obj_t *l2 = label(card, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(l2, "新词 %u · 错词 %u", st.fresh, st.wrong);
    lv_obj_set_pos(l2, 12, 42);

    // 动态菜单项:本类无名词时不显示辨性别。错题本已挪到首页。
    s_mode_item_count = 0;
    s_mode_items[s_mode_item_count++] = MODE_STUDY;
    s_mode_items[s_mode_item_count++] = MODE_LISTEN;
    if (st.nouns > 0) s_mode_items[s_mode_item_count++] = MODE_GENDER;

    // 从答题回来时,光标停在刚玩的那个模式;新进分类时回到第一行。
    s_mode_cursor = 0;
    if (resume_cursor) {
        for (uint8_t i = 0; i < s_mode_item_count; i++) {
            if (s_mode_items[i] == s_mode) { s_mode_cursor = i; break; }
        }
    }

    for (uint8_t i = 0; i < s_mode_item_count; i++)
        list_row(scr, 128 + i * 42, 36, "", true);
    modes_rows_text();
    list_select(s_mode_cursor);
}

// ---------------------------------------------------------------- 设置页

static void settings_text(void) {
    static const char *const BARS[VOLUME_LEVELS] = {
        "●○○○○", "●●○○○", "●●●○○", "●●●●○", "●●●●●",
    };
    lv_label_set_text_fmt(s_row_labels[SET_VOLUME], "音量  %s", BARS[s_volume_level]);
    lv_label_set_text(s_row_labels[SET_RESET],
                      s_reset_confirm ? "再按一次确认重置" : "重置进度");
}

static void show_settings(void) {
    s_reset_confirm = 0;
    lv_obj_t *scr = page_begin(PAGE_SETTINGS);

    lv_obj_t *title = label(scr, &ruw_font_20, COL_INK, "设置");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    for (int i = 0; i < SET_COUNT; i++) list_row(scr, 76 + i * 52, 44, "", true);
    settings_text();
    list_select(s_settings_cursor);

    lv_obj_t *hint = label(scr, &ruw_font_20, COL_MUTED, "长按确定：返回");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 250);
}

// 重置全部进度(词义与性别),回到初始状态并重绘分类页。
static void reset_progress(void) {
    ruw_progress_reset(&s_progress);
    ruw_progress_reset(&s_gender_progress);
    progress_save();
    s_sess.valid = 0;  // 进度清空,断点一并作废
    memset(&s_sess.session, 0, sizeof(s_sess.session));
    sess_save();
    show_home();
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

// 统一的白色题面大卡片:右上角小标签(新词/错词/再考一次)。
static lv_obj_t *create_question_card(lv_obj_t *scr, const char *tag) {
    lv_obj_t *card = box(scr, 16, 44, 208, 104, COL_CARD, 14);
    lv_obj_t *chip = label(card, &ruw_font_20, COL_ACCENT, tag);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, -8, 6);
    return card;
}

// 俄语单词块(放进卡片):重读元音标红 + 拉丁转写。
static void create_ru_block(lv_obj_t *card, const ruw_word_t *w) {
    bool long_word = utf8_len(w->ru) > 8;
    lv_obj_t *ru = label(card, long_word ? &ruw_font_28 : &ruw_font_36, COL_INK, "");
    lv_label_set_recolor(ru, true);
    lv_label_set_text(ru, w->ru_marked);
    lv_obj_set_width(ru, 188);
    lv_label_set_long_mode(ru, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ru, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ru, LV_ALIGN_TOP_MID, 0, long_word ? 18 : 16);

    lv_obj_t *tr = label(card, &ruw_font_20, COL_MUTED, w->translit);
    lv_obj_align(tr, LV_ALIGN_TOP_MID, 0, long_word ? 60 : 66);
}

// 阴阳中性题面(放进卡片):名词(重音标红)+ 转写 + 一行中文小字。
// 8 字符及以上用 28px,避免词尾贴近右上角标签。
static void create_gender_block(lv_obj_t *card, const ruw_word_t *w) {
    bool long_word = utf8_len(w->ru) >= 8;
    lv_obj_t *ru = label(card, long_word ? &ruw_font_28 : &ruw_font_36, COL_INK, "");
    lv_label_set_recolor(ru, true);
    lv_label_set_text(ru, w->ru_marked);
    lv_obj_set_width(ru, 188);
    lv_label_set_long_mode(ru, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ru, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ru, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *tr = label(card, &ruw_font_20, COL_MUTED, w->translit);
    lv_obj_align(tr, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t *zh = label(card, &ruw_font_20, COL_MUTED, w->zh);
    lv_obj_align(zh, LV_ALIGN_TOP_MID, 0, 80);
}

// 听音模式题面:48px 大"听"。
static void create_listen_block(lv_obj_t *card) {
    lv_obj_t *hear = label(card, &ruw_font_48, COL_ACCENT, "听");
    lv_obj_center(hear);
}

static void show_question(void) {
    s_answered = false;
    s_selected = 0;
    s_hear_prompt = NULL;
    s_example_page = 0;
    s_ans_card = NULL;   // page_begin 整屏重建,旧卡指针作废
    uint16_t target = s_session.queue[s_session.pos];
    bool gender_mode = s_mode == MODE_GENDER;

    // 性别模式选项固定顺序,正确位置即性别编码;其余模式随机抽干扰项。
    if (gender_mode) s_question.answer = RUW_WORDS[target].gender;
    else ruw_question_make(&s_question, target, RUW_WORD_COUNT, s_category, &s_seed);

    lv_obj_t *scr = page_begin(PAGE_QUIZ);
    const ruw_word_t *w = &RUW_WORDS[target];

    static const char *const MODE_NAME[4] = { "闯关", "错题", "听音", "辨性别" };
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

    lv_obj_t *card = create_question_card(scr, tag);
    if (gender_mode) {
        create_gender_block(card, w);
    } else if (s_mode == MODE_LISTEN) {
        // 听音模式:作答前不露俄语/转写,只放一个大"听"。
        s_hear_prompt = card;
        create_listen_block(card);
    } else {
        create_ru_block(card, w);
    }

    static const char *const GENDER_OPT[RUW_OPTIONS] = {
        "阳性", "阴性", "中性", "只有复数",
    };
    for (int i = 0; i < RUW_OPTIONS; i++) {
        const char *text = gender_mode ? GENDER_OPT[i]
                           : RUW_WORDS[s_question.word[i]].zh;
        list_row(scr, 154 + i * 33, 30, text, false);
    }
    // 施工单原文案在 240 宽屏一行放不下,精简为同等信息的「▲▼选择 ●确定 长按重听」。
    s_hint = label(scr, &ruw_font_20, COL_MUTED,
                   "▲▼选择 ●确定 长按重听");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 288);
    quiz_rows_refresh();
    play_word(target);
}

// 作答后详情卡,分两页:0=词性+完整释义,1=俄语例句+中文翻译。
// 翻页(s_example_page)时整卡重建;卡片 y=152,高 136,底边 288。
static void draw_answer_detail(void) {
    bool gender_mode = s_mode == MODE_GENDER;
    uint16_t target = current_word();
    const ruw_word_t *w = &RUW_WORDS[target];
    if (s_ans_card) lv_obj_delete(s_ans_card);
    s_ans_card = box(s_scr, 16, 152, 208, 136, s_ans_ok ? COL_OK_BG : COL_BAD_BG, 14);

    lv_obj_t *pg = label(s_ans_card, &ruw_font_20, COL_MUTED, "");
    lv_label_set_text_fmt(pg, "%u/2", s_example_page + 1);
    lv_obj_set_pos(pg, 176, 8);

    if (s_example_page == 0) {
        lv_obj_t *verdict = label(s_ans_card, &ruw_font_20, s_ans_ok ? COL_OK : COL_ACCENT,
                                  s_ans_ok ? "答对了！" : "答错了，已加入错题本");
        lv_obj_set_pos(verdict, 12, 8);

        if (gender_mode) {
            const char *hint = ruw_gender_hint(w->ru, w->gender);
            lv_obj_t *h = label(s_ans_card, &ruw_font_20, COL_INK, hint);
            lv_obj_set_width(h, 184);
            lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
            lv_obj_set_pos(h, 12, 36);
        } else {
            lv_obj_t *pos = label(s_ans_card, &ruw_font_20, COL_MUTED, w->pos);
            lv_obj_set_pos(pos, 12, 36);

            lv_obj_t *full = label(s_ans_card, &ruw_font_20, COL_INK, w->full);
            lv_obj_set_width(full, 184);
            lv_label_set_long_mode(full, LV_LABEL_LONG_WRAP);
            lv_obj_set_pos(full, 12, 62);
        }

        if (!s_ans_ok) {
            static const char *const GENDER_OPT[RUW_OPTIONS] = {
                "阳性", "阴性", "中性", "只有复数",
            };
            lv_obj_t *mine = label(s_ans_card, &ruw_font_20, COL_MUTED, "");
            if (gender_mode)
                lv_label_set_text_fmt(mine, "你选了：%s", GENDER_OPT[s_selected]);
            else
                lv_label_set_text_fmt(mine, "你选了：%s",
                                      RUW_WORDS[s_question.word[s_selected]].zh);
            lv_label_set_long_mode(mine, LV_LABEL_LONG_DOT);
            lv_obj_set_width(mine, 184);
            lv_obj_set_pos(mine, 12, 100);
        }
    } else {
        // 第 2 页:俄语例句 + 中文翻译,宽 184 自动换行,间隔 6px。
        lv_obj_t *eru = label(s_ans_card, &ruw_font_20, COL_INK, w->example_ru);
        lv_obj_set_width(eru, 184);
        lv_label_set_long_mode(eru, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(eru, 12, 18);

        lv_obj_t *ezh = label(s_ans_card, &ruw_font_20, COL_MUTED, w->example_zh);
        lv_obj_set_width(ezh, 184);
        lv_label_set_long_mode(ezh, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(ezh, 12, 92);
    }
}

static void show_answer_detail(bool ok) {
    s_ans_ok = ok;
    s_example_page = 0;
    for (int i = 0; i < s_row_count; i++) lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
    draw_answer_detail();
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
    s_sess.session = s_session;
    sess_save();

    // 听音模式:作答后清空卡片,把大"听"换成俄语(重音标红)+转写。
    if (s_mode == MODE_LISTEN) {
        lv_obj_clean(s_hear_prompt);
        create_ru_block(s_hear_prompt, &RUW_WORDS[target]);
    }

    lv_label_set_text(s_hint, "▼翻页 ▲重听 ●下一题");
    show_answer_detail(ok);
    play_word(target);
}

// ---------------------------------------------------------------- 本轮完成

static void show_done(void) {
    bool gender_mode = s_mode == MODE_GENDER;
    ruw_progress_t *prog = gender_mode ? &s_gender_progress : &s_progress;
    ruw_session_finish(prog);
    progress_save();
    // 本关打完:断点作废,但保留 filter/mode 供「继续挑战」下次直接开新关。
    s_sess.valid = 0;
    memset(&s_sess.session, 0, sizeof(s_sess.session));
    sess_save();

    lv_obj_t *scr = page_begin(PAGE_DONE);
    static const char *const DONE_TITLE[4] = {
        "本关完成", "错题复习完成", "本关完成", "辨性别本关完成",
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
        bool in_cat = (s_filter == 0 || s_category[i] == s_filter - 1);
        if (mode == MODE_WRONG) {
            // 错题本(首页入口):全部分类里词义错的词,不受分类影响。
            s_eligible[i] = (s_stage[i] == RUW_STAGE_WRONG1
                          || s_stage[i] == RUW_STAGE_WRONG2);
        } else if (mode == MODE_GENDER) {
            // 只考名词(性别非空),并按分类筛选。
            s_eligible[i] = in_cat && RUW_WORDS[i].gender != RUW_GENDER_NONE;
        } else {
            s_eligible[i] = in_cat;
        }
    }
    ruw_progress_t *prog = mode == MODE_GENDER ? &s_gender_progress : &s_progress;
    if (ruw_session_build(&s_session, prog, s_eligible) == 0) return false;
    s_mode = mode;
    // 落断点:这是一关掉到一半的现场。
    s_sess.valid = 1;
    s_sess.filter = s_filter;
    s_sess.mode = (uint8_t)mode;
    s_sess.session = s_session;
    sess_save();
    show_question();
    return true;
}

// ---------------------------------------------------------------- 输入

// 二级分类页输入:上下移动(循环);确定进模式页;长按确定回首页。
static void category_input(const input_event_t *in) {
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        show_home();
        return;
    }
    if (in->ev != BSP_BTN_CLICK) return;
    uint8_t total = RUW_CATEGORY_COUNT + 1;
    if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        s_cat_cursor = (uint8_t)((s_cat_cursor + (in->btn == BSP_BTN_UP ? total - 1 : 1)) % total);
        category_refresh();
        return;
    }
    // 确定:选中当前分类,存盘,进二级模式页。
    s_filter = s_cat_cursor;
    progress_save();
    show_modes(false);
}

// 首页输入:上/下循环;确定进入;长按确定进设置。
static void home_input(const input_event_t *in) {
    // 开机动画进行中:任意键立即跳到最终版面,本次按键不触发菜单。
    if (s_intro_timer) {
        lv_timer_delete(s_intro_timer);
        s_intro_timer = NULL;
        lv_anim_delete(s_home_logo, NULL);
        lv_anim_delete(s_home_title, NULL);
        lv_anim_delete(s_home_sub, NULL);
        if (s_battery) lv_anim_delete(s_battery, NULL);
        for (uint8_t i = 0; i < s_row_count; i++) {
            lv_anim_delete(s_rows[i], NULL);
            lv_anim_delete(s_row_labels[i], NULL);
        }
        home_finish_layout();
        s_intro_played = true;
        home_blink_start();
        return;
    }
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        show_settings();
        return;
    }
    if (in->ev != BSP_BTN_CLICK) return;
    if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        s_home_cursor = (uint8_t)((s_home_cursor +
            (in->btn == BSP_BTN_UP ? HOME_VISIBLE - 1 : 1)) % HOME_VISIBLE);
        list_select(s_home_cursor);
        return;
    }
    switch (s_home_cursor) {
    case 0:
        continue_challenge();
        break;
    case 1:
        s_cat_cursor = s_filter;
        show_category();
        break;
    default: {
        // 错题本:全局错词。没有错题时该行已写明,不跳页。
        ruw_category_stats_t all;
        ruw_category_stats(&all, 0, RUW_WORD_COUNT, s_category, s_gender,
                           s_stage, s_gender_stage);
        if (all.wrong > 0) start_session(MODE_WRONG);
        break;
    }
    }
}

// 设置页输入:上下移动;确定执行;长按确定回分类页。
static void settings_input(const input_event_t *in) {
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        show_home();
        return;
    }
    if (in->ev != BSP_BTN_CLICK) return;
    if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        s_settings_cursor = (uint8_t)((s_settings_cursor +
            (in->btn == BSP_BTN_UP ? SET_COUNT - 1 : 1)) % SET_COUNT);
        s_reset_confirm = 0;  // 移开后取消重置确认
        settings_text();
        list_select(s_settings_cursor);
        return;
    }
    switch (s_settings_cursor) {
    case SET_VOLUME:
        s_volume_level = (uint8_t)((s_volume_level + 1) % VOLUME_LEVELS);
        apply_volume();
        settings_text();
        progress_save();
        play_word(0);  // 试听
        break;
    case SET_RESET:
        // 连按两次确定才执行:第一次只改文字。
        if (!s_reset_confirm) {
            s_reset_confirm = 1;
            settings_text();
        } else {
            reset_progress();
        }
        break;
    }
}

// 二级模式页输入:上下移动(按动态菜单项循环);确定开始;长按确定回分类页。
static void modes_input(const input_event_t *in) {
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        show_category();
        return;
    }
    if (in->ev != BSP_BTN_CLICK) return;
    if (in->btn == BSP_BTN_UP || in->btn == BSP_BTN_DOWN) {
        s_mode_cursor = (uint8_t)((s_mode_cursor +
            (in->btn == BSP_BTN_UP ? s_mode_item_count - 1 : 1)) % s_mode_item_count);
        list_select(s_mode_cursor);
        return;
    }
    study_mode_t mode = s_mode_items[s_mode_cursor];
    if (!start_session(mode)) {
        // 范围内没有可学内容:在对应那一行上提示,不跳页。
        const char *hint;
        switch (mode) {
        case MODE_GENDER: hint = "本类名词已全部掌握"; break;
        default:          hint = "本类已全部掌握"; break;
        }
        lv_label_set_text(s_row_labels[s_mode_cursor], hint);
    }
}

static void quiz_input(const input_event_t *in) {
    if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_LONG) {
        // 已作答的词进度已保存;本轮不计入轮次。错题本回首页,其余回模式页。
        if (s_mode == MODE_WRONG) show_home();
        else show_modes(true);
    } else if (in->btn == BSP_BTN_UP && in->ev == BSP_BTN_LONG) {
        play_word(current_word());
    } else if (in->ev != BSP_BTN_CLICK) {
        return;
    } else if (s_answered) {
        if (in->btn == BSP_BTN_OK) {
            if (s_session.pos < s_session.len) show_question();
            else show_done();
        } else if (in->btn == BSP_BTN_UP) {
            play_word(current_word());      // ▲ 重听
        } else {  // DOWN: ▼ 翻到释义/例句页
            s_example_page ^= 1;
            draw_answer_detail();
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
    case PAGE_MODES:    modes_input(in); break;
    case PAGE_SETTINGS: settings_input(in); break;
    case PAGE_QUIZ:     quiz_input(in); break;
    case PAGE_DONE:
        if (in->btn == BSP_BTN_OK && in->ev == BSP_BTN_CLICK) {
            if (s_mode == MODE_WRONG) show_home();
            else show_modes(true);
        }
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

    for (int i = 0; i < RUW_WORD_COUNT; i++) {
        s_category[i] = RUW_WORDS[i].category;
        s_gender[i] = RUW_WORDS[i].gender;
    }
    s_seed = esp_random();
    progress_load();
    sess_load();

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
