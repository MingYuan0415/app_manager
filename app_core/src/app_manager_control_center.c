#define DBG_TAG "app_control_center"
#define DBG_LVL DBG_INFO

#ifndef HOST_TEST
    #include "mt_log.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "esp_attr.h"
#else
    #include <stdio.h>
    #define LOG_W(fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
#endif

#include "app_manager.h"
#include "app_manager_control_center.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_task_switcher.h"
#include "app_theme_colors.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Shared glyph semantic IDs; mirrors layers/apps/common/include/app_image_ids.h
 * (App Manager consumes product image IDs numerically, like app icon_id). */
#define CC_IMAGE_WIFI        0x5788U
#define CC_IMAGE_WIFI_OFF    0x5789U
#define CC_IMAGE_BLUETOOTH   0x5787U
#define CC_IMAGE_BRIGHTNESS  0x578AU
#define CC_IMAGE_POWER       0x578BU
#define CC_IMAGE_STANDBY     0x578CU

#define CC_BAND_HEIGHT       20
#define CC_BAND_EDGE_PX      30
#define CC_BAND_TRIGGER_PX   48
#define CC_CARD_MARGIN       12
#define CC_CARD_HEIGHT       292
#define CC_TILE_HEIGHT       76
#define CC_POLL_MS           1000U
#define CC_SCRIM_OPA         200

typedef struct app_manager_control_center
{
    bool initialized;
    bool visible;
    bool built;
    bool policy_enabled;
    int32_t width;
    int32_t height;
    int32_t press_y;
    bool band_armed;
    void *display;
    void *touch;
    app_manager_control_center_ops_t ops;
    lv_obj_t *band;
    lv_obj_t *root;
    lv_obj_t *clock_label;
    lv_obj_t *date_label;
    lv_obj_t *brightness_slider;
    lv_obj_t *wifi_button;
    lv_obj_t *wifi_icon;
    lv_obj_t *wifi_label;
    lv_obj_t *bt_button;
    lv_obj_t *bt_icon;
    lv_obj_t *bt_label;
    lv_timer_t *poll_timer;
} app_manager_control_center_t;

static app_manager_control_center_t s_cc;

static void _cc_refresh_band(void);
static void _cc_refresh_values(void);

static lv_obj_t *_cc_icon(lv_obj_t *parent, uint32_t image_id, int32_t size)
{
    const lv_image_dsc_t *dsc = NULL;
    if (app_manager_get_image(image_id, &dsc) == ESP_OK && dsc != NULL)
    {
        lv_obj_t *image = lv_image_create(parent);
        if (image != NULL)
        {
            lv_obj_set_size(image, size, size);
            lv_image_set_src(image, dsc);
            return image;
        }
    }
    lv_obj_t *symbol = lv_label_create(parent);
    if (symbol != NULL)
    {
        lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
    }
    return symbol;
}

static void _cc_icon_color(lv_obj_t *icon, uint32_t color)
{
    if (icon == NULL)
    {
        return;
    }
    lv_obj_set_style_text_color(icon, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
}

static uint32_t _cc_radio_color(app_manager_radio_state_t state)
{
    switch (state)
    {
    case APP_MANAGER_RADIO_CONNECTED:
        return APP_THEME_COLOR_MINT;
    case APP_MANAGER_RADIO_ON_IDLE:
        return APP_THEME_COLOR_AMBER;
    case APP_MANAGER_RADIO_OFF:
    default:
        return APP_THEME_COLOR_INK_SOFT;
    }
}

static void _cc_close(void)
{
    if (s_cc.root != NULL)
    {
        lv_obj_set_hidden(s_cc.root, true);
    }
    s_cc.visible = false;
    _cc_refresh_band();
}

static void _cc_root_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
            lv_event_get_target(event) != lv_event_get_current_target(event))
    {
        return;
    }
    _cc_close();
}

static void _cc_screen_off_event(lv_event_t *event)
{
    (void)event;
    _cc_close();
    if (app_manager_pm_request_screen_off() != ESP_OK)
    {
        LOG_W("screen-off request rejected");
    }
}

static void _cc_standby_event(lv_event_t *event)
{
    (void)event;
    _cc_close();
    if (app_manager_pm_request_standby() != ESP_OK)
    {
        LOG_W("standby request rejected");
    }
}

static void _cc_brightness_event(lv_event_t *event)
{
    if (s_cc.brightness_slider == NULL)
    {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    const uint8_t value =
        (uint8_t)(int32_t)lv_slider_get_value(s_cc.brightness_slider);
    if (code == LV_EVENT_VALUE_CHANGED)
    {
        (void)app_manager_screen_set_brightness_temp(value);
    }
    else if (code == LV_EVENT_RELEASED)
    {
        (void)app_manager_screen_set_brightness(value);
    }
}

static void _cc_wifi_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (s_cc.ops.get_wifi == NULL || s_cc.ops.set_wifi == NULL)
    {
        return;
    }
    app_manager_radio_state_t state = APP_MANAGER_RADIO_OFF;
    const char *ssid = NULL;
    if (s_cc.ops.get_wifi(&state, &ssid) != ESP_OK)
    {
        return;
    }
    (void)s_cc.ops.set_wifi(state == APP_MANAGER_RADIO_OFF);
    _cc_refresh_values();
}

static void _cc_bt_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (s_cc.ops.get_bluetooth == NULL || s_cc.ops.set_bluetooth == NULL)
    {
        return;
    }
    app_manager_radio_state_t state = APP_MANAGER_RADIO_OFF;
    if (s_cc.ops.get_bluetooth(&state) != ESP_OK)
    {
        return;
    }
    (void)s_cc.ops.set_bluetooth(state == APP_MANAGER_RADIO_OFF);
    _cc_refresh_values();
}

static void _cc_refresh_values(void)
{
    if (s_cc.clock_label != NULL)
    {
        const char *time_text = NULL;
        const char *date_text = NULL;
        const bool ok = s_cc.ops.get_clock != NULL &&
                        s_cc.ops.get_clock(&time_text, &date_text) == ESP_OK;
        lv_label_set_text(s_cc.clock_label,
                          ok && time_text != NULL ? time_text : "--:--");
        lv_label_set_text(s_cc.date_label,
                          ok && date_text != NULL ? date_text : "时间未同步");
    }

    if (s_cc.wifi_button != NULL)
    {
        app_manager_radio_state_t state = APP_MANAGER_RADIO_OFF;
        const char *ssid = NULL;
        const bool available = s_cc.ops.get_wifi != NULL &&
                               s_cc.ops.get_wifi(&state, &ssid) == ESP_OK;
        const uint32_t color = _cc_radio_color(available ? state :
                                               APP_MANAGER_RADIO_OFF);
        _cc_icon_color(s_cc.wifi_icon, color);
        lv_label_set_text(s_cc.wifi_label,
                          !available ? "未连接" :
                          state == APP_MANAGER_RADIO_CONNECTED ?
                          (ssid != NULL ? ssid : "已连接") :
                          state == APP_MANAGER_RADIO_ON_IDLE ?
                          "未连接" : "已关闭");
    }

    if (s_cc.bt_button != NULL)
    {
        app_manager_radio_state_t state = APP_MANAGER_RADIO_OFF;
        if (s_cc.ops.get_bluetooth != NULL &&
                s_cc.ops.get_bluetooth(&state) == ESP_OK)
        {
            _cc_icon_color(s_cc.bt_icon, _cc_radio_color(state));
            lv_label_set_text(s_cc.bt_label,
                              state == APP_MANAGER_RADIO_OFF ? "已关闭" :
                              state == APP_MANAGER_RADIO_CONNECTED ? "已连接" :
                              "已开启");
        }
    }
}

static void _cc_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_cc.visible)
    {
        _cc_refresh_values();
    }
}

static lv_obj_t *_cc_tile(lv_obj_t *parent, lv_obj_t **tile_out,
                          lv_obj_t **icon_out, lv_obj_t **label_out,
                          uint32_t image_id, lv_event_cb_t callback,
                          const char *title)
{
    lv_obj_t *tile = lv_button_create(parent);
    if (tile == NULL)
    {
        return NULL;
    }
    *tile_out = tile;
    lv_obj_set_height(tile, CC_TILE_HEIGHT);
    lv_obj_set_flex_grow(tile, 1);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_THEME_COLOR_PLUME_HI), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_THEME_COLOR_SHEET),
                              LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(tile, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = _cc_icon(tile, image_id, 26);
    if (icon != NULL)
    {
        _cc_icon_color(icon, APP_THEME_COLOR_INK_SOFT);
    }
    *icon_out = icon;

    lv_obj_t *text = lv_obj_create(tile);
    if (text != NULL)
    {
        lv_obj_remove_style_all(text);
        lv_obj_set_width(text, LV_PCT(100));
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, 1, 0);
        lv_obj_t *name = lv_label_create(text);
        lv_obj_set_style_text_color(name, lv_color_hex(APP_THEME_COLOR_INK), 0);
        lv_obj_set_style_text_font(name,
                                   app_manager_get_font(APP_THEME_FONT_BODY), 0);
        lv_label_set_text(name, title);
        lv_obj_t *state = lv_label_create(text);
        lv_obj_set_style_text_color(state,
                                    lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
        lv_obj_set_style_text_font(state,
                                   app_manager_get_font(APP_THEME_FONT_SMALL), 0);
        lv_label_set_text(state, "--");
        *label_out = state;
    }
    return tile;
}

static void _cc_build(void)
{
    const int32_t panel_w = s_cc.width - 2 * CC_CARD_MARGIN;
    const int32_t inner_w = panel_w - 24;

    lv_obj_t *panel = lv_obj_create(s_cc.root);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, CC_CARD_MARGIN, 0);
    lv_obj_set_size(panel, panel_w, CC_CARD_HEIGHT);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_THEME_COLOR_PLUME), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_scrollable(panel, false);

    s_cc.clock_label = lv_label_create(panel);
    lv_obj_set_style_text_color(s_cc.clock_label,
                                lv_color_hex(APP_THEME_COLOR_INK), 0);
    lv_obj_set_style_text_font(s_cc.clock_label,
                               app_manager_get_font(APP_THEME_FONT_BIGL), 0);
    lv_obj_align(s_cc.clock_label, LV_ALIGN_TOP_LEFT, 16, 12);
    lv_label_set_text(s_cc.clock_label, "--:--");

    s_cc.date_label = lv_label_create(panel);
    lv_obj_set_style_text_color(s_cc.date_label,
                                lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
    lv_obj_set_style_text_font(s_cc.date_label,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_obj_align(s_cc.date_label, LV_ALIGN_TOP_RIGHT, -16, 22);
    lv_label_set_text(s_cc.date_label, "时间未同步");

    /* Brightness row. */
    lv_obj_t *brightness = lv_obj_create(panel);
    lv_obj_remove_style_all(brightness);
    lv_obj_set_size(brightness, inner_w, 56);
    lv_obj_align(brightness, LV_ALIGN_TOP_LEFT, 12, 62);
    lv_obj_set_flex_flow(brightness, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightness, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(brightness, 12, 0);
    lv_obj_set_style_pad_all(brightness, 8, 0);
    lv_obj_set_style_bg_color(brightness, lv_color_hex(APP_THEME_COLOR_PLUME_HI),
                              0);
    lv_obj_set_style_bg_opa(brightness, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(brightness, 14, 0);
    lv_obj_set_scrollable(brightness, false);
    lv_obj_t *bright_icon = _cc_icon(brightness, CC_IMAGE_BRIGHTNESS, 24);
    if (bright_icon != NULL)
    {
        _cc_icon_color(bright_icon, APP_THEME_COLOR_AMBER);
    }
    s_cc.brightness_slider = lv_slider_create(brightness);
    if (s_cc.brightness_slider != NULL)
    {
        lv_obj_set_flex_grow(s_cc.brightness_slider, 1);
        lv_obj_set_height(s_cc.brightness_slider, 12);
        lv_slider_set_range(s_cc.brightness_slider, 16, 255);
        lv_slider_set_value(s_cc.brightness_slider, 128, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cc.brightness_slider,
                                  lv_color_hex(APP_THEME_COLOR_AZURE),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_cc.brightness_slider,
                                  lv_color_hex(APP_THEME_COLOR_INK), LV_PART_KNOB);
        lv_obj_add_event_cb(s_cc.brightness_slider, _cc_brightness_event,
                            LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_cc.brightness_slider, _cc_brightness_event,
                            LV_EVENT_RELEASED, NULL);
    }

    /* Wi-Fi / Bluetooth tiles. */
    lv_obj_t *tiles = lv_obj_create(panel);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, inner_w, CC_TILE_HEIGHT);
    lv_obj_align(tiles, LV_ALIGN_TOP_LEFT, 12, 128);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(tiles, 12, 0);
    lv_obj_set_scrollable(tiles, false);

    const bool has_wifi = s_cc.ops.get_wifi != NULL;
    const bool has_bt = s_cc.ops.get_bluetooth != NULL;
    if (has_wifi)
    {
        (void)_cc_tile(tiles, &s_cc.wifi_button, &s_cc.wifi_icon,
                       &s_cc.wifi_label, CC_IMAGE_WIFI, _cc_wifi_event,
                       "Wi-Fi");
    }
    if (has_bt)
    {
        (void)_cc_tile(tiles, &s_cc.bt_button, &s_cc.bt_icon,
                       &s_cc.bt_label, CC_IMAGE_BLUETOOTH, _cc_bt_event,
                       "蓝牙");
    }

    /* Power actions. */
    lv_obj_t *actions = lv_obj_create(panel);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, inner_w, 56);
    lv_obj_align(actions, LV_ALIGN_TOP_LEFT, 12, 216);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 12, 0);
    lv_obj_set_scrollable(actions, false);

    lv_obj_t *off = lv_button_create(actions);
    lv_obj_t *standby = lv_button_create(actions);
    lv_obj_t *buttons[2] = { off, standby };
    const uint32_t ids[2] = { CC_IMAGE_POWER, CC_IMAGE_STANDBY };
    const char *titles[2] = { "熄屏", "待机" };
    lv_event_cb_t cbs[2] = { _cc_screen_off_event, _cc_standby_event };
    for (size_t i = 0; i < 2U; i++)
    {
        if (buttons[i] == NULL)
        {
            continue;
        }
        lv_obj_set_height(buttons[i], 56);
        lv_obj_set_flex_grow(buttons[i], 1);
        lv_obj_set_style_radius(buttons[i], 14, 0);
        lv_obj_set_style_bg_color(buttons[i],
                                  lv_color_hex(APP_THEME_COLOR_PLUME_HI), 0);
        lv_obj_set_style_bg_color(buttons[i],
                                  lv_color_hex(APP_THEME_COLOR_SHEET),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(buttons[i], 0, 0);
        lv_obj_set_flex_flow(buttons[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(buttons[i], LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(buttons[i], 8, 0);
        lv_obj_add_event_cb(buttons[i], cbs[i], LV_EVENT_CLICKED, NULL);
        lv_obj_t *icon = _cc_icon(buttons[i], ids[i], 22);
        if (icon != NULL)
        {
            _cc_icon_color(icon, APP_THEME_COLOR_INK);
        }
        lv_obj_t *text = lv_label_create(buttons[i]);
        lv_obj_set_style_text_color(text, lv_color_hex(APP_THEME_COLOR_INK), 0);
        lv_obj_set_style_text_font(text,
                                   app_manager_get_font(APP_THEME_FONT_BODY), 0);
        lv_label_set_text(text, titles[i]);
    }
}

static bool _cc_band_ready(void)
{
    return s_cc.initialized && s_cc.policy_enabled && !s_cc.visible &&
           !app_manager_is_transitioning() &&
           app_manager_lifecycle_screen_mutation_allowed() &&
           !app_manager_task_switcher_is_visible();
}

static void _cc_refresh_band(void)
{
    if (s_cc.band == NULL)
    {
        return;
    }
    const bool ready = _cc_band_ready();
    lv_obj_set_hidden(s_cc.band, !ready);
    lv_obj_set_clickable(s_cc.band, ready);
}

static void _cc_band_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if (code == LV_EVENT_PRESSED)
    {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        s_cc.press_y = point.y;
        s_cc.band_armed = false;
    }
    else if (code == LV_EVENT_PRESSING)
    {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        s_cc.band_armed = (point.y - s_cc.press_y) >= CC_BAND_TRIGGER_PX;
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (s_cc.band_armed)
        {
            s_cc.band_armed = false;
            (void)app_manager_navigation_submit_system_op(
                APP_MANAGER_NAV_SYSTEM_CONTROL_CENTER_SHOW, NULL, NULL);
        }
    }
    else if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET)
    {
        s_cc.band_armed = false;
    }
}

void app_manager_control_center_set_ops(
    const app_manager_control_center_ops_t *ops)
{
    if (ops != NULL)
    {
        s_cc.ops = *ops;
    }
    else
    {
        memset(&s_cc.ops, 0, sizeof(s_cc.ops));
    }
}

static lv_obj_t *_cc_create_band(lv_obj_t *system_layer)
{
    lv_obj_t *band = lv_obj_create(system_layer);
    if (band == NULL)
    {
        return NULL;
    }
    lv_obj_remove_style_all(band);
    lv_obj_set_pos(band, CC_BAND_EDGE_PX, 0);
    lv_obj_set_size(band, s_cc.width - 2 * CC_BAND_EDGE_PX, CC_BAND_HEIGHT);
    lv_obj_set_press_lock(band, true);
    lv_obj_set_ignore_layout(band, true);
    lv_obj_set_floating(band, true);
    lv_obj_set_hidden(band, true);
    lv_obj_set_clickable(band, false);
    lv_obj_set_scrollable(band, false);
    lv_obj_set_scroll_elastic(band, false);
    lv_obj_set_scroll_momentum(band, false);
    lv_obj_set_scroll_chain(band, false);
    lv_obj_set_event_bubble(band, false);
    lv_obj_set_gesture_bubble(band, false);
    if (lv_obj_add_event_cb(band, _cc_band_event, LV_EVENT_ALL, NULL) == NULL)
    {
        lv_obj_delete(band);
        return NULL;
    }
    return band;
}

esp_err_t app_manager_control_center_init(void *display, void *touch)
{
    if (s_cc.initialized)
    {
        return ESP_OK;
    }
#ifndef HOST_TEST
    if (!app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (display == NULL || touch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    lv_display_t *disp = (lv_display_t *)display;
    lv_obj_t *system_layer = lv_display_get_layer_sys(disp);
    if (system_layer == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const bool ops_set = s_cc.ops.get_wifi != NULL || s_cc.ops.get_clock != NULL ||
                         s_cc.ops.get_bluetooth != NULL;
    app_manager_control_center_ops_t saved_ops = s_cc.ops;
    memset(&s_cc, 0, sizeof(s_cc));
    s_cc.ops = saved_ops;
    (void)ops_set;
    s_cc.display = display;
    s_cc.touch = touch;
    s_cc.policy_enabled = true;
    s_cc.width = lv_display_get_horizontal_resolution(disp);
    s_cc.height = lv_display_get_vertical_resolution(disp);
    if (s_cc.width <= 0 || s_cc.height <= 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_cc.band = _cc_create_band(system_layer);
    if (s_cc.band == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_cc.root = lv_obj_create(system_layer);
    if (s_cc.root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_cc.root);
    lv_obj_set_size(s_cc.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_ignore_layout(s_cc.root, true);
    lv_obj_set_floating(s_cc.root, true);
    lv_obj_set_hidden(s_cc.root, true);
    lv_obj_set_style_bg_color(s_cc.root, lv_color_hex(APP_THEME_COLOR_SCRIM), 0);
    lv_obj_set_style_bg_opa(s_cc.root, CC_SCRIM_OPA, 0);
    lv_obj_set_scrollable(s_cc.root, false);
    lv_obj_add_event_cb(s_cc.root, _cc_root_event, LV_EVENT_CLICKED, NULL);

    /* Contents are built lazily on first show: app_manager_get_image() only
     * resolves after app_manager_init() reaches APP_ADAPTER_READY. */
    s_cc.poll_timer = lv_timer_create(_cc_poll_cb, CC_POLL_MS, NULL);
    s_cc.initialized = true;
    _cc_refresh_band();
    return ESP_OK;
}

esp_err_t app_manager_control_center_deinit(void)
{
    if (!s_cc.initialized)
    {
        return ESP_OK;
    }
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_delete(s_cc.poll_timer);
        s_cc.poll_timer = NULL;
    }
    if (s_cc.root != NULL && lv_obj_is_valid(s_cc.root))
    {
        lv_obj_delete(s_cc.root);
    }
    if (s_cc.band != NULL && lv_obj_is_valid(s_cc.band))
    {
        lv_obj_delete(s_cc.band);
    }
    app_manager_control_center_ops_t ops = s_cc.ops;
    memset(&s_cc, 0, sizeof(s_cc));
    s_cc.ops = ops;
    return ESP_OK;
}

esp_err_t app_manager_control_center_show(void)
{
    if (!s_cc.initialized || s_cc.root == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cc.visible)
    {
        _cc_close();
        return ESP_OK;
    }
    if (!s_cc.built)
    {
        _cc_build();
        s_cc.built = true;
    }
    const uint8_t brightness = app_manager_screen_get_brightness();
    if (s_cc.brightness_slider != NULL && brightness != UINT8_MAX)
    {
        lv_slider_set_value(s_cc.brightness_slider,
                            brightness < 16 ? 16 : (int32_t)brightness,
                            LV_ANIM_OFF);
    }
    _cc_refresh_values();
    lv_obj_set_hidden(s_cc.root, false);
    s_cc.visible = true;
    _cc_refresh_band();
    return ESP_OK;
}

esp_err_t app_manager_control_center_hide(void)
{
    if (!s_cc.initialized)
    {
        return ESP_OK;
    }
    _cc_close();
    return ESP_OK;
}

void app_manager_control_center_navigation_begin(void)
{
    if (!s_cc.initialized)
    {
        return;
    }
    s_cc.band_armed = false;
    _cc_close();
}

bool app_manager_control_center_is_visible(void)
{
    return s_cc.initialized && s_cc.visible;
}

void app_manager_control_center_screen_suspend(void)
{
    if (!s_cc.initialized || !app_manager_mailbox_is_worker())
    {
        return;
    }
    _cc_close();
}

void app_manager_control_center_screen_resume(void)
{
    _cc_refresh_band();
}

esp_err_t app_manager_control_center_set_enabled(bool enabled)
{
    if (!s_cc.initialized)
    {
        s_cc.policy_enabled = enabled;
        return ESP_OK;
    }
    s_cc.policy_enabled = enabled;
    if (!enabled)
    {
        _cc_close();
    }
    _cc_refresh_band();
    return ESP_OK;
}

bool app_manager_control_center_is_enabled(void)
{
    return s_cc.initialized && s_cc.policy_enabled;
}