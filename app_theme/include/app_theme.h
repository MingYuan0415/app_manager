/**
 * @brief  应用主题与字体管理 API
 */

#ifndef __APP_THEME_H__
#define __APP_THEME_H__

#include "esp_err.h"
#include "lvgl.h"   /* lv_font_t */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default font sizes loaded in app_theme_font_id_t order. */
#define APP_THEME_FONT_SIZES {18, 22, 28, 36, 48, 54}

/* 字体 ID 枚举（对齐早期 GUI_APP_FONT_*，6 档） */
/** @brief Font roles loaded by the application theme. */
typedef enum
{
    APP_THEME_FONT_BODY   = 0,
    APP_THEME_FONT_SMALL  = 1,
    APP_THEME_FONT_HEAD   = 2,
    APP_THEME_FONT_BIGL   = 3,
    APP_THEME_FONT_HUGE   = 4,
    APP_THEME_FONT_TITLE  = 5,
    APP_THEME_FONT_MAX    = 6,
} app_theme_font_id_t;

/**
 * @brief Reset the theme font table.
 * @return ESP_OK when initialized.
 */
esp_err_t app_theme_init(void);
/** @brief Release references held by the theme font table. */
void app_theme_deinit(void);
/**
 * @brief Assign a loaded font to one theme role.
 * @param id selects the theme role.
 * @param font points to a font valid until app_theme_deinit().
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for invalid input.
 */
esp_err_t app_theme_set_font(app_theme_font_id_t id, const lv_font_t *font);
/**
 * @brief Return the font assigned to a theme role.
 * @param id selects the theme role.
 * @return Assigned font, or NULL when unavailable.
 */
const lv_font_t *app_theme_get_font(app_theme_font_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* __APP_THEME_H__ */
