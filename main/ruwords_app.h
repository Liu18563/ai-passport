// main/ruwords_app.h —— 俄语单词斩应用入口。
#pragma once

#include "esp_err.h"

// 显示与 LVGL 已初始化后调用:初始化 NVS、音频、电量、按键并显示首页。
esp_err_t ruwords_app_start(void);
