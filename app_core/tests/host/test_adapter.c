#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_manager.h"
#include "app_manager_back_gesture.h"
#include "app_manager_builtin.h"
#include "app_manager_config.h"
#include "app_manager_control.h"
#include "app_manager_display.h"
#include "app_manager_display_diagnostics_priv.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_presentation.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"

#if CONFIG_APP_MANAGER_LVGL_RGB565_SWAPPED
    #define EXPECTED_COLOR_FORMAT LV_COLOR_FORMAT_RGB565_SWAPPED
#else
    #define EXPECTED_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#endif

struct _lv_display_t
{
    int marker;
};

struct _lv_indev_t
{
    int marker;
};

struct _lv_font_t
{
    int marker;
};

struct _lv_obj_t
{
    int marker;
};

struct mmap_assets_t
{
    int marker;
};

typedef enum
{
    MOCK_THEME_INIT = 0,
    MOCK_ADAPTER_INIT,
    MOCK_REGISTER_DISPLAY,
    MOCK_REGISTER_TOUCH,
    MOCK_GPIO_INTR_DISABLE,
    MOCK_UI_CALL,
    MOCK_GET_ACTIVE_SCREEN,
    MOCK_ADAPTER_PAUSE,
    MOCK_IO_FENCE,
    MOCK_ADAPTER_RESUME,
    MOCK_MMAP_NEW,
    MOCK_MMAP_FILE_COUNT,
    MOCK_FS_MOUNT,
    MOCK_FONT_INIT,
    MOCK_FONT_GET,
    MOCK_THEME_SET_FONT,
    MOCK_ADAPTER_START,
    MOCK_MAILBOX_INIT,
    MOCK_REGISTRY_VALIDATE,
    MOCK_PRESENTATION_INIT,
    MOCK_LIFECYCLE_CONFIGURE,
    MOCK_LIFECYCLE_INIT,
    MOCK_NAVIGATION_INIT,
    MOCK_GESTURE_INIT,
    MOCK_LIFECYCLE_PAUSE,
    MOCK_LIFECYCLE_ACCESS_WAIT,
    MOCK_LIFECYCLE_RESUME,
    MOCK_CONTROL_INIT,
    MOCK_CONTROL_DEINIT,
    MOCK_GESTURE_SHUTDOWN,
    MOCK_NAVIGATION_DEINIT,
    MOCK_LIFECYCLE_SHUTDOWN_BEGIN,
    MOCK_LIFECYCLE_SHUTDOWN,
    MOCK_LIFECYCLE_DEINIT,
    MOCK_GESTURE_DEINIT,
    MOCK_PRESENTATION_DEINIT,
    MOCK_REGISTRY_RESET,
    MOCK_MAILBOX_DEINIT,
    MOCK_ADAPTER_LOCK,
    MOCK_ADAPTER_DEINIT,
    MOCK_MMAP_DEL,
    MOCK_THEME_DEINIT,
    MOCK_OP_COUNT,
    MOCK_OP_NONE = -1,
} mock_op_t;

typedef struct live_resources
{
    int theme;
    int adapter;
    int display;
    int touch;
    int fs;
    int fonts;
    int started;
    int mmap;
    int mailbox;
    int registry;
    int presentation;
    int lifecycle;
    int navigation;
    int gesture;
    int control;
} live_resources_t;

typedef struct failure_injection
{
    mock_op_t op;
    unsigned occurrence;
    unsigned seen;
    esp_err_t result;
} failure_injection_t;

typedef enum
{
    EVENT_TOUCH_DISABLE = 1,
    EVENT_TOUCH_ENABLE,
    EVENT_TOUCH_RESET,
    EVENT_LIFECYCLE_PAUSE,
    EVENT_ADAPTER_PAUSE,
    EVENT_IO_FENCE,
    EVENT_ADAPTER_RESUME,
    EVENT_LIFECYCLE_RESUME,
    EVENT_GET_ACTIVE_SCREEN,
    EVENT_INVALIDATE,
    EVENT_REFRESH,
    EVENT_TOUCH_NOTIFY,
    EVENT_CONTROL_DEINIT,
    EVENT_DISPLAY_COMMIT,
    EVENT_LIFECYCLE_SHUTDOWN,
    EVENT_LIFECYCLE_DEINIT,
    EVENT_MAILBOX_DEINIT,
    EVENT_THEME_DEINIT,
    EVENT_ADAPTER_DEINIT,
} adapter_event_t;

static live_resources_t s_live;
static failure_injection_t s_failure;
static unsigned s_calls[MOCK_OP_COUNT];
static bool s_order_error;
static bool s_contract_error;
static bool s_state_observation_error;
static bool s_ui_worker;
static bool s_adapter_paused;
static bool s_paused_fence_complete;
static bool s_lifecycle_paused;
static bool s_lifecycle_access_drained;
static bool s_lifecycle_admission_closed;
static bool s_gesture_accepting;
static bool s_gesture_suspended;
static char s_expected_drive_letter;
static unsigned s_lvgl_lock_depth;
static unsigned s_mmap_del_failures_remaining;
static unsigned s_checks;
static unsigned s_failures;
static esp_lv_adapter_touch_callbacks_t s_touch_callbacks;
static adapter_event_t s_events[32];
static size_t s_event_count;
static unsigned s_touch_notifications;
static esp_err_t s_draw_bitmap_result;
static unsigned s_panel_draw_calls;
static unsigned s_fence_calls;
static unsigned s_display_commit_calls;
static unsigned s_draw_callback_registrations;
static unsigned s_initial_touch_disable_calls;
static unsigned s_gpio_disable_calls;
static esp_lv_adapter_draw_bitmap_callbacks_t s_draw_callbacks;
static esp_lcd_panel_handle_t s_registered_panel;
static esp_lv_adapter_tear_avoid_mode_t s_registered_tear_mode;
static bool s_touch_enabled;
static bool s_touch_gpio_irq_enabled;
static bool s_touch_enabled_at_display_commit;
static bool s_touch_irq_enabled_at_display_commit;
static bool s_adapter_deinit_keeps_initialized;
static unsigned s_color_format_calls;
static lv_color_format_t s_color_format;
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    static bool s_diagnostics_ready;
    static bool s_draw_during_display_registration;
    static bool s_flush_is_last;
    static unsigned s_flush_is_last_calls;
    static unsigned s_diagnostics_record_calls;
    static lv_display_t *s_diagnostics_record_display;
    static bool s_diagnostics_record_final_flush;
    static uint32_t s_diagnostics_record_pixel_count;
    static esp_err_t s_diagnostics_record_result;
    static int64_t s_diagnostics_time_us;
#endif

static struct _lv_display_t s_display_handle;
static struct _lv_indev_t s_touch_handle;
static struct _lv_obj_t s_screen_handle;
static struct _lv_font_t s_font_handles[APP_THEME_FONT_MAX];
static struct mmap_assets_t s_assets_handle;
static int s_fs_handle;
static esp_err_t _panel_draw_bitmap(esp_lcd_panel_t *panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data);
static esp_lcd_panel_t s_panel =
{
    .draw_bitmap = _panel_draw_bitmap,
};
static int s_panel_io;
static esp_lcd_touch_t s_touch =
{
    .config = {.int_gpio_num = 12},
};

#define CHECK(condition) _test_check((condition), #condition, __LINE__)

static void _test_check(bool condition, const char *expression, int line)
{
    s_checks++;
    if (!condition)
    {
        s_failures++;
        fprintf(stderr, "line %d: CHECK(%s) failed\n", line, expression);
    }
}

static bool _mock_should_fail(mock_op_t op)
{
    s_calls[op]++;
    if (s_failure.op != op)
    {
        return false;
    }
    s_failure.seen++;
    return s_failure.seen == s_failure.occurrence;
}

static void _mock_observe_initializing_or_deinitializing(void)
{
    if (app_manager_is_initialized())
    {
        s_state_observation_error = true;
    }
}

static void _mock_inject(mock_op_t op, unsigned occurrence)
{
    s_failure.op = op;
    s_failure.occurrence = occurrence;
    s_failure.seen = 0;
    s_failure.result = ESP_FAIL;
}

static void _mock_clear_injection(void)
{
    s_failure.op = MOCK_OP_NONE;
    s_failure.occurrence = 0;
    s_failure.seen = 0;
    s_failure.result = ESP_FAIL;
}

static bool _mock_resources_are_zero(void)
{
    static const live_resources_t zero;
    return memcmp(&s_live, &zero, sizeof(s_live)) == 0;
}

static void _mock_reset(void)
{
    memset(&s_live, 0, sizeof(s_live));
    memset(s_calls, 0, sizeof(s_calls));
    _mock_clear_injection();
    s_order_error = false;
    s_contract_error = false;
    s_state_observation_error = false;
    s_ui_worker = false;
    s_adapter_paused = false;
    s_paused_fence_complete = false;
    s_lifecycle_paused = false;
    s_lifecycle_access_drained = false;
    s_lifecycle_admission_closed = false;
    s_gesture_accepting = false;
    s_gesture_suspended = true;
    s_expected_drive_letter = 'F';
    s_lvgl_lock_depth = 0;
    s_mmap_del_failures_remaining = 0;
    memset(&s_touch_callbacks, 0, sizeof(s_touch_callbacks));
    memset(s_events, 0, sizeof(s_events));
    s_event_count = 0;
    s_touch_notifications = 0;
    s_draw_bitmap_result = ESP_OK;
    s_panel_draw_calls = 0;
    s_fence_calls = 0;
    s_display_commit_calls = 0;
    s_draw_callback_registrations = 0;
    s_initial_touch_disable_calls = 0;
    s_gpio_disable_calls = 0;
    memset(&s_draw_callbacks, 0, sizeof(s_draw_callbacks));
    s_registered_panel = NULL;
    s_registered_tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT;
    s_touch_enabled = false;
    s_touch_gpio_irq_enabled = false;
    s_touch_enabled_at_display_commit = false;
    s_touch_irq_enabled_at_display_commit = false;
    s_adapter_deinit_keeps_initialized = false;
    s_color_format_calls = 0;
    s_color_format = LV_COLOR_FORMAT_UNKNOWN;
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    s_diagnostics_ready = false;
    s_draw_during_display_registration = false;
    s_flush_is_last = true;
    s_flush_is_last_calls = 0U;
    s_diagnostics_record_calls = 0U;
    s_diagnostics_record_display = NULL;
    s_diagnostics_record_final_flush = false;
    s_diagnostics_record_pixel_count = 0U;
    s_diagnostics_record_result = ESP_OK;
    s_diagnostics_time_us = 0;
#endif
}

static void _record_event(adapter_event_t event)
{
    if (s_event_count >= sizeof(s_events) / sizeof(s_events[0]))
    {
        s_contract_error = true;
        return;
    }
    s_events[s_event_count++] = event;
}

static esp_err_t _screen_suspend(void)
{
    return ESP_OK;
}

static esp_err_t _screen_resume_prepare(void)
{
    return ESP_OK;
}

static esp_err_t _screen_resume_commit(void)
{
    return ESP_OK;
}

static bool _screen_is_suspended(void)
{
    return false;
}

static bool _screen_is_suspend_committed(void)
{
    return false;
}

static esp_err_t _screen_set_brightness(uint8_t brightness)
{
    (void)brightness;
    return ESP_OK;
}

static uint8_t _screen_get_brightness(void)
{
    return UINT8_MAX;
}

static esp_err_t _input_register(app_manager_input_cb_t callback, void *user_data)
{
    (void)callback;
    (void)user_data;
    return ESP_OK;
}

static esp_err_t _input_unregister(void)
{
    return ESP_OK;
}

static esp_err_t _standby_request(void)
{
    return ESP_OK;
}

static bsp_display_port_t s_display_port =
{
    .width = 368,
    .height = 448,
    .panel = &s_panel,
    .panel_io = &s_panel_io,
    .touch = &s_touch,
    .te = {
        .enabled = true,
        .gpio_num = 13,
        .bus_freq_hz = 60000000U,
        .data_lines = 4U,
        .bits_per_pixel = 16U,
        .intr_type = 1,
    },
};

static app_manager_config_t _make_config(void)
{
    app_manager_config_t config =
    {
        .disp_port = &s_display_port,
        .font_sizes = NULL,
        .font_count = 0,
        .res_fs_letter = 'F',
        .screen_ops = {
            .suspend = _screen_suspend,
            .resume_prepare = _screen_resume_prepare,
            .resume_commit = _screen_resume_commit,
            .is_suspended = _screen_is_suspended,
            .is_suspend_committed = _screen_is_suspend_committed,
            .set_brightness = _screen_set_brightness,
            .set_brightness_temp = _screen_set_brightness,
            .get_brightness = _screen_get_brightness,
        },
        .input_ops = {
            .register_handler = _input_register,
            .unregister_handler = _input_unregister,
        },
        .standby_ops = {
            .request_standby = _standby_request,
            .cancel_standby = _standby_request,
        },
        .page_memory_bytes = 0,
    };
    return config;
}

esp_err_t app_theme_init(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (_mock_should_fail(MOCK_THEME_INIT))
    {
        return s_failure.result;
    }
    if (s_live.theme != 0)
    {
        s_contract_error = true;
    }
    s_live.theme = 1;
    return ESP_OK;
}

void app_theme_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    s_calls[MOCK_THEME_DEINIT]++;
    _record_event(EVENT_THEME_DEINIT);
    if (s_live.mailbox != 0 || s_live.registry != 0 ||
            s_live.presentation != 0 || s_live.lifecycle != 0 ||
            s_live.navigation != 0 || s_live.control != 0)
    {
        s_order_error = true;
    }
    if (s_live.adapter != 0 &&
            (!s_adapter_paused ||
             (s_live.display != 0 && !s_paused_fence_complete)))
    {
        s_order_error = true;
    }
    s_live.theme = 0;
}

esp_err_t app_theme_set_font(app_theme_font_id_t id, const lv_font_t *font)
{
    _mock_observe_initializing_or_deinitializing();
    if (id < APP_THEME_FONT_BODY || id >= APP_THEME_FONT_MAX || font == NULL)
    {
        s_contract_error = true;
    }
    return _mock_should_fail(MOCK_THEME_SET_FONT) ? s_failure.result : ESP_OK;
}

esp_err_t esp_lv_adapter_init(const esp_lv_adapter_config_t *config)
{
    _mock_observe_initializing_or_deinitializing();
    if (config == NULL || !config->stack_in_psram)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_ADAPTER_INIT))
    {
        return s_failure.result;
    }
    s_live.adapter = 1;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data)
{
    if (panel == NULL || panel->draw_bitmap == NULL)
    {
        s_contract_error = true;
        return ESP_ERR_INVALID_ARG;
    }
    return panel->draw_bitmap(panel, x_start, y_start,
                              x_end, y_end, color_data);
}

static esp_err_t _panel_draw_bitmap(esp_lcd_panel_t *panel,
                                    int x_start, int y_start,
                                    int x_end, int y_end,
                                    const void *color_data)
{
    (void)x_start;
    (void)y_start;
    (void)x_end;
    (void)y_end;
    (void)color_data;
    if (panel != &s_panel)
    {
        s_contract_error = true;
    }
    s_panel_draw_calls++;
    return s_draw_bitmap_result;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    if (gpio_num != s_touch.config.int_gpio_num ||
            !s_touch_gpio_irq_enabled)
    {
        s_contract_error = true;
    }
    s_gpio_disable_calls++;
    if (_mock_should_fail(MOCK_GPIO_INTR_DISABLE))
    {
        return s_failure.result;
    }
    s_touch_gpio_irq_enabled = false;
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,
                                    int lcd_cmd,
                                    const void *param,
                                    size_t param_size)
{
    if (io != &s_panel_io || lcd_cmd != -1 || param != NULL ||
            param_size != 0 || (!s_ui_worker && !s_adapter_paused))
    {
        s_contract_error = true;
    }
    _record_event(EVENT_IO_FENCE);
    s_fence_calls++;
    if (_mock_should_fail(MOCK_IO_FENCE))
    {
        return s_failure.result;
    }
    if (s_adapter_paused)
    {
        s_paused_fence_complete = true;
    }
    return ESP_OK;
}

esp_err_t esp_lv_adapter_set_draw_bitmap_callbacks(
    lv_display_t *display,
    const esp_lv_adapter_draw_bitmap_callbacks_t *callbacks,
    void *user_ctx)
{
    (void)user_ctx;
    if (display != &s_display_handle || callbacks == NULL ||
            callbacks->custom_draw_bitmap == NULL ||
            s_registered_tear_mode != ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT)
    {
        s_contract_error = true;
    }
    if (callbacks != NULL)
    {
        s_draw_callbacks = *callbacks;
    }
    s_draw_callback_registrations++;
    return ESP_OK;
}

lv_display_t *esp_lv_adapter_register_display(
    const esp_lv_adapter_display_config_t *config)
{
    _mock_observe_initializing_or_deinitializing();
    const bool te_config_valid = s_display_port.te.enabled ?
                                 config != NULL &&
                                 config->tear_avoid_mode ==
                                 ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC &&
                                 config->te_sync.gpio_num ==
                                 s_display_port.te.gpio_num &&
                                 config->te_sync.time_tvdl_ms ==
                                 ESP_LV_ADAPTER_TE_TVDL_DEFAULT_MS &&
                                 config->te_sync.time_tvdh_ms ==
                                 ESP_LV_ADAPTER_TE_TVDH_DEFAULT_MS &&
                                 config->te_sync.bus_freq_hz ==
                                 s_display_port.te.bus_freq_hz &&
                                 config->te_sync.data_lines ==
                                 s_display_port.te.data_lines &&
                                 config->te_sync.bits_per_pixel ==
                                 s_display_port.te.bits_per_pixel &&
                                 config->te_sync.intr_type ==
                                 s_display_port.te.intr_type &&
                                 config->te_sync.refresh_window_percent ==
                                 ESP_LV_ADAPTER_TE_WINDOW_PERCENT_DEFAULT :
                                 config != NULL &&
                                 config->tear_avoid_mode ==
                                 ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT &&
                                 config->te_sync.gpio_num == -1;
    const bool panel_valid = config != NULL &&
                             (s_display_port.te.enabled ?
                              config->panel != NULL &&
                              config->panel != s_display_port.panel &&
                              config->panel->draw_bitmap != NULL :
                              config->panel == s_display_port.panel);
    const bool profile_valid = config != NULL &&
                               (s_display_port.te.enabled ?
                                config->profile.buffer_height ==
                                s_display_port.height &&
                                !config->profile.require_double_buffer :
                                config->profile.buffer_height ==
                                CONFIG_APP_MANAGER_LVGL_PARTIAL_BUFFER_HEIGHT &&
                                config->profile.require_double_buffer);
    if (config == NULL || !panel_valid ||
            config->panel_io != s_display_port.panel_io ||
            config->profile.hor_res != s_display_port.width ||
            config->profile.ver_res != s_display_port.height ||
            !config->profile.use_psram ||
            !profile_valid ||
            !te_config_valid)
    {
        s_contract_error = true;
    }
    if (config != NULL)
    {
        s_registered_panel = config->panel;
        s_registered_tear_mode = config->tear_avoid_mode;
    }
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    if (s_draw_during_display_registration && config != NULL &&
            s_display_port.te.enabled &&
            esp_lcd_panel_draw_bitmap(config->panel, 0, 0, 16, 8, NULL) !=
            ESP_OK)
    {
        s_contract_error = true;
    }
#endif
    if (_mock_should_fail(MOCK_REGISTER_DISPLAY))
    {
        return NULL;
    }
    s_color_format_calls = 0;
    s_color_format = LV_COLOR_FORMAT_UNKNOWN;
    s_live.display = 1;
    return &s_display_handle;
}

lv_indev_t *esp_lv_adapter_register_touch(
    const esp_lv_adapter_touch_config_t *config)
{
    _mock_observe_initializing_or_deinitializing();
    if (config == NULL || config->disp != &s_display_handle ||
            config->handle != s_display_port.touch ||
            config->callbacks.on_interrupt == NULL ||
            s_color_format_calls != 1U ||
            s_color_format != EXPECTED_COLOR_FORMAT)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_REGISTER_TOUCH))
    {
        return NULL;
    }
    s_touch_callbacks = config->callbacks;
    s_live.touch = 1;
    s_touch_enabled = true;
    s_touch_gpio_irq_enabled =
        s_touch.config.int_gpio_num != GPIO_NUM_NC;
    return &s_touch_handle;
}

void lv_display_set_color_format(lv_display_t *display,
                                 lv_color_format_t color_format)
{
    if (display != &s_display_handle || s_live.display == 0 ||
            color_format != EXPECTED_COLOR_FORMAT)
    {
        s_contract_error = true;
    }
    s_color_format_calls++;
    s_color_format = color_format;
}

#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
bool lv_display_flush_is_last(lv_display_t *display)
{
    if (display != &s_display_handle || s_live.display == 0)
    {
        s_contract_error = true;
    }
    s_flush_is_last_calls++;
    return s_flush_is_last;
}

int64_t esp_timer_get_time(void)
{
    s_diagnostics_time_us += 100;
    return s_diagnostics_time_us;
}

esp_err_t app_manager_display_diagnostics_start(lv_display_t *display)
{
    if (display != &s_display_handle || s_live.display == 0 ||
            s_diagnostics_ready)
    {
        s_contract_error = true;
        return ESP_ERR_INVALID_STATE;
    }
    s_diagnostics_ready = true;
    return ESP_OK;
}

esp_err_t app_manager_display_diagnostics_stop(void)
{
    s_diagnostics_ready = false;
    return ESP_OK;
}

void app_manager_display_diagnostics_record_submit(lv_display_t *display,
        bool final_flush, uint32_t pixel_count, uint32_t wait_us,
        esp_err_t result)
{
    (void)wait_us;
    if (!s_diagnostics_ready)
    {
        return;
    }
    s_diagnostics_record_calls++;
    s_diagnostics_record_display = display;
    s_diagnostics_record_final_flush = final_flush;
    s_diagnostics_record_pixel_count = pixel_count;
    s_diagnostics_record_result = result;
}
#endif

esp_err_t esp_lv_adapter_pause(int32_t timeout_ms)
{
    if (s_live.adapter == 0 || s_ui_worker || timeout_ms != -1 ||
            (s_lifecycle_paused && !s_lifecycle_access_drained))
    {
        s_contract_error = true;
    }
    _record_event(EVENT_ADAPTER_PAUSE);
    if (_mock_should_fail(MOCK_ADAPTER_PAUSE))
    {
        return s_failure.result;
    }
    s_adapter_paused = true;
    s_paused_fence_complete = false;
    return ESP_OK;
}

esp_err_t esp_lv_adapter_resume(void)
{
    if (s_live.adapter == 0 || s_ui_worker || !s_adapter_paused)
    {
        s_contract_error = true;
    }
    _record_event(EVENT_ADAPTER_RESUME);
    if (_mock_should_fail(MOCK_ADAPTER_RESUME))
    {
        return s_failure.result;
    }
    s_adapter_paused = false;
    s_paused_fence_complete = false;
    return ESP_OK;
}

lv_obj_t *lv_display_get_screen_active(lv_display_t *display)
{
    if (!s_ui_worker || display != &s_display_handle)
    {
        s_contract_error = true;
    }
    _record_event(EVENT_GET_ACTIVE_SCREEN);
    if (_mock_should_fail(MOCK_GET_ACTIVE_SCREEN))
    {
        return NULL;
    }
    return &s_screen_handle;
}

void lv_obj_invalidate(lv_obj_t *obj)
{
    if (!s_ui_worker || obj != &s_screen_handle)
    {
        s_contract_error = true;
    }
    _record_event(EVENT_INVALIDATE);
}

void lv_refr_now(lv_display_t *display)
{
    if (!s_ui_worker || display != &s_display_handle)
    {
        s_contract_error = true;
    }
    _record_event(EVENT_REFRESH);
    if (s_registered_tear_mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC)
    {
        (void)esp_lcd_panel_draw_bitmap(s_registered_panel,
                                        0, 0, 16, 8, NULL);
    }
    else if (s_draw_callbacks.custom_draw_bitmap != NULL)
    {
        (void)s_draw_callbacks.custom_draw_bitmap(display, s_registered_panel,
                0, 0, 16, 8, NULL, NULL);
    }
}

void lv_indev_enable(lv_indev_t *indev, bool enable)
{
    const bool initial_disable = !s_ui_worker &&
                                 s_lvgl_lock_depth == 1 &&
                                 s_live.gesture == 0 && !enable;
    if ((!s_ui_worker && !initial_disable) || indev != &s_touch_handle)
    {
        s_contract_error = true;
    }
    if (!initial_disable &&
            ((enable && s_gesture_suspended) ||
             (!enable && !s_gesture_suspended)))
    {
        s_order_error = true;
    }
    s_touch_enabled = enable;
    if (initial_disable)
    {
        s_initial_touch_disable_calls++;
    }
    else
    {
        _record_event(enable ? EVENT_TOUCH_ENABLE : EVENT_TOUCH_DISABLE);
    }
}

void lv_indev_reset(lv_indev_t *indev, lv_obj_t *obj)
{
    const bool initial_reset = !s_ui_worker &&
                               s_lvgl_lock_depth == 1 &&
                               s_live.gesture == 0;
    if ((!s_ui_worker && !initial_reset) ||
            indev != &s_touch_handle || obj != NULL)
    {
        s_contract_error = true;
    }
    if (!initial_reset)
    {
        _record_event(EVENT_TOUCH_RESET);
    }
}

bool esp_lv_adapter_touch_notify_interrupt(lv_indev_t *touch)
{
    if (!s_ui_worker || touch != &s_touch_handle)
    {
        s_contract_error = true;
    }
    _record_event(EVENT_TOUCH_NOTIFY);
    return true;
}

esp_err_t mmap_assets_new(const mmap_assets_config_t *config,
                          mmap_assets_handle_t *ret_item)
{
    _mock_observe_initializing_or_deinitializing();
    if (config == NULL || ret_item == NULL ||
            strcmp(config->partition_label, "res") != 0 ||
            config->max_files != 1 || config->checksum != 0x4303U ||
            config->flags.mmap_enable == 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_MMAP_NEW))
    {
        return s_failure.result;
    }
    s_live.mmap = 1;
    *ret_item = &s_assets_handle;
    return ESP_OK;
}

int mmap_assets_get_stored_files(mmap_assets_handle_t handle)
{
    _mock_observe_initializing_or_deinitializing();
    if (handle != &s_assets_handle || s_live.mmap == 0)
    {
        s_contract_error = true;
    }
    return _mock_should_fail(MOCK_MMAP_FILE_COUNT) ? 0 : 1;
}

const uint8_t *mmap_assets_get_mem(mmap_assets_handle_t handle, int index)
{
    static const uint8_t font_data[] = {0x00, 0x01, 0x02, 0x03};
    (void)handle;
    (void)index;
    s_contract_error = true;
    return font_data;
}

int mmap_assets_get_size(mmap_assets_handle_t handle, int index)
{
    (void)handle;
    (void)index;
    s_contract_error = true;
    return 4;
}

esp_err_t esp_lv_adapter_fs_mount(const fs_cfg_t *config,
                                  esp_lv_fs_handle_t *ret_handle)
{
    _mock_observe_initializing_or_deinitializing();
    if (config == NULL || ret_handle == NULL ||
            config->fs_letter != s_expected_drive_letter ||
            config->fs_nums != 1 || config->fs_assets != &s_assets_handle)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_FS_MOUNT))
    {
        return s_failure.result;
    }
    s_live.fs = 1;
    *ret_handle = &s_fs_handle;
    return ESP_OK;
}

esp_err_t esp_lv_adapter_ft_font_init(
    const esp_lv_adapter_ft_font_config_t *config,
    esp_lv_adapter_ft_font_handle_t *handle)
{
    static const uint16_t expected_sizes[] = APP_THEME_FONT_SIZES;
    const char expected_path[] =
    {
        s_expected_drive_letter, ':', 'f', 'o', 'n', 't', '.', 't', 't', 'f', '\0'
    };
    _mock_observe_initializing_or_deinitializing();
    unsigned next_font = (unsigned)s_live.fonts;
    if (config == NULL || handle == NULL || next_font >= APP_THEME_FONT_MAX ||
            strcmp(config->name, expected_path) != 0 ||
            config->size != expected_sizes[next_font] ||
            config->style != ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL ||
            config->mem != NULL ||
            config->mem_size != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_FONT_INIT))
    {
        return s_failure.result;
    }
    *handle = &s_font_handles[next_font];
    s_live.fonts++;
    return ESP_OK;
}

const lv_font_t *esp_lv_adapter_ft_font_get(
    esp_lv_adapter_ft_font_handle_t handle)
{
    _mock_observe_initializing_or_deinitializing();
    bool found = false;
    for (int i = 0; i < s_live.fonts; i++)
    {
        if (handle == &s_font_handles[i])
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_FONT_GET))
    {
        return NULL;
    }
    return (const lv_font_t *)handle;
}

esp_err_t esp_lv_adapter_ft_font_deinit(
    esp_lv_adapter_ft_font_handle_t handle)
{
    _mock_observe_initializing_or_deinitializing();
    if (s_live.fonts <= 0 ||
            handle != &s_font_handles[s_live.fonts - 1] ||
            s_live.adapter == 0 || s_live.fs == 0 ||
            s_live.theme != 0 ||
            s_live.control != 0 || s_live.navigation != 0 ||
            s_live.lifecycle != 0 || s_live.presentation != 0 ||
            s_live.registry != 0 ||
            s_live.mailbox != 0 || s_lvgl_lock_depth != 1 ||
            !s_adapter_paused || !s_paused_fence_complete)
    {
        s_contract_error = true;
    }
    s_live.fonts--;
    return ESP_OK;
}

esp_err_t esp_lv_adapter_start(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (_mock_should_fail(MOCK_ADAPTER_START))
    {
        return s_failure.result;
    }
    s_live.started = 1;
    return ESP_OK;
}

esp_err_t app_manager_mailbox_init(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (_mock_should_fail(MOCK_MAILBOX_INIT))
    {
        return s_failure.result;
    }
    s_live.mailbox = 1;
    return ESP_OK;
}

esp_err_t app_manager_builtin_registry_validate(size_t max_page_memory_size)
{
    _mock_observe_initializing_or_deinitializing();
    if (max_page_memory_size !=
            ((32U * 1024U / APP_MANAGER_MAX_PAGE_INSTANCES) & ~(size_t)7U) ||
            s_live.mailbox == 0 ||
            s_live.registry != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_REGISTRY_VALIDATE))
    {
        return s_failure.result;
    }
    s_live.registry = 1;
    return ESP_OK;
}

esp_err_t app_manager_builtin_registry_reset(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (s_live.lifecycle != 0 || s_live.presentation != 0 ||
            s_live.navigation != 0 || s_live.gesture != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_REGISTRY_RESET))
    {
        return s_failure.result;
    }
    s_live.registry = 0;
    return ESP_OK;
}

esp_err_t app_manager_presentation_init(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (!s_ui_worker || s_live.registry == 0 || s_live.presentation != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_PRESENTATION_INIT))
    {
        return s_failure.result;
    }
    s_live.presentation = 1;
    return ESP_OK;
}

esp_err_t app_manager_presentation_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (!s_ui_worker || s_live.lifecycle != 0 || s_live.navigation != 0 ||
            s_live.gesture != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_PRESENTATION_DEINIT))
    {
        return s_failure.result;
    }
    s_live.presentation = 0;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_configure(
    size_t max_resident_apps, app_manager_resident_policy_t policy,
    const app_manager_transition_t *app_forward,
    const app_manager_transition_t *app_back,
    const app_manager_transition_t *page_forward,
    const app_manager_transition_t *page_back)
{
    _mock_observe_initializing_or_deinitializing();
    if (max_resident_apps != 0U || policy != APP_MANAGER_RESIDENT_REJECT ||
            app_forward == NULL || app_back == NULL ||
            page_forward == NULL || page_back == NULL ||
            s_live.registry == 0 || s_live.presentation == 0 ||
            s_live.lifecycle != 0)
    {
        s_contract_error = true;
    }
    return _mock_should_fail(MOCK_LIFECYCLE_CONFIGURE) ?
           s_failure.result : ESP_OK;
}

esp_err_t app_manager_lifecycle_init(size_t arena_bytes)
{
    _mock_observe_initializing_or_deinitializing();
    if (arena_bytes != 0 || s_live.registry == 0 ||
            s_live.presentation == 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_LIFECYCLE_INIT))
    {
        return s_failure.result;
    }
    s_live.lifecycle = 1;
    s_lifecycle_admission_closed = false;
    return ESP_OK;
}

esp_err_t app_manager_navigation_init(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (s_live.lifecycle == 0 || s_live.presentation == 0 ||
            s_live.navigation != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_NAVIGATION_INIT))
    {
        return s_failure.result;
    }
    s_live.navigation = 1;
    return ESP_OK;
}

esp_err_t app_manager_navigation_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (s_ui_worker || s_live.control != 0 || s_live.lifecycle == 0 ||
            (s_live.gesture != 0 && s_gesture_accepting))
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_NAVIGATION_DEINIT))
    {
        return s_failure.result;
    }
    s_live.navigation = 0;
    return ESP_OK;
}

esp_err_t app_manager_back_gesture_init(lv_display_t *display,
                                        lv_indev_t *touch)
{
    _mock_observe_initializing_or_deinitializing();
    if (!s_ui_worker || display != &s_display_handle ||
            touch != &s_touch_handle || s_live.navigation == 0 ||
            s_live.lifecycle == 0 || s_live.presentation == 0 ||
            s_live.gesture != 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_GESTURE_INIT))
    {
        return s_failure.result;
    }
    s_live.gesture = 1;
    s_gesture_accepting = true;
    s_gesture_suspended = false;
    return ESP_OK;
}

esp_err_t app_manager_back_gesture_shutdown_begin(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (s_ui_worker || s_live.gesture == 0 || s_live.control != 0 ||
            (s_gesture_accepting && s_live.navigation == 0))
    {
        s_contract_error = true;
    }
    s_gesture_accepting = false;
    s_gesture_suspended = true;
    return _mock_should_fail(MOCK_GESTURE_SHUTDOWN) ?
           s_failure.result : ESP_OK;
}

esp_err_t app_manager_back_gesture_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (!s_ui_worker || s_live.gesture == 0 || s_gesture_accepting ||
            s_live.navigation != 0 || s_live.lifecycle != 0 ||
            s_live.presentation == 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_GESTURE_DEINIT))
    {
        return s_failure.result;
    }
    s_live.gesture = 0;
    return ESP_OK;
}

void app_manager_back_gesture_screen_suspend(void)
{
    if (!s_ui_worker || s_live.gesture == 0)
    {
        s_contract_error = true;
        return;
    }
    s_gesture_suspended = true;
}

void app_manager_back_gesture_screen_resume(void)
{
    if (!s_ui_worker || s_live.gesture == 0)
    {
        s_contract_error = true;
        return;
    }
    s_gesture_suspended = false;
}

esp_err_t app_manager_control_init(const app_manager_config_t *config)
{
    _mock_observe_initializing_or_deinitializing();
    if (config == NULL || s_live.navigation == 0 || s_live.gesture == 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_CONTROL_INIT))
    {
        /* The adapter marks control cleanup-ready before calling this port. */
        s_live.control = 1;
        return s_failure.result;
    }
    s_live.control = 1;
    return ESP_OK;
}

esp_err_t app_manager_control_commit_display(void)
{
    if (s_live.control == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ++s_display_commit_calls;
    s_touch_enabled_at_display_commit = s_touch_enabled;
    s_touch_irq_enabled_at_display_commit = s_touch_gpio_irq_enabled;
    s_touch_gpio_irq_enabled = true;
    _record_event(EVENT_DISPLAY_COMMIT);
    return ESP_OK;
}

esp_err_t app_manager_control_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    _record_event(EVENT_CONTROL_DEINIT);
    if (_mock_should_fail(MOCK_CONTROL_DEINIT))
    {
        return s_failure.result;
    }
    if (app_manager_display_is_sleeping())
    {
        esp_err_t result = app_manager_display_sleep_prepare();
        if (result != ESP_OK)
        {
            return result;
        }
        result = app_manager_display_sleep_recover();
        if (result != ESP_OK)
        {
            return result;
        }
    }
    s_live.control = 0;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_screen_pause(void)
{
    if (!s_ui_worker || s_live.lifecycle == 0 ||
            s_lifecycle_admission_closed)
    {
        s_contract_error = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lifecycle_paused)
    {
        return ESP_OK;
    }
    _record_event(EVENT_LIFECYCLE_PAUSE);
    if (_mock_should_fail(MOCK_LIFECYCLE_PAUSE))
    {
        return s_failure.result;
    }
    s_lifecycle_paused = true;
    s_lifecycle_access_drained = false;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_screen_access_wait(void)
{
    if (s_ui_worker || s_live.lifecycle == 0 || !s_lifecycle_paused ||
            s_adapter_paused)
    {
        s_contract_error = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (_mock_should_fail(MOCK_LIFECYCLE_ACCESS_WAIT))
    {
        return s_failure.result;
    }
    s_lifecycle_access_drained = true;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_shutdown_begin_and_wait(void)
{
    if (s_ui_worker || s_live.lifecycle == 0 || s_live.control != 0)
    {
        s_contract_error = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (_mock_should_fail(MOCK_LIFECYCLE_SHUTDOWN_BEGIN))
    {
        return s_failure.result;
    }
    s_lifecycle_admission_closed = true;
    return ESP_OK;
}

esp_err_t app_manager_lifecycle_screen_resume(void)
{
    if (!s_ui_worker || s_live.lifecycle == 0 ||
            s_lifecycle_admission_closed)
    {
        s_contract_error = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_lifecycle_paused)
    {
        return ESP_OK;
    }
    s_lifecycle_paused = false;
    s_lifecycle_access_drained = false;
    _record_event(EVENT_LIFECYCLE_RESUME);
    return _mock_should_fail(MOCK_LIFECYCLE_RESUME) ?
           s_failure.result : ESP_OK;
}

esp_err_t app_manager_lifecycle_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    _record_event(EVENT_LIFECYCLE_DEINIT);
    if (s_live.control != 0 || s_live.navigation != 0 || !s_ui_worker)
    {
        s_order_error = true;
    }
    if (_mock_should_fail(MOCK_LIFECYCLE_DEINIT))
    {
        return s_failure.result;
    }
    s_lifecycle_paused = false;
    s_lifecycle_access_drained = false;
    s_lifecycle_admission_closed = false;
    s_live.lifecycle = 0;
    return ESP_OK;
}

void app_manager_lifecycle_force_deinit(void)
{
    (void)app_manager_lifecycle_deinit();
}

esp_err_t app_manager_mailbox_shutdown_call(app_manager_ui_call_fn callback,
        void *arg)
{
    _mock_observe_initializing_or_deinitializing();
    _record_event(EVENT_LIFECYCLE_SHUTDOWN);
    if (_mock_should_fail(MOCK_LIFECYCLE_SHUTDOWN))
    {
        return s_failure.result;
    }
    if (callback == NULL || s_live.mailbox == 0 || s_live.lifecycle == 0 ||
            s_live.presentation == 0 || s_live.navigation != 0 ||
            (s_live.gesture != 0 && s_gesture_accepting) ||
            s_live.control != 0 || s_adapter_paused ||
            !s_lifecycle_admission_closed)
    {
        s_contract_error = true;
        return ESP_FAIL;
    }
    s_ui_worker = true;
    esp_err_t result = callback(arg);
    s_ui_worker = false;
    return result;
}

esp_err_t app_manager_mailbox_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    _record_event(EVENT_MAILBOX_DEINIT);
    if (s_live.control != 0 || s_live.navigation != 0 ||
            s_live.lifecycle != 0 || s_live.presentation != 0 ||
            s_live.gesture != 0 || s_live.registry != 0)
    {
        s_order_error = true;
    }
    if (s_adapter_paused)
    {
        s_order_error = true;
    }
    if (_mock_should_fail(MOCK_MAILBOX_DEINIT))
    {
        return s_failure.result;
    }
    s_live.mailbox = 0;
    return ESP_OK;
}

bool app_manager_mailbox_is_worker(void)
{
    return s_ui_worker;
}

esp_err_t app_manager_ui_call(app_manager_ui_call_fn callback, void *arg,
                              uint32_t timeout_ms)
{
    if (callback == NULL || timeout_ms != UINT32_MAX || s_ui_worker)
    {
        s_contract_error = true;
        return ESP_FAIL;
    }
    if (_mock_should_fail(MOCK_UI_CALL))
    {
        return s_failure.result;
    }
    s_ui_worker = true;
    esp_err_t result = callback(arg);
    s_ui_worker = false;
    return result;
}

esp_err_t esp_lv_adapter_lock(int32_t timeout_ms)
{
    _mock_observe_initializing_or_deinitializing();
    if (timeout_ms != -1 || s_live.adapter == 0)
    {
        s_contract_error = true;
    }
    if (_mock_should_fail(MOCK_ADAPTER_LOCK))
    {
        return s_failure.result;
    }
    s_lvgl_lock_depth++;
    return ESP_OK;
}

void esp_lv_adapter_unlock(void)
{
    _mock_observe_initializing_or_deinitializing();
    if (s_lvgl_lock_depth != 1)
    {
        s_contract_error = true;
        return;
    }
    s_lvgl_lock_depth--;
}

esp_err_t esp_lv_adapter_deinit(void)
{
    _mock_observe_initializing_or_deinitializing();
    _record_event(EVENT_ADAPTER_DEINIT);
    if (s_live.control != 0 || s_live.navigation != 0 ||
            s_live.lifecycle != 0 || s_live.presentation != 0 ||
            s_live.gesture != 0 || s_live.registry != 0 ||
            s_live.mailbox != 0 || s_live.fonts != 0 ||
            s_lvgl_lock_depth != 0 || !s_adapter_paused ||
            (s_live.display != 0 && !s_paused_fence_complete))
    {
        s_order_error = true;
    }
    const bool fail = _mock_should_fail(MOCK_ADAPTER_DEINIT);
    if (fail && s_adapter_deinit_keeps_initialized)
    {
        return s_failure.result;
    }
    s_live.started = 0;
    s_live.fs = 0;
    s_live.touch = 0;
    s_live.display = 0;
    s_live.adapter = 0;
    s_touch_enabled = false;
    s_touch_gpio_irq_enabled = false;
    s_adapter_paused = false;
    s_paused_fence_complete = false;
    return fail ? s_failure.result : ESP_OK;
}

bool esp_lv_adapter_is_initialized(void)
{
    return s_live.adapter != 0;
}

esp_err_t mmap_assets_del(mmap_assets_handle_t handle)
{
    _mock_observe_initializing_or_deinitializing();
    if (handle != &s_assets_handle || s_live.mmap == 0)
    {
        s_contract_error = true;
    }
    if (s_live.adapter != 0 && s_live.fs != 0)
    {
        s_order_error = true;
    }
    s_calls[MOCK_MMAP_DEL]++;
    if (s_mmap_del_failures_remaining != 0)
    {
        s_mmap_del_failures_remaining--;
        return ESP_FAIL;
    }
    if (s_failure.op == MOCK_MMAP_DEL)
    {
        s_failure.seen++;
        if (s_failure.seen == s_failure.occurrence)
        {
            return s_failure.result;
        }
    }
    s_live.mmap = 0;
    return ESP_OK;
}

bool app_manager_control_notify_touch_activity_from_isr(void)
{
    s_touch_notifications++;
    return true;
}

static void _check_clean_state(void)
{
    CHECK(!app_manager_is_initialized());
    CHECK(_mock_resources_are_zero());
    CHECK(!s_order_error);
    CHECK(!s_contract_error);
    CHECK(!s_state_observation_error);
    CHECK(!s_adapter_paused);
    CHECK(!s_paused_fence_complete);
    CHECK(!s_lifecycle_paused);
    CHECK(!s_lifecycle_access_drained);
    CHECK(!s_lifecycle_admission_closed);
    CHECK(!s_gesture_accepting);
    CHECK(s_gesture_suspended);
    CHECK(!s_touch_enabled);
    CHECK(!s_touch_gpio_irq_enabled);
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    CHECK(!s_diagnostics_ready);
#endif
}

static void _init_then_deinit_successfully(void)
{
    app_manager_config_t config = _make_config();
    unsigned font_init_calls = s_calls[MOCK_FONT_INIT];
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_is_initialized());
    CHECK(s_live.fonts == APP_THEME_FONT_MAX);
    CHECK(s_calls[MOCK_FONT_INIT] - font_init_calls == APP_THEME_FONT_MAX);
    CHECK(!s_order_error);
    CHECK(!s_contract_error);
    CHECK(!s_state_observation_error);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_invalid_config_and_ready_state(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(!app_manager_is_initialized());
    CHECK(app_manager_init(NULL) == ESP_ERR_INVALID_ARG);

    const bsp_display_port_t *valid_port = config.disp_port;
    config.disp_port = NULL;
    CHECK(app_manager_init(&config) == ESP_ERR_INVALID_ARG);
    config.disp_port = valid_port;

    bsp_display_port_t bad_port = *valid_port;
    config.disp_port = &bad_port;
    bad_port.panel = NULL;
    CHECK(app_manager_init(&config) == ESP_ERR_INVALID_ARG);
    bad_port = *valid_port;
    bad_port.panel_io = NULL;
    CHECK(app_manager_init(&config) == ESP_ERR_INVALID_ARG);
    bad_port = *valid_port;
    bad_port.touch = NULL;
    CHECK(app_manager_init(&config) == ESP_ERR_INVALID_ARG);
    CHECK(s_calls[MOCK_THEME_INIT] == 0);
    _check_clean_state();

    _mock_reset();
    config = _make_config();
    config.res_fs_letter = 0;
    CHECK(app_manager_init(&config) == ESP_ERR_INVALID_ARG);
    _check_clean_state();

    _mock_reset();
    config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_is_initialized());
    unsigned init_calls = s_calls[MOCK_THEME_INIT];
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(s_calls[MOCK_THEME_INIT] == init_calls);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_resource_drive_letter_is_forwarded(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    config.res_fs_letter = 'R';
    s_expected_drive_letter = 'R';

    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_disabled_te_uses_default_mode(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    s_display_port.te.enabled = false;

    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(s_registered_panel == &s_panel);
    CHECK(s_registered_tear_mode ==
          ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT);
    CHECK(s_color_format_calls == 1U);
    CHECK(s_color_format == EXPECTED_COLOR_FORMAT);
    CHECK(s_draw_callback_registrations == 1U);
    s_draw_bitmap_result = ESP_FAIL;
    CHECK(app_manager_display_commit_initial() == ESP_FAIL);
    CHECK(s_panel_draw_calls == 1U);
    CHECK(s_display_commit_calls == 0U);
    s_draw_bitmap_result = ESP_OK;
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
    s_display_port.te.enabled = true;
}

static void _test_te_profile_uses_proxy_and_disables_startup_touch(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();

    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(s_registered_panel != NULL);
    CHECK(s_registered_panel != &s_panel);
    CHECK(s_registered_tear_mode ==
          ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC);
    CHECK(s_color_format_calls == 1U);
    CHECK(s_color_format == EXPECTED_COLOR_FORMAT);
    CHECK(s_draw_callback_registrations == 0U);
    CHECK(s_initial_touch_disable_calls == 1U);
    CHECK(s_gpio_disable_calls == 1U);
    CHECK(!s_touch_enabled);
    CHECK(!s_touch_gpio_irq_enabled);

    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_te_register_failure_clears_proxy_after_deinit(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    _mock_inject(MOCK_REGISTER_DISPLAY, 1);

    CHECK(app_manager_init(&config) == ESP_FAIL);
    esp_lcd_panel_handle_t proxy = s_registered_panel;
    CHECK(proxy != NULL && proxy != &s_panel);
    CHECK(esp_lcd_panel_draw_bitmap(proxy, 0, 0, 16, 8, NULL) ==
          ESP_ERR_INVALID_STATE);
    _check_clean_state();
}

#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
static void _test_diagnostics_bitmap_display_contract(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    s_draw_during_display_registration = true;

    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(s_panel_draw_calls == 1U);
    CHECK(s_flush_is_last_calls == 0U);
    CHECK(s_diagnostics_record_calls == 0U);
    CHECK(app_manager_display_commit_initial() == ESP_OK);
    CHECK(s_flush_is_last_calls == 1U);
    CHECK(s_diagnostics_record_calls == 1U);
    CHECK(s_diagnostics_record_display == &s_display_handle);
    CHECK(s_diagnostics_record_final_flush);
    CHECK(s_diagnostics_record_pixel_count == 128U);
    CHECK(s_diagnostics_record_result == ESP_OK);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();

    _mock_reset();
    config = _make_config();
    s_display_port.te.enabled = false;
    s_flush_is_last = false;
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_commit_initial() == ESP_OK);
    CHECK(s_flush_is_last_calls == 1U);
    CHECK(s_diagnostics_record_calls == 1U);
    CHECK(s_diagnostics_record_display == &s_display_handle);
    CHECK(!s_diagnostics_record_final_flush);
    CHECK(s_diagnostics_record_pixel_count == 128U);
    CHECK(s_diagnostics_record_result == ESP_OK);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
    s_display_port.te.enabled = true;
}
#endif

static void _test_polling_touch_skips_gpio_disable(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    s_touch.config.int_gpio_num = GPIO_NUM_NC;

    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(s_gpio_disable_calls == 0U);
    CHECK(!s_touch_enabled);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
    s_touch.config.int_gpio_num = 12;
}

typedef struct init_failure_case
{
    mock_op_t op;
    unsigned occurrence;
    const char *name;
} init_failure_case_t;

static void _test_each_init_failure_rolls_back(void)
{
    static const init_failure_case_t cases[] =
    {
        {MOCK_THEME_INIT, 1, "theme"},
        {MOCK_ADAPTER_INIT, 1, "adapter"},
        {MOCK_REGISTER_DISPLAY, 1, "display"},
        {MOCK_REGISTER_TOUCH, 1, "touch"},
        {MOCK_GPIO_INTR_DISABLE, 1, "touch GPIO"},
        {MOCK_ADAPTER_LOCK, 1, "initial touch lock"},
        {MOCK_MMAP_NEW, 1, "mmap"},
        {MOCK_MMAP_FILE_COUNT, 1, "empty assets"},
        {MOCK_FS_MOUNT, 1, "filesystem"},
        {MOCK_FONT_INIT, 4, "font init"},
        {MOCK_FONT_GET, 4, "font handle"},
        {MOCK_THEME_SET_FONT, 4, "theme font"},
        {MOCK_ADAPTER_START, 1, "adapter start"},
        {MOCK_MAILBOX_INIT, 1, "mailbox"},
        {MOCK_REGISTRY_VALIDATE, 1, "registry"},
        {MOCK_PRESENTATION_INIT, 1, "presentation"},
        {MOCK_LIFECYCLE_CONFIGURE, 1, "lifecycle config"},
        {MOCK_LIFECYCLE_INIT, 1, "lifecycle"},
        {MOCK_NAVIGATION_INIT, 1, "navigation"},
        {MOCK_GESTURE_INIT, 1, "back gesture"},
        {MOCK_CONTROL_INIT, 1, "control"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        (void)cases[i].name;
        _mock_reset();
        app_manager_config_t config = _make_config();
        _mock_inject(cases[i].op, cases[i].occurrence);
        CHECK(app_manager_init(&config) != ESP_OK);
        CHECK(s_failure.seen == cases[i].occurrence);
        _check_clean_state();

        _mock_clear_injection();
        _init_then_deinit_successfully();
    }
}

static void _test_cleanup_failure_is_retryable(mock_op_t cleanup_op)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_is_initialized());
    const unsigned cleanup_calls = s_calls[cleanup_op];

    _mock_inject(cleanup_op, 1);
    CHECK(app_manager_deinit() == ESP_FAIL);
    CHECK(!app_manager_is_initialized());
    CHECK(!_mock_resources_are_zero());
    CHECK(!s_order_error);
    CHECK(!s_contract_error);
    CHECK(!s_state_observation_error);

    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    CHECK(s_calls[cleanup_op] == cleanup_calls + 2U);
    _check_clean_state();

    _mock_reset();
    _init_then_deinit_successfully();
}

static void _test_all_cleanup_failures_are_retryable(void)
{
    static const mock_op_t cases[] =
    {
        MOCK_CONTROL_DEINIT,
        MOCK_GESTURE_SHUTDOWN,
        MOCK_NAVIGATION_DEINIT,
        MOCK_LIFECYCLE_SHUTDOWN,
        MOCK_LIFECYCLE_DEINIT,
        MOCK_GESTURE_DEINIT,
        MOCK_PRESENTATION_DEINIT,
        MOCK_REGISTRY_RESET,
        MOCK_MAILBOX_DEINIT,
        MOCK_ADAPTER_LOCK,
        MOCK_ADAPTER_PAUSE,
        MOCK_IO_FENCE,
        MOCK_MMAP_DEL,
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        _test_cleanup_failure_is_retryable(cases[i]);
    }
}

static void _test_adapter_deinit_error_reconciles_ownership(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);

    _mock_inject(MOCK_ADAPTER_DEINIT, 1);
    CHECK(app_manager_deinit() == ESP_FAIL);
    CHECK(!app_manager_is_initialized());
    CHECK(s_live.adapter == 0);
    CHECK(s_live.display == 0);
    CHECK(s_live.mmap == 1);
    CHECK(s_calls[MOCK_ADAPTER_DEINIT] == 1);
    CHECK(!s_order_error);
    CHECK(!s_contract_error);

    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    CHECK(s_calls[MOCK_ADAPTER_DEINIT] == 1);
    _check_clean_state();
}

static void _test_init_rollback_cleanup_failure_is_retryable(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    _mock_inject(MOCK_FS_MOUNT, 1);
    s_mmap_del_failures_remaining = 2;

    CHECK(app_manager_init(&config) == ESP_FAIL);
    CHECK(!app_manager_is_initialized());
    CHECK(s_live.mmap == 1);
    CHECK(s_calls[MOCK_MMAP_DEL] == 2);
    CHECK(app_manager_init(&config) == ESP_ERR_INVALID_STATE);
    CHECK(!s_order_error);
    CHECK(!s_contract_error);
    CHECK(!s_state_observation_error);

    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    CHECK(s_calls[MOCK_MMAP_DEL] == 3);
    _check_clean_state();

    _mock_reset();
    _init_then_deinit_successfully();
}

static void _test_ui_worker_cannot_deinitialize(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_is_initialized());

    unsigned control_cleanup_calls = s_calls[MOCK_CONTROL_DEINIT];
    s_ui_worker = true;
    CHECK(app_manager_deinit() == ESP_ERR_INVALID_STATE);
    s_ui_worker = false;
    CHECK(app_manager_is_initialized());
    CHECK(s_calls[MOCK_CONTROL_DEINIT] == control_cleanup_calls);
    CHECK(!_mock_resources_are_zero());

    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_display_sleep_recovery_transaction(void)
{
    static const adapter_event_t expected[] =
    {
        EVENT_TOUCH_DISABLE,
        EVENT_TOUCH_RESET,
        EVENT_LIFECYCLE_PAUSE,
        EVENT_ADAPTER_PAUSE,
        EVENT_IO_FENCE,
        EVENT_ADAPTER_RESUME,
        EVENT_LIFECYCLE_RESUME,
        EVENT_GET_ACTIVE_SCREEN,
        EVENT_INVALIDATE,
        EVENT_REFRESH,
        EVENT_IO_FENCE,
        EVENT_TOUCH_RESET,
        EVENT_TOUCH_NOTIFY,
        EVENT_TOUCH_ENABLE,
    };

    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(s_touch_callbacks.on_interrupt != NULL);

    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(s_calls[MOCK_ADAPTER_PAUSE] == 1);
    CHECK(s_calls[MOCK_LIFECYCLE_ACCESS_WAIT] == 1);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    CHECK(s_calls[MOCK_ADAPTER_RESUME] == 1);
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(s_event_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        CHECK(s_events[i] == expected[i]);
    }
    CHECK(s_fence_calls == 2);
    s_touch_callbacks.on_interrupt(NULL, s_touch_callbacks.user_ctx);
    CHECK(s_touch_notifications == 1);
    CHECK(!s_contract_error);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_initial_display_commit_fences_before_panel_enable(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);

    CHECK(app_manager_display_commit_initial() == ESP_OK);
    CHECK(s_fence_calls == 1U);
    CHECK(s_display_commit_calls == 1U);
    CHECK(!s_touch_enabled_at_display_commit);
    CHECK(!s_touch_irq_enabled_at_display_commit);
    CHECK(!s_touch_enabled);
    CHECK(s_touch_gpio_irq_enabled);
    CHECK(s_events[s_event_count - 1U] == EVENT_DISPLAY_COMMIT);
    CHECK(s_events[s_event_count - 2U] == EVENT_TOUCH_DISABLE);
    s_event_count = 0;
    CHECK(app_manager_display_enable_startup_touch() == ESP_OK);
    CHECK(s_event_count == 3U);
    CHECK(s_events[0] == EVENT_TOUCH_RESET);
    CHECK(s_events[1] == EVENT_TOUCH_NOTIFY);
    CHECK(s_events[2] == EVENT_TOUCH_ENABLE);
    CHECK(s_touch_enabled);
    CHECK(s_touch_gpio_irq_enabled);
    s_event_count = 0;
    CHECK(app_manager_display_disable_startup_touch() == ESP_OK);
    CHECK(s_event_count == 2U);
    CHECK(s_events[0] == EVENT_TOUCH_DISABLE);
    CHECK(s_events[1] == EVENT_TOUCH_RESET);
    CHECK(!s_contract_error);

    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_initial_te_draw_no_mem_blocks_commit(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);

    s_draw_bitmap_result = ESP_ERR_NO_MEM;
    _mock_inject(MOCK_IO_FENCE, 1);
    CHECK(app_manager_display_commit_initial() == ESP_ERR_NO_MEM);
    CHECK(s_panel_draw_calls == 1U);
    CHECK(s_fence_calls == 1U);
    CHECK(s_display_commit_calls == 0U);
    CHECK(!s_touch_enabled);
    CHECK(!s_touch_gpio_irq_enabled);
    CHECK(app_manager_display_enable_startup_touch() ==
          ESP_ERR_INVALID_STATE);

    _mock_clear_injection();
    s_draw_bitmap_result = ESP_OK;
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_refresh_without_touch_requires_explicit_enable(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);

    CHECK(app_manager_display_refresh_without_touch() == ESP_OK);
    CHECK(!s_touch_enabled);
    CHECK(app_manager_display_enable_touch() == ESP_OK);
    CHECK(s_touch_enabled);

    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_panel_proxy_delegate_lifetime(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    esp_lcd_panel_handle_t proxy = s_registered_panel;
    CHECK(proxy != NULL && proxy != &s_panel);

    s_adapter_deinit_keeps_initialized = true;
    _mock_inject(MOCK_ADAPTER_DEINIT, 1);
    CHECK(app_manager_deinit() == ESP_FAIL);
    CHECK(s_live.adapter == 1);
    const unsigned draw_calls = s_panel_draw_calls;
    CHECK(esp_lcd_panel_draw_bitmap(proxy, 0, 0, 16, 8, NULL) == ESP_OK);
    CHECK(s_panel_draw_calls == draw_calls + 1U);

    s_adapter_deinit_keeps_initialized = false;
    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    CHECK(esp_lcd_panel_draw_bitmap(proxy, 0, 0, 16, 8, NULL) ==
          ESP_ERR_INVALID_STATE);
    _check_clean_state();
}

static void _test_display_port_is_copied(void)
{
    _mock_reset();
    bsp_display_port_t local_port = s_display_port;
    app_manager_config_t config = _make_config();
    config.disp_port = &local_port;

    CHECK(app_manager_init(&config) == ESP_OK);
    memset(&local_port, 0, sizeof(local_port));
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(!s_contract_error);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_display_refresh_fence_failure(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    _mock_inject(MOCK_IO_FENCE, 1);
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_FAIL);
    CHECK(s_fence_calls == 2);
    CHECK(s_event_count == 11);
    CHECK(s_events[10] == EVENT_IO_FENCE);
    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_display_refresh_draw_failure(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    s_draw_bitmap_result = ESP_FAIL;
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_FAIL);
    CHECK(s_fence_calls == 2);
    CHECK(s_event_count == 11);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_display_refresh_requires_active_screen(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);

    s_event_count = 0;
    const unsigned fence_calls = s_fence_calls;
    const unsigned panel_draw_calls = s_panel_draw_calls;
    _mock_inject(MOCK_GET_ACTIVE_SCREEN, 1);
    CHECK(app_manager_display_refresh_and_enable_touch() ==
          ESP_ERR_INVALID_STATE);
    CHECK(s_event_count == 2);
    CHECK(s_events[0] == EVENT_LIFECYCLE_RESUME);
    CHECK(s_events[1] == EVENT_GET_ACTIVE_SCREEN);
    CHECK(s_fence_calls == fence_calls);
    CHECK(s_panel_draw_calls == panel_draw_calls);

    _mock_clear_injection();
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(s_events[s_event_count - 1U] == EVENT_TOUCH_ENABLE);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_display_sleep_failures_are_retryable(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);

    _mock_inject(MOCK_UI_CALL, 1);
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(s_calls[MOCK_ADAPTER_PAUSE] == 0);
    CHECK(s_event_count == 0);

    _mock_clear_injection();
    _mock_inject(MOCK_LIFECYCLE_PAUSE, 1);
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(s_calls[MOCK_ADAPTER_PAUSE] == 0);
    CHECK(s_event_count == 3);
    CHECK(s_events[0] == EVENT_TOUCH_DISABLE);
    CHECK(s_events[1] == EVENT_TOUCH_RESET);
    CHECK(s_events[2] == EVENT_LIFECYCLE_PAUSE);
    CHECK(!s_lifecycle_paused);

    _mock_clear_injection();
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(s_event_count == 10);
    CHECK(s_events[9] == EVENT_TOUCH_ENABLE);

    s_event_count = 0;
    _mock_inject(MOCK_ADAPTER_PAUSE, 1);
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(s_event_count == 12);
    CHECK(s_events[0] == EVENT_TOUCH_DISABLE);
    CHECK(s_events[1] == EVENT_TOUCH_RESET);
    CHECK(s_events[2] == EVENT_LIFECYCLE_PAUSE);
    CHECK(s_events[3] == EVENT_ADAPTER_PAUSE);
    CHECK(s_events[4] == EVENT_LIFECYCLE_RESUME);
    CHECK(s_events[5] == EVENT_GET_ACTIVE_SCREEN);
    CHECK(s_events[6] == EVENT_INVALIDATE);
    CHECK(s_events[7] == EVENT_REFRESH);
    CHECK(s_events[8] == EVENT_IO_FENCE);
    CHECK(s_events[9] == EVENT_TOUCH_RESET);
    CHECK(s_events[10] == EVENT_TOUCH_NOTIFY);
    CHECK(s_events[11] == EVENT_TOUCH_ENABLE);
    CHECK(!s_lifecycle_paused);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    CHECK(s_calls[MOCK_ADAPTER_RESUME] == 0);

    s_event_count = 0;
    _mock_clear_injection();
    _mock_inject(MOCK_IO_FENCE, 1);
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(s_events[2] == EVENT_LIFECYCLE_PAUSE);
    CHECK(s_events[3] == EVENT_ADAPTER_PAUSE);
    CHECK(s_events[4] == EVENT_IO_FENCE);
    CHECK(s_event_count == 5);
    CHECK(app_manager_display_is_sleeping());
    CHECK(s_lifecycle_paused);

    _mock_clear_injection();
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(s_events[5] == EVENT_IO_FENCE);
    _mock_inject(MOCK_ADAPTER_RESUME, 1);
    CHECK(app_manager_display_sleep_recover() == ESP_FAIL);
    CHECK(app_manager_display_is_sleeping());
    CHECK(app_manager_display_refresh_and_enable_touch() ==
          ESP_ERR_INVALID_STATE);
    _mock_clear_injection();
    CHECK(app_manager_display_sleep_recover() == ESP_OK);

    _mock_inject(MOCK_LIFECYCLE_RESUME, 1);
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_FAIL);
    CHECK(!s_lifecycle_paused);
    CHECK(s_events[s_event_count - 1U] == EVENT_LIFECYCLE_RESUME);

    _mock_clear_injection();
    _mock_inject(MOCK_UI_CALL, 1);
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_FAIL);
    _mock_clear_injection();
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(!s_contract_error);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_resumed_pause_failure_keeps_touch_disabled(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_sleep_recover() == ESP_OK);
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);

    const unsigned pause_calls = s_calls[MOCK_ADAPTER_PAUSE];
    s_event_count = 0;
    _mock_inject(MOCK_LIFECYCLE_PAUSE, 1);
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(s_calls[MOCK_ADAPTER_PAUSE] == pause_calls);
    CHECK(s_event_count == 3);
    CHECK(s_events[0] == EVENT_TOUCH_DISABLE);
    CHECK(s_events[1] == EVENT_TOUCH_RESET);
    CHECK(s_events[2] == EVENT_LIFECYCLE_PAUSE);

    _mock_clear_injection();
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(s_events[s_event_count - 1U] == EVENT_TOUCH_ENABLE);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_pause_and_restore_double_failure(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);

    _mock_inject(MOCK_ADAPTER_PAUSE, 1);
    s_draw_bitmap_result = ESP_FAIL;
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(!app_manager_display_is_sleeping());
    CHECK(!s_lifecycle_paused);
    CHECK(s_event_count == 9);
    CHECK(s_events[8] == EVENT_IO_FENCE);

    _mock_clear_injection();
    s_draw_bitmap_result = ESP_OK;
    CHECK(app_manager_display_refresh_and_enable_touch() == ESP_OK);
    CHECK(s_events[s_event_count - 1U] == EVENT_TOUCH_ENABLE);
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_deinit_while_display_is_paused(void)
{
    static const adapter_event_t expected[] =
    {
        EVENT_CONTROL_DEINIT,
        EVENT_ADAPTER_RESUME,
        EVENT_LIFECYCLE_SHUTDOWN,
        EVENT_LIFECYCLE_DEINIT,
        EVENT_MAILBOX_DEINIT,
        EVENT_ADAPTER_PAUSE,
        EVENT_IO_FENCE,
        EVENT_THEME_DEINIT,
        EVENT_ADAPTER_DEINIT,
    };

    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_is_sleeping());

    const unsigned pause_calls = s_calls[MOCK_ADAPTER_PAUSE];
    const unsigned resume_calls = s_calls[MOCK_ADAPTER_RESUME];
    const unsigned panel_draw_calls = s_panel_draw_calls;
    const unsigned fence_calls = s_fence_calls;
    s_event_count = 0;
    CHECK(app_manager_deinit() == ESP_OK);
    CHECK(s_calls[MOCK_ADAPTER_PAUSE] == pause_calls + 1U);
    CHECK(s_calls[MOCK_ADAPTER_RESUME] == resume_calls + 1U);
    CHECK(s_panel_draw_calls == panel_draw_calls);
    CHECK(s_fence_calls == fence_calls + 1U);
    CHECK(s_event_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    {
        CHECK(s_events[i] == expected[i]);
    }
    _check_clean_state();
}

static void _test_paused_deinit_control_failure_is_retryable(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);
    CHECK(app_manager_display_sleep_prepare() == ESP_OK);
    CHECK(app_manager_display_is_sleeping());

    _mock_inject(MOCK_CONTROL_DEINIT, 1);
    CHECK(app_manager_deinit() == ESP_FAIL);
    CHECK(app_manager_display_is_sleeping());
    CHECK(s_live.mailbox == 1);
    CHECK(s_calls[MOCK_LIFECYCLE_SHUTDOWN] == 0);
    CHECK(!s_order_error);
    CHECK(!s_contract_error);

    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    _check_clean_state();
}

static void _test_paused_deinit_retries_incomplete_fence(void)
{
    _mock_reset();
    app_manager_config_t config = _make_config();
    CHECK(app_manager_init(&config) == ESP_OK);

    _mock_inject(MOCK_IO_FENCE, 1);
    CHECK(app_manager_display_sleep_prepare() == ESP_FAIL);
    CHECK(app_manager_display_is_sleeping());
    CHECK(s_lifecycle_paused);
    const unsigned fence_calls = s_fence_calls;

    _mock_clear_injection();
    CHECK(app_manager_deinit() == ESP_OK);
    CHECK(s_fence_calls == fence_calls + 2U);
    _check_clean_state();
}

int main(void)
{
    _test_invalid_config_and_ready_state();
    _test_resource_drive_letter_is_forwarded();
    _test_disabled_te_uses_default_mode();
    _test_te_profile_uses_proxy_and_disables_startup_touch();
    _test_te_register_failure_clears_proxy_after_deinit();
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
    _test_diagnostics_bitmap_display_contract();
#endif
    _test_polling_touch_skips_gpio_disable();
    _test_each_init_failure_rolls_back();
    _test_all_cleanup_failures_are_retryable();
    _test_adapter_deinit_error_reconciles_ownership();
    _test_init_rollback_cleanup_failure_is_retryable();
    _test_ui_worker_cannot_deinitialize();
    _test_display_port_is_copied();
    _test_initial_display_commit_fences_before_panel_enable();
    _test_initial_te_draw_no_mem_blocks_commit();
    _test_refresh_without_touch_requires_explicit_enable();
    _test_panel_proxy_delegate_lifetime();
    _test_display_sleep_recovery_transaction();
    _test_display_refresh_fence_failure();
    _test_display_refresh_draw_failure();
    _test_display_refresh_requires_active_screen();
    _test_display_sleep_failures_are_retryable();
    _test_resumed_pause_failure_keeps_touch_disabled();
    _test_pause_and_restore_double_failure();
    _test_deinit_while_display_is_paused();
    _test_paused_deinit_control_failure_is_retryable();
    _test_paused_deinit_retries_incomplete_fence();

    if (s_failures != 0)
    {
        fprintf(stderr, "adapter tests failed: %u/%u checks\n",
                s_failures, s_checks);
        return 1;
    }
    printf("adapter tests passed: %u checks\n", s_checks);
    return 0;
}
