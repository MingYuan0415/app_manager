/**
 * @brief  应用主题组件 — 字体查找表
 */

#include "app_theme.h"
#include <string.h>

static const lv_font_t *s_fonts[APP_THEME_FONT_MAX] = {NULL};

esp_err_t app_theme_init(void)
{
    memset(s_fonts, 0, sizeof(s_fonts));
    return ESP_OK;
}

void app_theme_deinit(void)
{
    memset(s_fonts, 0, sizeof(s_fonts));
}

esp_err_t app_theme_set_font(app_theme_font_id_t id, const lv_font_t *font)
{
    if (id < 0 || id >= APP_THEME_FONT_MAX || font == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_fonts[id] = font;
    return ESP_OK;
}

const lv_font_t *app_theme_get_font(app_theme_font_id_t id)
{
    if (id < 0 || id >= APP_THEME_FONT_MAX)
    {
        return NULL;
    }
    return s_fonts[id];
}
