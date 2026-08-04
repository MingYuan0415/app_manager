/** @file Host LVGL adapter contract used by App Manager tests. */
#ifndef __HOST_ESP_LV_ADAPTER_H__
#define __HOST_ESP_LV_ADAPTER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_mmap_assets.h"
#include "lvgl.h"

typedef void *esp_lcd_panel_io_handle_t;

/** @brief Adapter initialization options. */
typedef struct esp_lv_adapter_config
{
    uint32_t task_stack_size;
    int task_core_id;
    bool stack_in_psram;
} esp_lv_adapter_config_t;

#define ESP_LV_ADAPTER_DEFAULT_CONFIG() \
    { .task_stack_size = 8192U, .task_core_id = -1, \
      .stack_in_psram = false }

/** @brief Display interface values used by the fixture. */
typedef enum
{
    ESP_LV_ADAPTER_PANEL_IF_OTHER = 2,
} esp_lv_adapter_panel_interface_t;

/** @brief Display rotation values used by the fixture. */
typedef enum
{
    ESP_LV_ADAPTER_ROTATE_0 = 0,
} esp_lv_adapter_rotation_t;

/** @brief Monochrome layout values used by the fixture. */
typedef enum
{
    ESP_LV_ADAPTER_MONO_LAYOUT_NONE = 0,
} esp_lv_adapter_mono_layout_t;

/** @brief Tear-avoidance values used by the fixture. */
typedef enum
{
    ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT = 0,
    ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC = 1,
} esp_lv_adapter_tear_avoid_mode_t;

/** @brief Tearing-effect synchronization options. */
typedef struct esp_lv_adapter_te_sync_config
{
    int gpio_num;
    uint32_t time_tvdl_ms;
    uint32_t time_tvdh_ms;
    uint32_t bus_freq_hz;
    uint8_t data_lines;
    uint8_t bits_per_pixel;
    int intr_type;
    uint8_t refresh_window_percent;
} esp_lv_adapter_te_sync_config_t;

#define ESP_LV_ADAPTER_TE_SYNC_DISABLED() \
    ((esp_lv_adapter_te_sync_config_t){ .gpio_num = -1 })
#define ESP_LV_ADAPTER_TE_TVDL_DEFAULT_MS 13
#define ESP_LV_ADAPTER_TE_TVDH_DEFAULT_MS 1
#define ESP_LV_ADAPTER_TE_WINDOW_PERCENT_DEFAULT 66

/** @brief Display resolution and buffer profile. */
typedef struct esp_lv_adapter_display_profile
{
    esp_lv_adapter_panel_interface_t interface;
    esp_lv_adapter_rotation_t rotation;
    uint16_t hor_res;
    uint16_t ver_res;
    uint16_t buffer_height;
    bool use_psram;
    bool enable_ppa_accel;
    bool require_double_buffer;
    esp_lv_adapter_mono_layout_t mono_layout;
} esp_lv_adapter_display_profile_t;

/** @brief Display registration handles and profile. */
typedef struct esp_lv_adapter_display_config
{
    esp_lcd_panel_handle_t panel;
    void *panel_io;
    esp_lv_adapter_display_profile_t profile;
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;
    esp_lv_adapter_te_sync_config_t te_sync;
} esp_lv_adapter_display_config_t;

/** @brief Touch interrupt callback table. */
typedef struct esp_lv_adapter_touch_callbacks
{
    void (*on_interrupt)(esp_lcd_touch_handle_t touch, void *user_ctx);
    void *user_ctx;
} esp_lv_adapter_touch_callbacks_t;

/** @brief Touch registration handles and callbacks. */
typedef struct esp_lv_adapter_touch_config
{
    lv_display_t *disp;
    esp_lcd_touch_handle_t handle;
    esp_lv_adapter_touch_callbacks_t callbacks;
} esp_lv_adapter_touch_config_t;

/** @brief Optional custom bitmap callback table. */
typedef struct esp_lv_adapter_draw_bitmap_callbacks
{
    esp_err_t (*custom_draw_bitmap)(lv_display_t *display,
                                    esp_lcd_panel_handle_t panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_map,
                                    void *user_ctx);
} esp_lv_adapter_draw_bitmap_callbacks_t;

#define ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch_handle) \
    { .disp = (display), .handle = (touch_handle) }

/** @brief Filesystem mount options for the fake resource adapter. */
typedef struct esp_lv_adapter_fs_config
{
    char fs_letter;
    int fs_nums;
    mmap_assets_handle_t fs_assets;
} fs_cfg_t;

typedef void *esp_lv_fs_handle_t;
typedef void *esp_lv_adapter_ft_font_handle_t;

/** @brief FreeType font styles used by the fixture. */
typedef enum
{
    ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL = 0,
} esp_lv_adapter_ft_font_style_t;

/** @brief Font resource configuration. */
typedef struct esp_lv_adapter_ft_font_config
{
    const char *name;
    uint16_t size;
    esp_lv_adapter_ft_font_style_t style;
    const void *mem;
    size_t mem_size;
} esp_lv_adapter_ft_font_config_t;

/** @brief Initialize the fake LVGL adapter. */
esp_err_t esp_lv_adapter_init(const esp_lv_adapter_config_t *config);
/** @brief Start the fake LVGL worker. */
esp_err_t esp_lv_adapter_start(void);
/** @brief Deinitialize the fake LVGL adapter. */
esp_err_t esp_lv_adapter_deinit(void);
/** @brief Report whether the fake LVGL adapter is initialized. */
bool esp_lv_adapter_is_initialized(void);
/** @brief Lock the fake LVGL adapter. */
esp_err_t esp_lv_adapter_lock(int32_t timeout_ms);
/** @brief Unlock the fake LVGL adapter. */
void esp_lv_adapter_unlock(void);
/** @brief Pause the fake LVGL worker. */
esp_err_t esp_lv_adapter_pause(int32_t timeout_ms);
/** @brief Resume the fake LVGL worker. */
esp_err_t esp_lv_adapter_resume(void);
/** @brief Register a fake display. */
lv_display_t *esp_lv_adapter_register_display(
    const esp_lv_adapter_display_config_t *config);
/** @brief Register a fake touch input device. */
lv_indev_t *esp_lv_adapter_register_touch(
    const esp_lv_adapter_touch_config_t *config);
/** @brief Mount fake resource assets. */
esp_err_t esp_lv_adapter_fs_mount(const fs_cfg_t *config,
                                  esp_lv_fs_handle_t *ret_handle);
/** @brief Initialize a fake FreeType font. */
esp_err_t esp_lv_adapter_ft_font_init(
    const esp_lv_adapter_ft_font_config_t *config,
    esp_lv_adapter_ft_font_handle_t *handle);
/** @brief Return a fake FreeType font handle. */
const lv_font_t *esp_lv_adapter_ft_font_get(
    esp_lv_adapter_ft_font_handle_t handle);
/** @brief Release a fake FreeType font. */
esp_err_t esp_lv_adapter_ft_font_deinit(
    esp_lv_adapter_ft_font_handle_t handle);
/** @brief Install fake bitmap callbacks. */
esp_err_t esp_lv_adapter_set_draw_bitmap_callbacks(
    lv_display_t *display,
    const esp_lv_adapter_draw_bitmap_callbacks_t *callbacks,
    void *user_ctx);
/** @brief Notify a fake touch interrupt. */
bool esp_lv_adapter_touch_notify_interrupt(lv_indev_t *touch);

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
    /** @brief Enable or disable fake display FPS statistics. */
    esp_err_t esp_lv_adapter_fps_stats_enable(lv_display_t *display, bool enable);
    /** @brief Read fake display FPS statistics. */
    esp_err_t esp_lv_adapter_get_fps(lv_display_t *display, uint32_t *fps);
    /** @brief Reset fake display FPS statistics. */
    esp_err_t esp_lv_adapter_fps_stats_reset(lv_display_t *display);
#endif

#endif /* __HOST_ESP_LV_ADAPTER_H__ */
