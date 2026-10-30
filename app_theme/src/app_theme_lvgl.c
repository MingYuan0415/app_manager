/**
 * @file app_theme_lvgl.c
 * @brief Project LVGL theme: default-theme styling with a widget mask.
 *
 * LVGL's default theme references every enabled widget class from its apply
 * callback. That prevents --gc-sections from dropping widgets the application
 * never creates. This translation unit compiles the pinned LVGL 9.6 default
 * theme with an explicit, theme-only widget mask: the application's widget
 * Kconfig options stay enabled, while this theme references only the widgets
 * MicroTech actually uses.
 */

#include "app_theme.h"

#if !defined(LVGL_VERSION_MAJOR) || (LVGL_VERSION_MAJOR != 9) || \
    !defined(LVGL_VERSION_MINOR) || (LVGL_VERSION_MINOR != 6)
    #error "app_theme local LVGL theme is pinned to LVGL 9.6"
#endif

/* The public default-theme header hides these declarations when the built-in
 * theme Kconfig option is disabled, although the implementation below still
 * needs them. The names are local to this translation unit. */
#define lv_theme_default_init       app_theme_lvgl_init
#define lv_theme_default_is_inited  app_theme_lvgl_is_inited
#define lv_theme_default_get        app_theme_lvgl_get
#define lv_theme_default_deinit     app_theme_lvgl_deinit
lv_theme_t *app_theme_lvgl_init(lv_display_t *disp,
                                lv_color_t color_primary,
                                lv_color_t color_secondary,
                                bool dark,
                                const lv_font_t *font);
bool app_theme_lvgl_is_inited(void);
void app_theme_lvgl_deinit(void);

/*
 * Theme-only widget mask. Do not move these definitions to sdkconfig: widget
 * APIs and their CONFIG_LV_USE_* options remain enabled for application code.
 */
#undef LV_USE_TABVIEW
#define LV_USE_TABVIEW 0
#undef LV_USE_WIN
#define LV_USE_WIN 0
#undef LV_USE_CALENDAR
#define LV_USE_CALENDAR 0
#undef LV_USE_CALENDAR_HEADER_ARROW
#define LV_USE_CALENDAR_HEADER_ARROW 0
#undef LV_USE_CALENDAR_HEADER_DROPDOWN
#define LV_USE_CALENDAR_HEADER_DROPDOWN 0
#undef LV_USE_MENU
#define LV_USE_MENU 0
#undef LV_USE_TABLE
#define LV_USE_TABLE 0
#undef LV_USE_TEXTAREA
#define LV_USE_TEXTAREA 0
#undef LV_USE_KEYBOARD
#define LV_USE_KEYBOARD 0
#undef LV_USE_LIST
#define LV_USE_LIST 0
#undef LV_USE_MSGBOX
#define LV_USE_MSGBOX 0
#undef LV_USE_SPINBOX
#define LV_USE_SPINBOX 0
#undef LV_USE_TILEVIEW
#define LV_USE_TILEVIEW 0
#undef LV_USE_LED
#define LV_USE_LED 0
#undef LV_USE_SCALE
#define LV_USE_SCALE 0
#undef LV_USE_DROPDOWN
#define LV_USE_DROPDOWN 0
#undef LV_USE_CHECKBOX
#define LV_USE_CHECKBOX 0
/* LV_USE_LINE stays enabled: level_app creates lv_line widgets. */
#undef LV_USE_BUTTONMATRIX
#define LV_USE_BUTTONMATRIX 0
#undef LV_USE_SPINNER
#define LV_USE_SPINNER 0

/* Keep the same transition behavior as the current default theme. */
#undef LV_THEME_DEFAULT_GROW
#define LV_THEME_DEFAULT_GROW 1
#undef LV_THEME_DEFAULT_TRANSITION_TIME
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#undef LV_THEME_DEFAULT_DARK
#define LV_THEME_DEFAULT_DARK 0

#undef LV_USE_THEME_DEFAULT
#define LV_USE_THEME_DEFAULT 1
#include "lv_theme_default_local.inc"

/* The project theme owns one display; a second display is rejected. */
static lv_display_t *s_theme_display;

esp_err_t app_theme_lvgl_attach(lv_display_t *display)
{
    if (display == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_theme_display == display && app_theme_lvgl_is_inited())
    {
        return ESP_OK;
    }
    if (s_theme_display != NULL && s_theme_display != display)
    {
        return ESP_ERR_INVALID_STATE;
    }

    lv_theme_t *theme = app_theme_lvgl_init(display,
                                            lv_palette_main(LV_PALETTE_BLUE),
                                            lv_palette_main(LV_PALETTE_RED),
                                            false,
                                            LV_FONT_DEFAULT);
    if (theme == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_theme(display, theme);
    s_theme_display = display;
    return ESP_OK;
}

void app_theme_lvgl_detach(void)
{
    if (s_theme_display != NULL)
    {
        lv_display_set_theme(s_theme_display, NULL);
        s_theme_display = NULL;
    }
    app_theme_lvgl_deinit();
}
