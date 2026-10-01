// main/main.c —— 俄语单词斩固件入口:初始化显示后交给 ruwords_app。
// 基线的硬件测试菜单(demo_*.c)不参与本应用构建,仅作参考保留在仓库中。
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "ruwords_app.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "俄语单词斩启动");
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    esp_err_t err = ruwords_app_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(err));
}
