#define DBG_TAG "app_adapter"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_manager.h"
#include "app_manager_back_gesture.h"
#include "app_manager_builtin.h"
#include "app_manager_config.h"
#include "app_manager_control.h"
#include "app_manager_display_diagnostics_priv.h"
#include "app_manager_display.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"
#include "app_theme.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    #include "esp_timer.h"
#endif
#include <stdatomic.h>
#include <string.h>

#define APP_ADAPTER_PARTIAL_BUFFER_HEIGHT \
    ((uint32_t)CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT)

#define APP_ADAPTER_COLOR_FORMAT      LV_COLOR_FORMAT_RGB565
#define APP_ADAPTER_COLOR_FORMAT_NAME "RGB565"

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE && !CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    #error "App manager FreeType integration requires the LVGL adapter filesystem"
#endif

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    #include "esp_mmap_assets.h"

    #define APP_RESOURCE_PARTITION_LABEL "res"

    static mmap_assets_handle_t s_resource_assets;
#endif

typedef struct app_image_resource
{
    uint32_t semantic_id;
    lv_image_dsc_t image;
} app_image_resource_t;

typedef enum
{
    APP_ADAPTER_UNINITIALIZED = 0,
    APP_ADAPTER_INITIALIZING,
    APP_ADAPTER_READY,
    APP_ADAPTER_DEINITIALIZING,
} app_adapter_state_t;

static atomic_int s_state = ATOMIC_VAR_INIT(APP_ADAPTER_UNINITIALIZED);
static atomic_flag s_cleanup_busy = ATOMIC_FLAG_INIT;
static bool s_theme_ready;
static bool s_adapter_ready;
static bool s_mailbox_ready;
static bool s_registry_ready;
static bool s_presentation_ready;
static bool s_lifecycle_ready;
static bool s_navigation_ready;
static bool s_gesture_ready;
static bool s_control_ready;
static bool s_active_config_valid;
static app_manager_config_t s_active_config;
static bsp_display_port_t s_display_port_storage;
static const bsp_display_port_t *s_display_port;
static lv_display_t *s_display;
static lv_indev_t *s_touch;
static atomic_bool s_display_sleeping;
static atomic_bool s_display_fenced;
static atomic_bool s_initial_display_committed;
/* The completion ISR only reports flush-ready. Keep the first synchronous
 * submission error in the UI worker so a failed refresh is not made visible. */
static atomic_bool s_refresh_capture_active;
static atomic_int s_refresh_capture_error;
static esp_lcd_panel_handle_t s_panel_proxy_delegate;
static app_image_resource_t *s_image_resources;
static size_t s_image_resource_count;

static void _app_adapter_capture_refresh_error(esp_err_t result)
{
    if (result != ESP_OK &&
            atomic_load_explicit(&s_refresh_capture_active,
                                 memory_order_acquire))
    {
        int expected = ESP_OK;
        (void)atomic_compare_exchange_strong_explicit(
            &s_refresh_capture_error, &expected, (int)result,
            memory_order_acq_rel, memory_order_acquire);
    }
}

static esp_err_t _app_adapter_submit_bitmap(lv_display_t *display,
        esp_lcd_panel_handle_t panel,
        int x_start, int y_start, int x_end, int y_end,
        const void *color_map)
{
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    const int64_t start_us = esp_timer_get_time();
    const uint32_t pixel_count = (uint32_t)(x_end - x_start) *
                                 (uint32_t)(y_end - y_start);
#else
    (void)display;
#endif
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel, x_start, y_start,
                       x_end, y_end, color_map);
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    if (display != NULL)
    {
        app_manager_display_diagnostics_record_submit(display,
                lv_display_flush_is_last(display),
                pixel_count,
                (uint32_t)(esp_timer_get_time() - start_us), result);
    }
#endif
    _app_adapter_capture_refresh_error(result);
    return result;
}

static esp_err_t _app_adapter_panel_proxy_draw_bitmap(esp_lcd_panel_t *panel,
        int x_start, int y_start, int x_end, int y_end,
        const void *color_map)
{
    if (panel == NULL || s_panel_proxy_delegate == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return _app_adapter_submit_bitmap(s_display, s_panel_proxy_delegate,
                                      x_start, y_start, x_end, y_end,
                                      color_map);
}

static esp_lcd_panel_t s_panel_proxy =
{
    .draw_bitmap = _app_adapter_panel_proxy_draw_bitmap,
};

static esp_err_t _app_adapter_draw_bitmap(lv_display_t *display,
        esp_lcd_panel_handle_t panel,
        int x_start, int y_start, int x_end, int y_end,
        const void *color_map, void *user_ctx)
{
    (void)user_ctx;
    return _app_adapter_submit_bitmap(display, panel, x_start, y_start,
                                      x_end, y_end, color_map);
}

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE
static const app_theme_font_id_t s_font_ids[] =
{
    APP_THEME_FONT_BODY,
    APP_THEME_FONT_SMALL,
    APP_THEME_FONT_HEAD,
    APP_THEME_FONT_BIGL,
    APP_THEME_FONT_HUGE,
    APP_THEME_FONT_TITLE,
};
static esp_lv_adapter_ft_font_handle_t s_font_handles[
    sizeof(s_font_ids) / sizeof(s_font_ids[0])];
static size_t s_font_handle_count;
#endif

static void IRAM_ATTR _app_adapter_touch_interrupt(esp_lcd_touch_handle_t touch,
        void *user_ctx)
{
    (void)touch;
    (void)user_ctx;
    (void)app_manager_control_notify_touch_activity_from_isr();
}

static esp_err_t _app_adapter_refresh_on_ui(void *arg);

static esp_err_t _app_adapter_enable_touch_on_ui(void *arg)
{
    (void)arg;
    if (s_touch == NULL || !app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
    app_manager_back_gesture_screen_resume();
    lv_indev_reset(s_touch, NULL);
    (void)esp_lv_adapter_touch_notify_interrupt(s_touch);
    lv_indev_enable(s_touch, true);
    return ESP_OK;
}

static esp_err_t _app_adapter_disable_touch_on_ui(void *arg)
{
    (void)arg;
    if (s_touch == NULL || !app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
    app_manager_back_gesture_screen_suspend();
    lv_indev_enable(s_touch, false);
    lv_indev_reset(s_touch, NULL);
    return ESP_OK;
}

static esp_err_t _app_adapter_pause_lifecycle_on_ui(void *arg)
{
    (void)arg;
    if (s_touch == NULL || !app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
    app_manager_back_gesture_screen_suspend();
    lv_indev_enable(s_touch, false);
    lv_indev_reset(s_touch, NULL);
    return app_manager_lifecycle_screen_pause();
}

static esp_err_t _app_adapter_pause_and_fence(void)
{
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        result = esp_lv_adapter_pause(-1);
        if (result != ESP_OK)
        {
            return result;
        }
        atomic_store_explicit(&s_display_fenced, false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_display_sleeping, true,
                              memory_order_release);
    }

    if (atomic_load_explicit(&s_display_fenced, memory_order_acquire))
    {
        return ESP_OK;
    }

    /* Pause closes the producer side; this no-command transaction drains all
     * queued color DMA while the adapter completion callback is still valid. */
    if (s_display != NULL && s_display_port != NULL &&
            s_display_port->panel_io != NULL)
    {
        result = esp_lcd_panel_io_tx_param(s_display_port->panel_io,
                                           -1, NULL, 0);
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_display_fenced, true,
                              memory_order_release);
    }
    return result;
}

static esp_err_t _app_adapter_restore_prepare_failure(esp_err_t primary_error)
{
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return primary_error;
    }

    const esp_err_t restore_result = app_manager_ui_call(
                                         _app_adapter_refresh_on_ui,
                                         NULL, UINT32_MAX);
    if (restore_result != ESP_OK)
    {
        return restore_result;
    }
    return primary_error;
}

static esp_err_t _app_adapter_refresh_on_ui(void *arg)
{
    const bool enable_touch = arg == NULL || *(const bool *)arg;
    if (s_display == NULL || s_touch == NULL || s_display_port == NULL ||
            !app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = app_manager_lifecycle_screen_resume();
    if (result != ESP_OK)
    {
        return result;
    }

    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    if (screen == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&s_refresh_capture_error, ESP_OK,
                          memory_order_release);
    atomic_store_explicit(&s_refresh_capture_active, true,
                          memory_order_release);
    lv_obj_invalidate(screen);
    lv_refr_now(s_display);
    atomic_store_explicit(&s_refresh_capture_active, false,
                          memory_order_release);

    /* Color transfers are queued asynchronously. A no-command parameter
     * transaction drains the queue and propagates any fence API failure. */
    result = esp_lcd_panel_io_tx_param(s_display_port->panel_io, -1, NULL, 0);
    const int captured = atomic_load_explicit(&s_refresh_capture_error,
                         memory_order_acquire);
    if (captured != ESP_OK)
    {
        result = (esp_err_t)captured;
    }
    if (result != ESP_OK)
    {
        return result;
    }

    if (enable_touch)
    {
        return _app_adapter_enable_touch_on_ui(NULL);
    }
    app_manager_back_gesture_screen_suspend();
    lv_indev_reset(s_touch, NULL);
    lv_indev_enable(s_touch, false);
    return ESP_OK;
}

esp_err_t app_manager_display_sleep_prepare(void)
{
    if (s_display == NULL || s_touch == NULL || s_display_port == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return _app_adapter_pause_and_fence();
    }

    esp_err_t result = app_manager_ui_call(_app_adapter_pause_lifecycle_on_ui,
                                           NULL, UINT32_MAX);
    if (result != ESP_OK)
    {
        return result;
    }
    result = app_manager_lifecycle_screen_access_wait();
    if (result != ESP_OK)
    {
        return result;
    }
    result = _app_adapter_pause_and_fence();
    if (result != ESP_OK)
    {
        result = _app_adapter_restore_prepare_failure(result);
        return result;
    }
    return result;
}

esp_err_t app_manager_display_sleep_recover(void)
{
    if (s_display == NULL || s_display_port == NULL ||
            s_display_port->panel == NULL || s_display_port->panel_io == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return ESP_OK;
    }
    if (!atomic_load_explicit(&s_display_fenced, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = esp_lv_adapter_resume();
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_display_fenced, false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_display_sleeping, false, memory_order_release);
    }
    return result;
}

esp_err_t app_manager_display_refresh_and_enable_touch(void)
{
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    return app_manager_ui_call(_app_adapter_refresh_on_ui, NULL, UINT32_MAX);
}

esp_err_t app_manager_display_refresh_without_touch(void)
{
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool enable_touch = false;
    return app_manager_ui_call(_app_adapter_refresh_on_ui,
                               (void *)&enable_touch, UINT32_MAX);
}

esp_err_t app_manager_display_enable_touch(void)
{
    if (s_display == NULL || s_touch == NULL ||
            atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    return app_manager_ui_call(_app_adapter_enable_touch_on_ui,
                               NULL, UINT32_MAX);
}

esp_err_t app_manager_display_commit_initial(void)
{
    if (s_display == NULL || s_touch == NULL ||
            atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool enable_touch = false;
    esp_err_t result = app_manager_ui_call(_app_adapter_refresh_on_ui,
                                           (void *)&enable_touch, UINT32_MAX);
    if (result == ESP_OK)
    {
        result = app_manager_control_commit_display();
    }
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_initial_display_committed, true,
                              memory_order_release);
    }
    return result;
}

esp_err_t app_manager_display_enable_startup_touch(void)
{
    if (!atomic_load_explicit(&s_initial_display_committed,
                              memory_order_acquire) ||
            atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        return ESP_ERR_INVALID_STATE;
    }
    return app_manager_ui_call(_app_adapter_enable_touch_on_ui,
                               NULL, UINT32_MAX);
}

esp_err_t app_manager_display_disable_startup_touch(void)
{
    return app_manager_ui_call(_app_adapter_disable_touch_on_ui,
                               NULL, UINT32_MAX);
}

bool app_manager_display_is_sleeping(void)
{
    return atomic_load_explicit(&s_display_sleeping, memory_order_acquire);
}

esp_err_t app_manager_get_image(uint32_t semantic_id,
                                const lv_image_dsc_t **image)
{
    if (semantic_id == 0U || image == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&s_state) != APP_ADAPTER_READY)
    {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t index = 0U; index < s_image_resource_count; ++index)
    {
        if (s_image_resources[index].semantic_id == semantic_id)
        {
            *image = &s_image_resources[index].image;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
static esp_err_t _app_adapter_mount_resources(
    const app_manager_config_t *config)
{
    const mmap_assets_config_t mmap_config =
    {
        .partition_label = APP_RESOURCE_PARTITION_LABEL,
        .max_files = config->resource_file_count,
        .checksum = config->resource_checksum,
        .flags = {.mmap_enable = true},
    };

    esp_err_t result = mmap_assets_new(&mmap_config, &s_resource_assets);
    if (result != ESP_OK)
    {
        return result;
    }

    size_t file_count = mmap_assets_get_stored_files(s_resource_assets);
    if (file_count == 0)
    {
        if (mmap_assets_del(s_resource_assets) == ESP_OK)
        {
            s_resource_assets = NULL;
        }
        return ESP_ERR_NOT_FOUND;
    }

    const fs_cfg_t fs_config =
    {
        .fs_letter = config->res_fs_letter,
        .fs_nums = (int)file_count,
        .fs_assets = s_resource_assets,
    };
    esp_lv_fs_handle_t fs_handle = NULL;
    result = esp_lv_adapter_fs_mount(&fs_config, &fs_handle);
    if (result != ESP_OK)
    {
        if (mmap_assets_del(s_resource_assets) == ESP_OK)
        {
            s_resource_assets = NULL;
        }
        return result;
    }
    return result;
}

static esp_err_t _app_adapter_initialize_images(
    const app_manager_config_t *config)
{
    if (config->image_resource_count == 0U)
    {
        return ESP_OK;
    }
    s_image_resources = heap_caps_calloc(
                            config->image_resource_count,
                            sizeof(*s_image_resources),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_image_resources == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_image_resource_count = config->image_resource_count;
    for (size_t index = 0U; index < config->image_resource_count; ++index)
    {
        const app_manager_image_resource_config_t *binding =
            &config->image_resources[index];
        const uint8_t *memory = mmap_assets_get_mem(
                                    s_resource_assets, binding->asset_index);
        int size = mmap_assets_get_size(s_resource_assets,
                                        binding->asset_index);
        lv_image_header_t header;
        if (memory == NULL || size <= (int)sizeof(header))
        {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&header, memory, sizeof(header));
        size_t data_size = (size_t)size - sizeof(header);
        if (header.magic != LV_IMAGE_HEADER_MAGIC ||
                header.cf != LV_COLOR_FORMAT_RGB565A8 ||
                header.w != binding->width || header.h != binding->height ||
                (header.flags & LV_IMAGE_FLAGS_COMPRESSED) != 0U ||
                (header.stride & 1U) != 0U ||
                header.stride < (uint32_t)header.w * 2U)
        {
            return ESP_ERR_INVALID_ARG;
        }
        if (header.stride > SIZE_MAX / header.h)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t expected_data_size = (size_t)header.stride * header.h;
        size_t alpha_stride = header.stride / 2U;
        if (alpha_stride > (SIZE_MAX - expected_data_size) / header.h)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        expected_data_size += alpha_stride * header.h;
        if (data_size != expected_data_size || expected_data_size > UINT32_MAX)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t previous = 0U; previous < index; ++previous)
        {
            if (s_image_resources[previous].semantic_id ==
                    binding->semantic_id)
            {
                return ESP_ERR_INVALID_ARG;
            }
        }
        s_image_resources[index].semantic_id = binding->semantic_id;
        s_image_resources[index].image = (lv_image_dsc_t)
        {
            .header = header,
            .data_size = (uint32_t)expected_data_size,
            .data = memory + sizeof(header),
        };
    }
    return ESP_OK;
}
#endif

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE
static esp_err_t _app_adapter_load_fonts(char drive_letter,
        const uint16_t *font_sizes,
        size_t font_count)
{
    if (font_sizes == NULL ||
            font_count != sizeof(s_font_ids) / sizeof(s_font_ids[0]))
    {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    if (drive_letter == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const char font_path[] =
    {
        drive_letter, ':', 'f', 'o', 'n', 't', '.', 't', 't', 'f', '\0'
    };

    for (size_t i = 0; i < font_count; i++)
    {
        const esp_lv_adapter_ft_font_config_t font_config =
        {
            .name = font_path,
            .size = font_sizes[i],
            .style = ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL,
            .mem = NULL,
            .mem_size = 0,
        };
        esp_lv_adapter_ft_font_handle_t handle = NULL;
        esp_err_t result = esp_lv_adapter_ft_font_init(&font_config, &handle);
        if (result != ESP_OK)
        {
            return result;
        }
        s_font_handles[i] = handle;
        s_font_handle_count = i + 1;
        const lv_font_t *font = esp_lv_adapter_ft_font_get(handle);
        if (font == NULL)
        {
            return ESP_FAIL;
        }
        result = app_theme_set_font(s_font_ids[i], font);
        if (result != ESP_OK)
        {
            return result;
        }
    }
#else
    (void)drive_letter;
    (void)font_sizes;
    (void)font_count;
    return ESP_ERR_NOT_SUPPORTED;
#endif
    return ESP_OK;
}
#endif

static esp_err_t _app_adapter_presentation_init_on_ui(void *arg)
{
    (void)arg;
    return app_manager_presentation_init();
}

static esp_err_t _app_adapter_gesture_init_on_ui(void *arg)
{
    (void)arg;
    return app_manager_back_gesture_init(s_display, s_touch);
}

static esp_err_t _app_adapter_runtime_deinit_on_ui(void *arg)
{
    (void)arg;
    esp_err_t result = ESP_OK;

    if (s_lifecycle_ready)
    {
        result = app_manager_lifecycle_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_lifecycle_ready = false;
    }
    if (s_gesture_ready)
    {
        result = app_manager_back_gesture_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_gesture_ready = false;
    }
    if (s_presentation_ready)
    {
        result = app_manager_presentation_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_presentation_ready = false;
    }
    return result;
}

static esp_err_t _app_adapter_cleanup_services(void)
{
    esp_err_t result = ESP_OK;
    /* CONTROL_STOP restarts a sleeping LVGL worker without resuming pages.
     * Keep the adapter alive until control teardown completes. */
    if (s_control_ready)
    {
        result = app_manager_control_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_control_ready = false;
    }
    if (s_gesture_ready)
    {
        result = app_manager_back_gesture_shutdown_begin();
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (s_navigation_ready)
    {
        result = app_manager_navigation_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_navigation_ready = false;
    }
    if (s_lifecycle_ready && s_mailbox_ready)
    {
        result = app_manager_lifecycle_shutdown_begin_and_wait();
        if (result != ESP_OK)
        {
            return result;
        }
        result = app_manager_mailbox_shutdown_call(
                     _app_adapter_runtime_deinit_on_ui, NULL);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    else if (s_lifecycle_ready)
    {
        result = app_manager_lifecycle_shutdown_begin_and_wait();
        if (result != ESP_OK)
        {
            return result;
        }
        app_manager_lifecycle_force_deinit();
        s_lifecycle_ready = false;
    }
    if (s_presentation_ready && s_mailbox_ready)
    {
        result = app_manager_ui_call(_app_adapter_runtime_deinit_on_ui,
                                     NULL, UINT32_MAX);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    if (s_presentation_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_registry_ready)
    {
        app_manager_builtin_registry_reset();
        s_registry_ready = false;
    }
    if (s_mailbox_ready)
    {
        result = app_manager_mailbox_deinit();
        if (result != ESP_OK)
        {
            return result;
        }
        s_mailbox_ready = false;
    }
    return result;
}

static void _app_adapter_cleanup_theme(void)
{
    if (s_theme_ready)
    {
        app_theme_deinit();
        s_theme_ready = false;
    }
}

static esp_err_t _app_adapter_cleanup_fonts(void)
{
    esp_err_t result = ESP_OK;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE
    if (s_font_handle_count > 0)
    {
        result = esp_lv_adapter_lock(-1);
        if (result != ESP_OK)
        {
            return result;
        }

        /* File-backed faces must close while the adapter VFS is mounted. */
        while (s_font_handle_count > 0)
        {
            size_t index = s_font_handle_count - 1;
            result = esp_lv_adapter_ft_font_deinit(s_font_handles[index]);
            if (result != ESP_OK)
            {
                break;
            }
            s_font_handles[index] = NULL;
            s_font_handle_count = index;
        }
        esp_lv_adapter_unlock();
        if (result != ESP_OK)
        {
            return result;
        }
    }
#endif
    return result;
}

static void _app_adapter_clear_lvgl_runtime(void)
{
    s_adapter_ready = false;
    s_panel_proxy_delegate = NULL;
    s_display_port = NULL;
    s_display_port_storage = (bsp_display_port_t)
    {
        0
    };
    s_display = NULL;
    s_touch = NULL;
    atomic_store_explicit(&s_display_sleeping, false, memory_order_release);
    atomic_store_explicit(&s_display_fenced, false, memory_order_release);
    atomic_store_explicit(&s_initial_display_committed, false,
                          memory_order_release);
    atomic_store_explicit(&s_refresh_capture_active, false,
                          memory_order_release);
    atomic_store_explicit(&s_refresh_capture_error, ESP_OK,
                          memory_order_release);
}

static esp_err_t _app_adapter_cleanup_lvgl(void)
{
    esp_err_t result = ESP_OK;
    if (s_adapter_ready)
    {
        if (!esp_lv_adapter_is_initialized())
        {
            _app_adapter_clear_lvgl_runtime();
            return ESP_OK;
        }

        result = _app_adapter_pause_and_fence();
        if (result != ESP_OK)
        {
            return result;
        }
        result = esp_lv_adapter_deinit();
        if (!esp_lv_adapter_is_initialized())
        {
            _app_adapter_clear_lvgl_runtime();
        }
    }
    return result;
}

static esp_err_t _app_adapter_cleanup_resources(void)
{
    esp_err_t result = ESP_OK;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    heap_caps_free(s_image_resources);
    s_image_resources = NULL;
    s_image_resource_count = 0U;
    if (s_resource_assets != NULL)
    {
        result = mmap_assets_del(s_resource_assets);
        if (result != ESP_OK)
        {
            return result;
        }
        s_resource_assets = NULL;
    }
#endif
    return result;
}

static esp_err_t _app_adapter_cleanup(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool cleanup_owned = !atomic_flag_test_and_set(&s_cleanup_busy);
    if (!cleanup_owned)
    {
        goto exit;
    }

    result = app_manager_display_diagnostics_stop();
    if (result == ESP_OK)
    {
        result = _app_adapter_cleanup_services();
    }
    if (result == ESP_OK && s_adapter_ready &&
            esp_lv_adapter_is_initialized())
    {
        result = _app_adapter_pause_and_fence();
    }
    if (result == ESP_OK)
    {
        _app_adapter_cleanup_theme();
        result = _app_adapter_cleanup_fonts();
    }
    if (result == ESP_OK)
    {
        result = _app_adapter_cleanup_lvgl();
    }
    if (result == ESP_OK)
    {
        result = _app_adapter_cleanup_resources();
    }

exit:
    if (cleanup_owned)
    {
        atomic_flag_clear(&s_cleanup_busy);
    }
    return result;
}

static bool _app_adapter_config_valid(const app_manager_config_t *config)
{
    bool resources_valid = config != NULL &&
                           config->resource_file_count > 0U &&
                           (config->image_resource_count == 0U ||
                            config->image_resources != NULL);
    if (resources_valid)
    {
        for (size_t index = 0U; index < config->image_resource_count; ++index)
        {
            const app_manager_image_resource_config_t *binding =
                &config->image_resources[index];
            if (binding->semantic_id == 0U || binding->width == 0U ||
                    binding->height == 0U ||
                    binding->asset_index >= config->resource_file_count)
            {
                resources_valid = false;
                break;
            }
        }
    }
    return resources_valid && config->disp_port != NULL &&
           config->disp_port->panel != NULL &&
           config->disp_port->panel_io != NULL &&
           config->disp_port->touch != NULL &&
           config->disp_port->transport.kind !=
           BSP_DISPLAY_TRANSPORT_UNKNOWN &&
           config->disp_port->transport.clock_hz > 0U &&
           config->disp_port->transport.max_transfer_lines > 0U &&
           config->disp_port->transport.max_transfer_lines <=
           config->disp_port->height &&
           config->disp_port->transport.dma_max_full_lines > 0U &&
           config->disp_port->transport.max_transfer_lines <=
           config->disp_port->transport.dma_max_full_lines &&
           config->disp_port->transport.transaction_queue_depth > 0U &&
           config->disp_port->transport.data_lines > 0U &&
           config->disp_port->transport.bits_per_pixel == 16U &&
           (!config->disp_port->te.enabled ||
            config->disp_port->te.gpio_num >= 0) &&
           (config->disp_port->te.enabled ||
            APP_ADAPTER_PARTIAL_BUFFER_HEIGHT <= config->disp_port->height);
}

static bool _app_adapter_display_port_equal(const bsp_display_port_t *left,
        const bsp_display_port_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
           left->height == right->height && left->panel == right->panel &&
           left->panel_io == right->panel_io && left->touch == right->touch &&
           left->touch_io == right->touch_io &&
           left->transport.kind == right->transport.kind &&
           left->transport.clock_hz == right->transport.clock_hz &&
           left->transport.max_transfer_lines ==
           right->transport.max_transfer_lines &&
           left->transport.dma_max_full_lines ==
           right->transport.dma_max_full_lines &&
           left->transport.transaction_queue_depth ==
           right->transport.transaction_queue_depth &&
           left->transport.data_lines == right->transport.data_lines &&
           left->transport.bits_per_pixel ==
           right->transport.bits_per_pixel &&
           left->transport.psram_dma_direct ==
           right->transport.psram_dma_direct &&
           left->te.enabled == right->te.enabled &&
           left->te.gpio_num == right->te.gpio_num &&
           left->te.intr_type == right->te.intr_type;
}

static bool _app_adapter_transition_equal(
    const app_manager_transition_t *left,
    const app_manager_transition_t *right)
{
    return left->effect == right->effect &&
           left->duration_ms == right->duration_ms &&
           left->reserved == right->reserved;
}

static bool _app_adapter_config_equal(const app_manager_config_t *config)
{
    const app_manager_config_t *active = &s_active_config;
    return s_active_config_valid && config != NULL &&
           _app_adapter_display_port_equal(config->disp_port,
                                           active->disp_port) &&
           config->font_sizes == active->font_sizes &&
           config->font_count == active->font_count &&
           config->res_fs_letter == active->res_fs_letter &&
           config->resource_file_count == active->resource_file_count &&
           config->resource_checksum == active->resource_checksum &&
           config->image_resources == active->image_resources &&
           config->image_resource_count == active->image_resource_count &&
           config->screen_ops.suspend == active->screen_ops.suspend &&
           config->screen_ops.resume_prepare ==
           active->screen_ops.resume_prepare &&
           config->screen_ops.resume_commit ==
           active->screen_ops.resume_commit &&
           config->screen_ops.is_suspended ==
           active->screen_ops.is_suspended &&
           config->screen_ops.is_suspend_committed ==
           active->screen_ops.is_suspend_committed &&
           config->screen_ops.set_brightness ==
           active->screen_ops.set_brightness &&
           config->screen_ops.set_brightness_temp ==
           active->screen_ops.set_brightness_temp &&
           config->screen_ops.get_brightness ==
           active->screen_ops.get_brightness &&
           config->input_ops.register_handler ==
           active->input_ops.register_handler &&
           config->input_ops.unregister_handler ==
           active->input_ops.unregister_handler &&
           config->standby_ops.request_standby ==
           active->standby_ops.request_standby &&
           config->standby_ops.cancel_standby ==
           active->standby_ops.cancel_standby &&
           config->standby_ops.is_standby_allowed ==
           active->standby_ops.is_standby_allowed &&
           config->max_resident_apps == active->max_resident_apps &&
           config->resident_policy == active->resident_policy &&
           _app_adapter_transition_equal(&config->app_forward_transition,
                                         &active->app_forward_transition) &&
           _app_adapter_transition_equal(&config->app_back_transition,
                                         &active->app_back_transition) &&
           _app_adapter_transition_equal(&config->page_forward_transition,
                                         &active->page_forward_transition) &&
           _app_adapter_transition_equal(&config->page_back_transition,
                                         &active->page_back_transition) &&
           config->control_task_priority == active->control_task_priority;
}

static void _app_adapter_reset_runtime(void)
{
    s_theme_ready = false;
    s_adapter_ready = false;
    s_mailbox_ready = false;
    s_registry_ready = false;
    s_presentation_ready = false;
    s_lifecycle_ready = false;
    s_navigation_ready = false;
    s_gesture_ready = false;
    s_control_ready = false;
    s_active_config_valid = false;
    s_active_config = (app_manager_config_t)
    {
        0
    };
    s_display_port = NULL;
    s_display_port_storage = (bsp_display_port_t)
    {
        0
    };
    s_display = NULL;
    s_touch = NULL;
    s_panel_proxy_delegate = NULL;
    atomic_store_explicit(&s_display_sleeping, false, memory_order_release);
    atomic_store_explicit(&s_display_fenced, false, memory_order_release);
    atomic_store_explicit(&s_initial_display_committed, false,
                          memory_order_release);
    atomic_store_explicit(&s_refresh_capture_active, false,
                          memory_order_release);
    atomic_store_explicit(&s_refresh_capture_error, ESP_OK,
                          memory_order_release);
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE
    s_font_handle_count = 0;
#endif
}

static esp_err_t _app_adapter_register_display(
    const bsp_display_port_t *display_port)
{
    const bool te_enabled = display_port->te.enabled;
    esp_lv_adapter_display_config_t display_config =
    {
        .panel = te_enabled ? &s_panel_proxy : display_port->panel,
        .panel_io = display_port->panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = display_port->width,
            .ver_res = display_port->height,
            .buffer_height = APP_ADAPTER_PARTIAL_BUFFER_HEIGHT,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    if (te_enabled)
    {
        s_panel_proxy_delegate = display_port->panel;
        display_config.profile.buffer_height = display_port->height;
        display_config.profile.require_double_buffer = false;
        display_config.tear_avoid_mode =
            ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC;
        display_config.te_sync = (esp_lv_adapter_te_sync_config_t)
        {
            .gpio_num = display_port->te.gpio_num,
            .time_tvdl_ms = ESP_LV_ADAPTER_TE_TVDL_DEFAULT_MS,
            .time_tvdh_ms = ESP_LV_ADAPTER_TE_TVDH_DEFAULT_MS,
            .bus_freq_hz = display_port->transport.clock_hz,
            .data_lines = display_port->transport.data_lines,
            .bits_per_pixel = display_port->transport.bits_per_pixel,
            .intr_type = display_port->te.intr_type,
            .refresh_window_percent =
                ESP_LV_ADAPTER_TE_WINDOW_PERCENT_DEFAULT,
        };
    }
    s_display = esp_lv_adapter_register_display(&display_config);
    if (s_display == NULL)
    {
        return ESP_FAIL;
    }
    lv_display_set_color_format(s_display, APP_ADAPTER_COLOR_FORMAT);
    LOG_I("display color format: %s", APP_ADAPTER_COLOR_FORMAT_NAME);

    if (te_enabled)
    {
        return ESP_OK;
    }

    const esp_lv_adapter_draw_bitmap_callbacks_t draw_callbacks =
    {
        .custom_draw_bitmap = _app_adapter_draw_bitmap,
    };
    return esp_lv_adapter_set_draw_bitmap_callbacks(
               s_display, &draw_callbacks, NULL);
}

static esp_err_t _app_adapter_register_touch(
    const bsp_display_port_t *display_port)
{
    esp_lv_adapter_touch_config_t touch_config =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_display, display_port->touch);
    touch_config.callbacks.on_interrupt = _app_adapter_touch_interrupt;
    s_touch = esp_lv_adapter_register_touch(&touch_config);
    if (s_touch == NULL)
    {
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    if (display_port->touch->config.int_gpio_num != GPIO_NUM_NC)
    {
        result = gpio_intr_disable(display_port->touch->config.int_gpio_num);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    result = esp_lv_adapter_lock(-1);
    if (result != ESP_OK)
    {
        return result;
    }
    lv_indev_enable(s_touch, false);
    lv_indev_reset(s_touch, NULL);
    esp_lv_adapter_unlock();
    return ESP_OK;
}

static esp_err_t _app_adapter_initialize_lvgl(
    const app_manager_config_t *config)
{
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size =
        CONFIG_APP_MANAGER_LVGL_WORKER_STACK_SIZE;
    adapter_config.task_core_id =
        CONFIG_APP_MANAGER_LVGL_WORKER_CORE_ID;
    adapter_config.stack_in_psram = true;
    esp_err_t result = esp_lv_adapter_init(&adapter_config);
    if (result != ESP_OK)
    {
        return result;
    }
    s_adapter_ready = true;
    s_display_port_storage = *config->disp_port;
    s_display_port = &s_display_port_storage;
    result = _app_adapter_register_display(s_display_port);
    if (result == ESP_OK)
    {
        result = _app_adapter_register_touch(s_display_port);
    }
    return result;
}

static esp_err_t _app_adapter_initialize_resources(
    const app_manager_config_t *config)
{
    esp_err_t result = ESP_OK;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    if (config->res_fs_letter == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    result = _app_adapter_mount_resources(config);
    if (result == ESP_OK)
    {
        result = _app_adapter_initialize_images(config);
    }
#endif
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE
    if (result == ESP_OK)
    {
        static const uint16_t default_font_sizes[] = APP_THEME_FONT_SIZES;
        const uint16_t *font_sizes = config->font_sizes != NULL ?
                                     config->font_sizes : default_font_sizes;
        size_t font_count = config->font_sizes != NULL ?
                            config->font_count :
                            sizeof(default_font_sizes) / sizeof(default_font_sizes[0]);
        result = _app_adapter_load_fonts(config->res_fs_letter,
                                         font_sizes, font_count);
    }
#endif
    return result;
}

static esp_err_t _app_adapter_start_services(
    const app_manager_config_t *config)
{
    esp_err_t result = esp_lv_adapter_start();
    if (result != ESP_OK)
    {
        return result;
    }
    result = app_manager_mailbox_init();
    if (result != ESP_OK)
    {
        return result;
    }
    s_mailbox_ready = true;
    result = app_manager_builtin_registry_validate();
    if (result != ESP_OK)
    {
        return result;
    }
    s_registry_ready = true;
    result = app_manager_ui_call(_app_adapter_presentation_init_on_ui,
                                 NULL, UINT32_MAX);
    if (result != ESP_OK)
    {
        return result;
    }
    s_presentation_ready = true;
    result = app_manager_lifecycle_configure(
                 config->max_resident_apps, config->resident_policy,
                 &config->app_forward_transition,
                 &config->app_back_transition,
                 &config->page_forward_transition,
                 &config->page_back_transition);
    if (result != ESP_OK)
    {
        return result;
    }
    result = app_manager_lifecycle_init();
    if (result != ESP_OK)
    {
        return result;
    }
    s_lifecycle_ready = true;
    result = app_manager_navigation_init();
    if (result != ESP_OK)
    {
        return result;
    }
    s_navigation_ready = true;
    result = app_manager_ui_call(_app_adapter_gesture_init_on_ui,
                                 NULL, UINT32_MAX);
    if (result != ESP_OK)
    {
        return result;
    }
    s_gesture_ready = true;
    s_control_ready = true;
    result = app_manager_control_init(config);
    if (result != ESP_OK)
    {
        return result;
    }
    return app_manager_display_diagnostics_start(s_display);
}

esp_err_t app_manager_init(const struct app_manager_config *config)
{
    esp_err_t result = ESP_OK;
    int state = atomic_load(&s_state);
    if (state == APP_ADAPTER_READY)
    {
        return _app_adapter_config_equal(config) ? ESP_OK :
               ESP_ERR_INVALID_STATE;
    }
    int expected = APP_ADAPTER_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_state, &expected,
                                        APP_ADAPTER_INITIALIZING))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_app_adapter_config_valid(config))
    {
        atomic_store(&s_state, APP_ADAPTER_UNINITIALIZED);
        return ESP_ERR_INVALID_ARG;
    }

    _app_adapter_reset_runtime();

    result = app_theme_init();
    if (result != ESP_OK)
    {
        atomic_store(&s_state, APP_ADAPTER_UNINITIALIZED);
        return result;
    }
    s_theme_ready = true;

    result = _app_adapter_initialize_lvgl(config);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = _app_adapter_initialize_resources(config);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = _app_adapter_start_services(config);
    if (result != ESP_OK)
    {
        goto fail;
    }

    s_active_config = *config;
    s_active_config.disp_port = &s_display_port_storage;
    s_active_config_valid = true;
    atomic_store(&s_state, APP_ADAPTER_READY);
    LOG_I("initialized: %ux%u, navigation=%u, mailbox=%u, "
          "page_arena=%u",
          s_display_port->width, s_display_port->height,
          (unsigned)CONFIG_APP_MANAGER_NAV_COMMAND_CAPACITY,
          (unsigned)CONFIG_APP_MANAGER_MAILBOX_CAPACITY,
          (unsigned)(APP_MANAGER_PAGE_STATE_BYTES *
                     APP_MANAGER_MAX_PAGE_INSTANCES));
    return ESP_OK;

fail:
    {
        const esp_err_t cleanup_result = _app_adapter_cleanup();
        atomic_store(&s_state, cleanup_result == ESP_OK ?
                     APP_ADAPTER_UNINITIALIZED : APP_ADAPTER_DEINITIALIZING);
    }
    return result;
}

esp_err_t app_manager_deinit(void)
{
    if (app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }

    int state = atomic_load(&s_state);
    if (state == APP_ADAPTER_UNINITIALIZED)
    {
        return ESP_OK;
    }
    if (state == APP_ADAPTER_INITIALIZING)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == APP_ADAPTER_READY)
    {
        int expected = APP_ADAPTER_READY;
        if (!atomic_compare_exchange_strong(&s_state, &expected,
                                            APP_ADAPTER_DEINITIALIZING))
        {
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t result = _app_adapter_cleanup();
    if (result == ESP_OK)
    {
        atomic_store(&s_state, APP_ADAPTER_UNINITIALIZED);
    }
    return result;
}

bool app_manager_is_initialized(void)
{
    return atomic_load(&s_state) == APP_ADAPTER_READY;
}
