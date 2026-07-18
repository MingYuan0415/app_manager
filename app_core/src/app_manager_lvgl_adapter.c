#define DBG_TAG "app_adapter"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_manager.h"
#include "app_manager_config.h"
#include "app_manager_control.h"
#include "app_manager_display.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_theme.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lv_adapter.h"
#include <stdatomic.h>

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE && !CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    #error "App manager FreeType integration requires the LVGL adapter filesystem"
#endif

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    #include "esp_mmap_assets.h"

    #define APP_RESOURCE_PARTITION_LABEL "res"
    #define APP_RESOURCE_FILE_COUNT      1
    #define APP_RESOURCE_CHECKSUM        0x4303U

    static mmap_assets_handle_t s_resource_assets;
#endif

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
static bool s_lifecycle_ready;
static bool s_control_ready;
static bsp_display_port_t s_display_port_storage;
static const bsp_display_port_t *s_display_port;
static lv_display_t *s_display;
static lv_indev_t *s_touch;
static atomic_bool s_display_sleeping;
static atomic_bool s_display_fenced;
/* The completion ISR only reports flush-ready. Keep the first synchronous
 * submission error in the UI worker so a failed refresh is not made visible. */
static atomic_bool s_refresh_capture_active;
static atomic_int s_refresh_capture_error;

static esp_err_t _app_adapter_draw_bitmap(lv_display_t *display,
        esp_lcd_panel_handle_t panel,
        int x_start, int y_start, int x_end, int y_end,
        const void *color_map, void *user_ctx)
{
    (void)user_ctx;
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel, x_start, y_start,
                       x_end, y_end, color_map);
    if (result != ESP_OK &&
            atomic_load_explicit(&s_refresh_capture_active,
                                 memory_order_acquire))
    {
        int expected = ESP_OK;
        (void)atomic_compare_exchange_strong_explicit(
            &s_refresh_capture_error, &expected, (int)result,
            memory_order_acq_rel, memory_order_acquire);
    }
    (void)display;
    return result;
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

static esp_err_t _app_adapter_pause_lifecycle_on_ui(void *arg)
{
    (void)arg;
    esp_err_t result = ESP_OK;
    if (s_touch == NULL || !app_manager_mailbox_is_worker())
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    lv_indev_enable(s_touch, false);
    lv_indev_reset(s_touch, NULL);
    result = app_manager_lifecycle_screen_pause();

exit:
    return result;
}

static esp_err_t _app_adapter_pause_and_fence(void)
{
    esp_err_t result = ESP_OK;
    if (!atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        result = esp_lv_adapter_pause(-1);
        if (result != ESP_OK)
        {
            goto exit;
        }
        atomic_store_explicit(&s_display_fenced, false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_display_sleeping, true,
                              memory_order_release);
    }

    if (atomic_load_explicit(&s_display_fenced, memory_order_acquire))
    {
        goto exit;
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

exit:
    return result;
}

static esp_err_t _app_adapter_restore_prepare_failure(esp_err_t primary_error)
{
    esp_err_t result = primary_error;
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        goto exit;
    }

    const esp_err_t restore_result = app_manager_ui_call(
                                         _app_adapter_refresh_on_ui,
                                         NULL, UINT32_MAX);
    if (restore_result != ESP_OK)
    {
        result = restore_result;
    }

exit:
    return result;
}

static esp_err_t _app_adapter_refresh_on_ui(void *arg)
{
    (void)arg;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_display == NULL || s_touch == NULL || s_display_port == NULL ||
            !app_manager_mailbox_is_worker())
    {
        goto exit;
    }

    result = app_manager_lifecycle_screen_resume();
    if (result != ESP_OK)
    {
        goto exit;
    }

    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    if (screen == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
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
        goto exit;
    }

    lv_indev_reset(s_touch, NULL);
    (void)esp_lv_adapter_touch_notify_interrupt(s_touch);
    lv_indev_enable(s_touch, true);

exit:
    return result;
}

esp_err_t app_manager_display_sleep_prepare(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_display == NULL || s_touch == NULL || s_display_port == NULL)
    {
        goto exit;
    }
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        result = _app_adapter_pause_and_fence();
        goto exit;
    }

    result = app_manager_ui_call(_app_adapter_pause_lifecycle_on_ui, NULL,
                                 UINT32_MAX);
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = app_manager_lifecycle_screen_access_wait();
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = _app_adapter_pause_and_fence();
    if (result != ESP_OK)
    {
        result = _app_adapter_restore_prepare_failure(result);
        goto exit;
    }

exit:
    return result;
}

esp_err_t app_manager_display_sleep_recover(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_display == NULL || s_display_port == NULL ||
            s_display_port->panel == NULL || s_display_port->panel_io == NULL)
    {
        goto exit;
    }
    if (!atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        result = ESP_OK;
        goto exit;
    }
    if (!atomic_load_explicit(&s_display_fenced, memory_order_acquire))
    {
        goto exit;
    }

    result = esp_lv_adapter_resume();
    if (result == ESP_OK)
    {
        atomic_store_explicit(&s_display_fenced, false,
                              memory_order_relaxed);
        atomic_store_explicit(&s_display_sleeping, false, memory_order_release);
    }

exit:
    return result;
}

esp_err_t app_manager_display_refresh_and_enable_touch(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        goto exit;
    }
    result = app_manager_ui_call(_app_adapter_refresh_on_ui, NULL, UINT32_MAX);

exit:
    return result;
}

esp_err_t app_manager_display_commit_initial(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_display == NULL || s_touch == NULL ||
            atomic_load_explicit(&s_display_sleeping, memory_order_acquire))
    {
        goto exit;
    }
    result = app_manager_ui_call(_app_adapter_refresh_on_ui, NULL, UINT32_MAX);

exit:
    return result;
}

bool app_manager_display_is_sleeping(void)
{
    return atomic_load_explicit(&s_display_sleeping, memory_order_acquire);
}

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
static esp_err_t _app_adapter_mount_resources(char drive_letter)
{
    const mmap_assets_config_t mmap_config =
    {
        .partition_label = APP_RESOURCE_PARTITION_LABEL,
        .max_files = APP_RESOURCE_FILE_COUNT,
        .checksum = APP_RESOURCE_CHECKSUM,
        .flags = {.mmap_enable = true},
    };

    esp_err_t result = mmap_assets_new(&mmap_config, &s_resource_assets);
    if (result != ESP_OK)
    {
        goto exit;
    }

    size_t file_count = mmap_assets_get_stored_files(s_resource_assets);
    if (file_count == 0)
    {
        if (mmap_assets_del(s_resource_assets) == ESP_OK)
        {
            s_resource_assets = NULL;
        }
        result = ESP_ERR_NOT_FOUND;
        goto exit;
    }

    const fs_cfg_t fs_config =
    {
        .fs_letter = drive_letter,
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
        goto exit;
    }

exit:
    return result;
}
#endif

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE
static esp_err_t _app_adapter_load_fonts(char drive_letter,
        const uint16_t *font_sizes,
        size_t font_count)
{
    esp_err_t result = ESP_OK;
    if (font_sizes == NULL ||
            font_count != sizeof(s_font_ids) / sizeof(s_font_ids[0]))
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }

#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    if (drive_letter == 0)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
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
        result = esp_lv_adapter_ft_font_init(&font_config, &handle);
        if (result != ESP_OK)
        {
            goto exit;
        }
        s_font_handles[i] = handle;
        s_font_handle_count = i + 1;
        const lv_font_t *font = esp_lv_adapter_ft_font_get(handle);
        if (font == NULL)
        {
            result = ESP_FAIL;
            goto exit;
        }
        result = app_theme_set_font(s_font_ids[i], font);
        if (result != ESP_OK)
        {
            goto exit;
        }
    }
#else
    (void)drive_letter;
    (void)font_sizes;
    (void)font_count;
    result = ESP_ERR_NOT_SUPPORTED;
#endif

exit:
    return result;
}
#endif

static esp_err_t _app_adapter_lifecycle_deinit_on_ui(void *arg)
{
    (void)arg;
    return app_manager_lifecycle_deinit();
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
            goto exit;
        }
        s_control_ready = false;
    }
    if (s_lifecycle_ready && s_mailbox_ready)
    {
        result = app_manager_lifecycle_shutdown_begin_and_wait();
        if (result != ESP_OK)
        {
            goto exit;
        }
        result = app_manager_mailbox_shutdown_call(
                     _app_adapter_lifecycle_deinit_on_ui, NULL);
        if (result != ESP_OK)
        {
            goto exit;
        }
        s_lifecycle_ready = false;
    }
    else if (s_lifecycle_ready)
    {
        result = app_manager_lifecycle_shutdown_begin_and_wait();
        if (result != ESP_OK)
        {
            goto exit;
        }
        result = app_manager_lifecycle_deinit();
        if (result != ESP_OK)
        {
            goto exit;
        }
        s_lifecycle_ready = false;
    }
    if (s_mailbox_ready)
    {
        result = app_manager_mailbox_deinit();
        if (result != ESP_OK)
        {
            goto exit;
        }
        s_mailbox_ready = false;
    }
exit:
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
            goto exit;
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
            goto exit;
        }
    }
#endif

exit:
    return result;
}

static void _app_adapter_clear_lvgl_runtime(void)
{
    s_adapter_ready = false;
    s_display_port = NULL;
    s_display_port_storage = (bsp_display_port_t)
    {
        0
    };
    s_display = NULL;
    s_touch = NULL;
    atomic_store_explicit(&s_display_sleeping, false, memory_order_release);
    atomic_store_explicit(&s_display_fenced, false, memory_order_release);
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
            goto exit;
        }

        result = _app_adapter_pause_and_fence();
        if (result != ESP_OK)
        {
            goto exit;
        }
        result = esp_lv_adapter_deinit();
        if (!esp_lv_adapter_is_initialized())
        {
            _app_adapter_clear_lvgl_runtime();
        }
    }

exit:
    return result;
}

static esp_err_t _app_adapter_cleanup_resources(void)
{
    esp_err_t result = ESP_OK;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    if (s_resource_assets != NULL)
    {
        result = mmap_assets_del(s_resource_assets);
        if (result != ESP_OK)
        {
            goto exit;
        }
        s_resource_assets = NULL;
    }
#endif

exit:
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

    result = _app_adapter_cleanup_services();
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
    return config != NULL && config->disp_port != NULL &&
           config->disp_port->panel != NULL &&
           config->disp_port->panel_io != NULL &&
           config->disp_port->touch != NULL;
}

static void _app_adapter_reset_runtime(void)
{
    s_theme_ready = false;
    s_adapter_ready = false;
    s_mailbox_ready = false;
    s_lifecycle_ready = false;
    s_control_ready = false;
    s_display_port = NULL;
    s_display_port_storage = (bsp_display_port_t)
    {
        0
    };
    s_display = NULL;
    s_touch = NULL;
    atomic_store_explicit(&s_display_sleeping, false, memory_order_release);
    atomic_store_explicit(&s_display_fenced, false, memory_order_release);
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
    esp_err_t result = ESP_OK;
    const esp_lv_adapter_display_config_t display_config =
    {
        .panel = display_port->panel,
        .panel_io = display_port->panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = display_port->width,
            .ver_res = display_port->height,
            .buffer_height = 60,
            .use_psram = true,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };
    s_display = esp_lv_adapter_register_display(&display_config);
    if (s_display == NULL)
    {
        result = ESP_FAIL;
        goto exit;
    }

    const esp_lv_adapter_draw_bitmap_callbacks_t draw_callbacks =
    {
        .custom_draw_bitmap = _app_adapter_draw_bitmap,
    };
    result = esp_lv_adapter_set_draw_bitmap_callbacks(
                 s_display, &draw_callbacks, NULL);

exit:
    return result;
}

static esp_err_t _app_adapter_register_touch(
    const bsp_display_port_t *display_port)
{
    esp_lv_adapter_touch_config_t touch_config =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_display, display_port->touch);
    touch_config.callbacks.on_interrupt = _app_adapter_touch_interrupt;
    s_touch = esp_lv_adapter_register_touch(&touch_config);
    return s_touch != NULL ? ESP_OK : ESP_FAIL;
}

static esp_err_t _app_adapter_initialize_lvgl(
    const app_manager_config_t *config)
{
    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.stack_in_psram = true;
    esp_err_t result = esp_lv_adapter_init(&adapter_config);
    if (result != ESP_OK)
    {
        goto exit;
    }
    s_adapter_ready = true;
    s_display_port_storage = *config->disp_port;
    s_display_port = &s_display_port_storage;
    result = _app_adapter_register_display(s_display_port);
    if (result == ESP_OK)
    {
        result = _app_adapter_register_touch(s_display_port);
    }

exit:
    return result;
}

static esp_err_t _app_adapter_initialize_resources(
    const app_manager_config_t *config)
{
    esp_err_t result = ESP_OK;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS
    if (config->res_fs_letter == 0)
    {
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    result = _app_adapter_mount_resources(config->res_fs_letter);
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

exit:
    return result;
}

static esp_err_t _app_adapter_start_services(
    const app_manager_config_t *config)
{
    esp_err_t result = esp_lv_adapter_start();
    if (result != ESP_OK)
    {
        goto exit;
    }
    result = app_manager_mailbox_init();
    if (result != ESP_OK)
    {
        goto exit;
    }
    s_mailbox_ready = true;
    result = app_manager_lifecycle_init(config->page_memory_bytes);
    if (result != ESP_OK)
    {
        goto exit;
    }
    s_lifecycle_ready = true;
    s_control_ready = true;
    result = app_manager_control_init(config);

exit:
    return result;
}

esp_err_t app_manager_init(const struct app_manager_config *config)
{
    esp_err_t result = ESP_OK;
    int state = atomic_load(&s_state);
    if (state == APP_ADAPTER_READY)
    {
        goto exit;
    }
    int expected = APP_ADAPTER_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(&s_state, &expected,
                                        APP_ADAPTER_INITIALIZING))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!_app_adapter_config_valid(config))
    {
        atomic_store(&s_state, APP_ADAPTER_UNINITIALIZED);
        result = ESP_ERR_INVALID_ARG;
        goto exit;
    }

    _app_adapter_reset_runtime();

    result = app_theme_init();
    if (result != ESP_OK)
    {
        atomic_store(&s_state, APP_ADAPTER_UNINITIALIZED);
        goto exit;
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

    atomic_store(&s_state, APP_ADAPTER_READY);
    LOG_I("initialized: %ux%u, mailbox=24, page_arena=%u",
          s_display_port->width, s_display_port->height,
          (unsigned)(config->page_memory_bytes == 0 ? 32U * 1024U :
                     config->page_memory_bytes));
    goto exit;

fail:
    {
        const esp_err_t cleanup_result = _app_adapter_cleanup();
        atomic_store(&s_state, cleanup_result == ESP_OK ?
                     APP_ADAPTER_UNINITIALIZED : APP_ADAPTER_DEINITIALIZING);
    }

exit:
    return result;
}

esp_err_t app_manager_deinit(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (app_manager_mailbox_is_worker())
    {
        goto exit;
    }

    int state = atomic_load(&s_state);
    if (state == APP_ADAPTER_UNINITIALIZED)
    {
        result = ESP_OK;
        goto exit;
    }
    if (state == APP_ADAPTER_INITIALIZING)
    {
        goto exit;
    }
    if (state == APP_ADAPTER_READY)
    {
        int expected = APP_ADAPTER_READY;
        if (!atomic_compare_exchange_strong(&s_state, &expected,
                                            APP_ADAPTER_DEINITIALIZING))
        {
            goto exit;
        }
    }

    result = _app_adapter_cleanup();
    if (result == ESP_OK)
    {
        atomic_store(&s_state, APP_ADAPTER_UNINITIALIZED);
    }

exit:
    return result;
}

bool app_manager_is_initialized(void)
{
    return atomic_load(&s_state) == APP_ADAPTER_READY;
}
