#define DBG_TAG "app_control_center"

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
#include "app_manager_back_gesture.h"
#include "app_manager_control_center.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_task_switcher.h"
#include "app_theme_colors.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Shared glyph semantic IDs. App Manager consumes the product image IDs
 * numerically (like app icon_id) because it cannot include the apps headers;
 * layers/apps/common/include/app_image_ids.h stays the single source of truth,
 * so keep these values in sync with APP_IMAGE_UI_* there. */
#define CC_IMAGE_WIFI        0x5788U
#define CC_IMAGE_BLUETOOTH   0x5787U
#define CC_IMAGE_BRIGHTNESS  0x578AU
#define CC_IMAGE_VOLUME      0x5785U
#define CC_IMAGE_POWER       0x578BU
#define CC_IMAGE_STANDBY     0x578CU

/* The band is invisible: its height *is* the grab affordance, and a press must
 * start inside it. 20 px was about half a fingertip, so pulls beginning on the
 * bezel missed the band and scrolled the page instead. Keep both top corners
 * clear of the band so page back/menu controls and top-right page actions
 * receive their full hit area. */
#define CC_BAND_HEIGHT            40
#define CC_BAND_EDGE_FALLBACK_PX  30
/* A 52 px page control plus margin on either side. Keep in sync with the page
 * header/action controls in layers/apps/common/src/app_ui.c. */
#define CC_BAND_CONTROL_RESERVE_PX 76
#define CC_BAND_TRIGGER_PX   48
#define CC_CARD_MARGIN       8
/* The transparent strip below the sheet is both the direct drag-to-dismiss
 * handle and the visible page context around the modal surface. */
#define CC_CARD_BOTTOM_RESERVE 48
/* One vertical page holds every section. The header and action buttons scroll
 * with the controls and App cards instead of forming a fixed inner viewport. */
#define CC_PANEL_PAD         16
#define CC_HEADER_HEIGHT     56
#define CC_CARD_GAP          8
#define CC_DIVIDER_H         1
#define CC_ROW_GAP           12
#define CC_LEVEL_ROW_HEIGHT  68
#define CC_LEVEL_TRACK_H     44
#define CC_TILE_HEIGHT       84
#define CC_ACTION_HEIGHT     52
#define CC_WELL_SIZE         36
/* Widest reading is "100%": measured ink is 77 px at the HEAD role, so 80 px
 * keeps the capsule length stable without clipping. Both level rows reserve
 * the same width, so their tracks stay aligned. */
#define CC_VALUE_LABEL_W     80
#define CC_POLL_MS           1000U
#define CC_CARD_MOUNT_RETRY_LIMIT 3U
/* The panel never goes fully dark: 16 is the safe brightness floor the old
 * raw 16..255 slider encoded. The UI now exposes 0..100 % like the speaker. */
#define CC_BRIGHTNESS_FLOOR  16
/* Applying the speaker level is a synchronous codec control transfer (one I2C
 * register write, ~0.3 ms at the 100 kHz bus the codec opens with) taken under
 * the audio service lock, so a drag that crosses 100 steps would otherwise hold
 * that lock 100 times inside render callbacks. 60 ms keeps the audible change
 * continuous while capping the rate; the label still updates on every step.
 * Nothing in this path persists: storage is written once per drag through
 * set_volume_persisted, matching how brightness keeps its NVS write on release. */
#define CC_VOLUME_APPLY_MS   60U
#define CC_SCRIM_OPA         225
#define CC_CARD_SLOT_MAX     APP_MANAGER_CONTROL_CENTER_CARD_MAX
#define CC_DISMISS_DRAG_PX   48
#define CC_BOTTOM_SWIPE_PX   48

/* The surface transition follows the pull direction: the card slides down from
 * above while the scrim fades in. Keep the duration in the same short motion
 * class as the task switcher while leaving each surface's budget independent. */
#define CC_ANIM_MS           180
#define CC_ANIM_PROGRESS_MAX 100

/* Where the surface sits in its transition. Input is accepted only while
 * settled, and HIDDEN is the only state that arms the pull band. */
typedef enum
{
    CC_STATE_HIDDEN = 0,
    CC_STATE_ENTERING,
    CC_STATE_VISIBLE,
    CC_STATE_LEAVING
} cc_state_t;

/* A tile glyph opens its Settings page on a long press. LVGL still reports
 * CLICKED on the release that ends a long press, so long_press_fired makes the
 * toggle handler ignore that release; PRESSED clears it for the next touch. */
typedef struct cc_tile_gesture
{
    const char *settings_page;
    bool long_press_fired;
} cc_tile_gesture_t;

typedef struct cc_card_slot
{
    const app_manager_control_center_card_ops_t *ops;
    const char *app_id;
    lv_obj_t *host;
    void *state;
    bool mounted;
    uint8_t mount_failures;
    uint32_t retry_at;
} cc_card_slot_t;

typedef struct cc_card_candidate
{
    const app_manager_control_center_card_ops_t *ops;
    const char *app_id;
    uint16_t order;
} cc_card_candidate_t;

typedef struct app_manager_control_center
{
    bool initialized;
    cc_state_t state;
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
    lv_obj_t *panel;
    lv_obj_t *page;
    lv_obj_t *dismiss_strip;
    lv_obj_t *card_section;
    lv_obj_t *cards;
    lv_obj_t *actions;
    /* Full-screen transition barrier. It lives above the panel so child
     * buttons cannot receive a press while the surface is moving. */
    lv_obj_t *input_shield;
    int32_t page_scroll_start_y;
    bool page_started_at_bottom;
    bool page_scroll_tracking;
    int32_t strip_press_y;
    int32_t strip_base_y;
    bool strip_dragging;
    cc_card_slot_t card_slots[CC_CARD_SLOT_MAX];
    size_t active_card_count;
    size_t mounted_card_count;
    lv_obj_t *clock_label;
    lv_obj_t *date_label;
    lv_obj_t *brightness_slider;
    lv_obj_t *brightness_label;
    bool brightness_dragging;
    lv_obj_t *volume_slider;
    lv_obj_t *volume_label;
    bool volume_dragging;
    uint8_t volume_applied;
    /* Tick of the last accepted set_volume during a drag; CC_VOLUME_APPLY_MS
     * gates the next one. */
    uint32_t volume_apply_at;
    /* Last level handed to the service during this drag and whether it was
     * refused, so a release does not resend a level that just failed. */
    uint8_t volume_attempted;
    bool volume_attempt_failed;
    /* Level already written to storage, so a release that lands on the same
     * value does not write the codec and the key a second time. */
    uint8_t volume_persisted;
    bool volume_persisted_valid;
    int32_t card_height;
    bool dismiss_latched;
    lv_obj_t *wifi_button;
    lv_obj_t *wifi_icon;
    lv_obj_t *wifi_label;
    cc_tile_gesture_t wifi_gesture;
    lv_obj_t *bt_button;
    lv_obj_t *bt_icon;
    lv_obj_t *bt_label;
    cc_tile_gesture_t bt_gesture;
    lv_timer_t *poll_timer;
} app_manager_control_center_t;

static app_manager_control_center_t s_cc;
/* Cross-task snapshots. s_cc itself is UI-worker-only; these mirror the
 * fields that public getters read from arbitrary tasks. */
static atomic_bool s_policy_published;
static atomic_bool s_visible_published;
static atomic_bool s_cc_published;
static atomic_bool s_preinit_policy_configured;
static atomic_bool s_preinit_policy_enabled;

#ifdef HOST_TEST
    /* Test observability belongs to the integration LVGL fake. Firmware and the
    * simulator resolve the same controller-owned host metadata below. */
    extern void host_lv_set_object_identity(lv_obj_t *object, const char *identity);
#endif

static void _cc_refresh_band(void);
static void _cc_refresh_values(void);
static void _cc_sync_cards(void);
static void _cc_clear_cards(void);
static void _cc_touch_event(lv_event_t *event);
static void _cc_dismiss_strip_event(lv_event_t *event);
static void _cc_open_completed(lv_anim_t *animation);
static void _cc_reset_gesture(void);

static void _cc_set_state(cc_state_t state)
{
    s_cc.state = state;
    atomic_store_explicit(&s_visible_published,
                          state == CC_STATE_ENTERING ||
                          state == CC_STATE_VISIBLE,
                          memory_order_release);
    /* A sibling above the panel is required: LVGL still searches clickable
     * card descendants when their container is press-locked. The shield owns
     * no action and only exists while the surface is moving. */
    if (s_cc.input_shield != NULL)
    {
        const bool blocking = state == CC_STATE_ENTERING ||
                              state == CC_STATE_LEAVING;
        lv_obj_set_hidden(s_cc.input_shield, !blocking);
        lv_obj_set_clickable(s_cc.input_shield, blocking);
        lv_obj_set_press_lock(s_cc.input_shield, blocking);
    }
}

/* Public visibility: the surface counts as visible while it slides in and while
 * it is settled. A leaving surface reads as hidden so callers and the pull band
 * see the close before the slide-out finishes. */
static bool _cc_surface_visible(void)
{
    return s_cc.state == CC_STATE_ENTERING || s_cc.state == CC_STATE_VISIBLE;
}

static lv_obj_t *_cc_icon(lv_obj_t *parent, uint32_t image_id, int32_t size,
                          const char *fallback)
{
    const lv_image_dsc_t *dsc = NULL;
    if (app_manager_get_image(image_id, &dsc) == ESP_OK && dsc != NULL)
    {
        lv_obj_t *image = lv_image_create(parent);
        if (image != NULL)
        {
            lv_obj_set_size(image, size, size);
            /* An lv_image is clickable by default: left as the press target it
             * would swallow the tile/row gesture and long-press would never
             * reach the owner. */
            lv_obj_set_clickable(image, false);
            lv_image_set_src(image, dsc);
            return image;
        }
    }
    lv_obj_t *symbol = lv_label_create(parent);
    if (symbol != NULL)
    {
        lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
        lv_label_set_text(symbol,
                          fallback != NULL ? fallback : LV_SYMBOL_IMAGE);
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

/* Radio tiles repeat their state in the glyph and its circular well tint, so
 * the state line is never the only cue. */
static void _cc_radio_paint(lv_obj_t *icon, app_manager_radio_state_t state)
{
    _cc_icon_color(icon, _cc_radio_color(state));
    lv_obj_t *well = icon != NULL ? lv_obj_get_parent(icon) : NULL;
    if (well != NULL)
    {
        lv_obj_set_style_bg_color(well, lv_color_hex(
                                      state == APP_MANAGER_RADIO_CONNECTED ?
                                      APP_THEME_COLOR_MINT_TINT :
                                      state == APP_MANAGER_RADIO_ON_IDLE ?
                                      APP_THEME_COLOR_AMBER_TINT :
                                      APP_THEME_COLOR_TRACK), 0);
    }
}

static int32_t _cc_panel_offscreen_y(void)
{
    return -(s_cc.card_height + CC_CARD_MARGIN);
}

/* One progress value drives both halves of the transition: the card travels
 * between the off-screen start and its resting margin while the scrim crosses
 * the same 0..100 range, so the two never drift apart. */
static void _cc_anim_apply(int32_t progress)
{
    if (progress < 0)
    {
        progress = 0;
    }
    else if (progress > CC_ANIM_PROGRESS_MAX)
    {
        progress = CC_ANIM_PROGRESS_MAX;
    }
    if (s_cc.panel != NULL)
    {
        const int32_t hidden_y = _cc_panel_offscreen_y();
        lv_obj_set_y(s_cc.panel,
                     hidden_y + (CC_CARD_MARGIN - hidden_y) * progress /
                     CC_ANIM_PROGRESS_MAX);
    }
    if (s_cc.root != NULL)
    {
        lv_obj_set_style_bg_opa(s_cc.root,
                                (lv_opa_t)(CC_SCRIM_OPA * progress /
                                           CC_ANIM_PROGRESS_MAX), 0);
    }
}

static void _cc_anim_exec(void *variable, int32_t value)
{
    (void)variable;
    _cc_anim_apply(value);
}

static void _cc_anim_stop(void)
{
    if (s_cc.root != NULL)
    {
        lv_anim_delete(s_cc.root, NULL);
    }
}

static void _cc_anim_start(int32_t from, int32_t to,
                           lv_anim_path_cb_t path,
                           lv_anim_completed_cb_t completed)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_cc.root);
    lv_anim_set_exec_cb(&animation, _cc_anim_exec);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, CC_ANIM_MS);
    lv_anim_set_path_cb(&animation, path);
    lv_anim_set_early_apply(&animation, true);
    if (completed != NULL)
    {
        lv_anim_set_completed_cb(&animation, completed);
    }
    (void)lv_anim_start(&animation);
}

/* Park a hidden surface at its resting geometry. The next open resets progress
 * to zero, so this only keeps the object state coherent for teardown and for
 * tests that inspect the tree while the surface is closed. */
static void _cc_anim_park(void)
{
    _cc_anim_stop();
    _cc_anim_apply(CC_ANIM_PROGRESS_MAX);
}

/* Non-gesture teardown: display sleep, standby, navigation, policy disable and
 * deinit must not spend 180 ms on screen. */
static void _cc_close_now(void)
{
    _cc_anim_park();
    _cc_reset_gesture();
    s_cc.brightness_dragging = false;
    s_cc.volume_dragging = false;
    _cc_clear_cards();
    if (s_cc.panel != NULL)
    {
        lv_obj_set_hidden(s_cc.panel, true);
    }
    if (s_cc.root != NULL)
    {
        lv_obj_set_hidden(s_cc.root, true);
    }
    _cc_set_state(CC_STATE_HIDDEN);
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_pause(s_cc.poll_timer);
    }
    _cc_refresh_band();
}

static void _cc_reset_gesture(void)
{
    s_cc.page_scroll_start_y = 0;
    s_cc.page_started_at_bottom = false;
    s_cc.page_scroll_tracking = false;
    s_cc.strip_press_y = 0;
    s_cc.strip_base_y = CC_CARD_MARGIN;
    s_cc.strip_dragging = false;
    s_cc.dismiss_latched = false;
}

static void _cc_close_completed(lv_anim_t *animation)
{
    (void)animation;
    if (s_cc.state != CC_STATE_LEAVING)
    {
        return;
    }
    _cc_reset_gesture();
    _cc_clear_cards();
    if (s_cc.panel != NULL)
    {
        lv_obj_set_hidden(s_cc.panel, true);
    }
    if (s_cc.root != NULL)
    {
        lv_obj_set_hidden(s_cc.root, true);
    }
    _cc_anim_apply(CC_ANIM_PROGRESS_MAX);
    _cc_set_state(CC_STATE_HIDDEN);
    s_cc.brightness_dragging = false;
    s_cc.volume_dragging = false;
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_pause(s_cc.poll_timer);
    }
    _cc_refresh_band();
}

/* User-gesture close. The surface stops reading as visible before the first
 * frame so the pull band and is_visible() see the close immediately, then the
 * card slides back out. */
static void _cc_close_animated(void)
{
    if (s_cc.root == NULL)
    {
        return;
    }
    if (s_cc.state == CC_STATE_ENTERING)
    {
        _cc_close_now();
        return;
    }
    if (s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    if (s_cc.panel == NULL)
    {
        _cc_close_now();
        return;
    }
    _cc_anim_stop();
    _cc_set_state(CC_STATE_LEAVING);
    _cc_refresh_band();
    _cc_anim_start(CC_ANIM_PROGRESS_MAX, 0, lv_anim_path_ease_in,
                   _cc_close_completed);
}

static int32_t _cc_progress_from_y(int32_t y)
{
    const int32_t hidden_y = _cc_panel_offscreen_y();
    const int32_t span = CC_CARD_MARGIN - hidden_y;
    if (span <= 0)
    {
        return CC_ANIM_PROGRESS_MAX;
    }
    int32_t progress = (y - hidden_y) * CC_ANIM_PROGRESS_MAX / span;
    if (progress < 0)
    {
        progress = 0;
    }
    else if (progress > CC_ANIM_PROGRESS_MAX)
    {
        progress = CC_ANIM_PROGRESS_MAX;
    }
    return progress;
}

static bool _cc_panel_contains(lv_obj_t *object)
{
    while (object != NULL)
    {
        if (object == s_cc.panel)
        {
            return true;
        }
        object = lv_obj_get_parent(object);
    }
    return false;
}

/* Input-device callbacks are needed because provider cards can contain
 * arbitrary descendants and event bubbling is not enabled across third-party
 * trees. At the hard bottom LVGL has no scroll object to report, so PRESSED
 * and RELEASED are the only reliable signal for one more upward swipe. */
static void _cc_touch_event(lv_event_t *event)
{
    lv_indev_t *indev = s_cc.touch;
    if (indev == NULL)
    {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if ((code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED) &&
            s_cc.dismiss_latched)
    {
        /* The dismissal was recognized on RELEASED. Consume the synthetic
         * click produced by the same gesture so no card action can fire. */
        lv_indev_stop_processing(indev);
        return;
    }
    if (code == LV_EVENT_INDEV_RESET)
    {
        _cc_reset_gesture();
        return;
    }
    if (s_cc.state != CC_STATE_VISIBLE || s_cc.panel == NULL)
    {
        return;
    }
    if (code == LV_EVENT_PRESSED)
    {
        lv_obj_t *active = lv_indev_get_active_obj();
        s_cc.page_scroll_tracking = false;
        s_cc.page_started_at_bottom = false;
        s_cc.dismiss_latched = false;
        if (!_cc_panel_contains(active) ||
                lv_obj_get_scroll_bottom(s_cc.panel) > 0)
        {
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        s_cc.page_scroll_start_y = point.y;
        s_cc.page_started_at_bottom = true;
        s_cc.page_scroll_tracking = true;
        return;
    }
    if (code != LV_EVENT_RELEASED || !s_cc.page_scroll_tracking ||
            !s_cc.page_started_at_bottom || s_cc.dismiss_latched)
    {
        return;
    }

    s_cc.page_scroll_tracking = false;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int32_t upward = s_cc.page_scroll_start_y - point.y;
    /* A page that starts at its end has no normal scroll left. A second
     * deliberate upward gesture is the dismissal signal; ordinary scrolling
     * and a gesture that merely reaches the end started above it. */
    if (lv_indev_get_scroll_obj(indev) != NULL ||
            upward < CC_BOTTOM_SWIPE_PX)
    {
        return;
    }
    s_cc.dismiss_latched = true;
    /* Let the widget receive RELEASED so LVGL clears LV_STATE_PRESSED. The
     * following CLICKED event is consumed above while dismiss_latched is set. */
    _cc_close_animated();
}

/* The bottom strip is the dedicated transparent grab surface. Dragging
 * dismisses the whole panel directly; it never participates in page scroll. */
static void _cc_dismiss_strip_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (s_cc.state != CC_STATE_VISIBLE ||
            (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
             code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST))
    {
        return;
    }
    lv_point_t point;
    const lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL)
    {
        indev = s_cc.touch;
    }
    if (indev == NULL)
    {
        return;
    }
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED)
    {
        s_cc.strip_press_y = point.y;
        s_cc.strip_base_y = lv_obj_get_y(s_cc.panel);
        s_cc.strip_dragging = true;
        return;
    }
    if (!s_cc.strip_dragging)
    {
        return;
    }
    if (code == LV_EVENT_PRESSING)
    {
        const int32_t moved = s_cc.strip_press_y - point.y;
        const int32_t hidden_y = _cc_panel_offscreen_y();
        int32_t y = s_cc.strip_base_y - moved;
        if (y > CC_CARD_MARGIN)
        {
            y = CC_CARD_MARGIN;
        }
        else if (y < hidden_y)
        {
            y = hidden_y;
        }
        _cc_anim_apply(_cc_progress_from_y(y));
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST)
    {
        return;
    }

    s_cc.strip_dragging = false;
    const int32_t progress = _cc_progress_from_y(lv_obj_get_y(s_cc.panel));
    const bool dismiss = code == LV_EVENT_RELEASED &&
                         s_cc.strip_press_y - point.y >= CC_DISMISS_DRAG_PX;
    if (dismiss)
    {
        _cc_set_state(CC_STATE_LEAVING);
        _cc_refresh_band();
        _cc_anim_start(progress, 0, lv_anim_path_ease_in,
                       _cc_close_completed);
    }
    else
    {
        _cc_set_state(CC_STATE_ENTERING);
        _cc_anim_start(progress, CC_ANIM_PROGRESS_MAX, lv_anim_path_ease_out,
                       _cc_open_completed);
    }
}

static void _cc_root_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
            lv_event_get_target(event) != lv_event_get_current_target(event) ||
            s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    _cc_close_animated();
}

static void _cc_screen_off_event(lv_event_t *event)
{
    (void)event;
    if (s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    _cc_close_now();
    if (app_manager_pm_request_screen_off() != ESP_OK)
    {
        LOG_W("screen-off request rejected");
    }
}

static void _cc_standby_event(lv_event_t *event)
{
    (void)event;
    if (s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    _cc_close_now();
    if (app_manager_pm_request_standby() != ESP_OK)
    {
        LOG_W("standby request rejected");
    }
}

/* The usable panel range is CC_BRIGHTNESS_FLOOR..255, so 0 % is the safe
 * floor and 100 % is full output; mapping over the usable span keeps a
 * release-and-reopen round trip stable instead of showing 6 % at the floor. */
static uint8_t _cc_brightness_percent(uint8_t raw)
{
    if (raw <= CC_BRIGHTNESS_FLOOR)
    {
        return 0U;
    }
    const uint32_t span = 255U - CC_BRIGHTNESS_FLOOR;
    return (uint8_t)(((uint32_t)(raw - CC_BRIGHTNESS_FLOOR) * 100U +
                      span / 2U) / span);
}

static uint8_t _cc_brightness_raw(int32_t percent)
{
    if (percent < 0)
    {
        percent = 0;
    }
    else if (percent > 100)
    {
        percent = 100;
    }
    const int32_t span = 255 - CC_BRIGHTNESS_FLOOR;
    return (uint8_t)(CC_BRIGHTNESS_FLOOR + (percent * span + 50) / 100);
}

static void _cc_brightness_text(int32_t percent)
{
    if (s_cc.brightness_label != NULL)
    {
        lv_label_set_text_fmt(s_cc.brightness_label, "%d%%", (int)percent);
    }
}

/* Brightness previews through the temporary setter while the finger moves and
 * commits once on release, mirroring the speaker row. Both rows now share the
 * same 0..100 % scale and the same value-label position; the raw hardware
 * floor is applied only in _cc_brightness_raw(). */
static void _cc_brightness_event(lv_event_t *event)
{
    if (s_cc.brightness_slider == NULL || s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (!s_cc.brightness_dragging)
        {
            return;
        }
        s_cc.brightness_dragging = false;
        const int32_t percent =
            lv_slider_get_value(s_cc.brightness_slider);
        _cc_brightness_text(percent);
        if (app_manager_screen_set_brightness(
                    _cc_brightness_raw(percent)) != ESP_OK)
        {
            LOG_W("brightness commit rejected");
        }
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    s_cc.brightness_dragging = true;
    const int32_t percent = lv_slider_get_value(s_cc.brightness_slider);
    _cc_brightness_text(percent);
    if (app_manager_screen_set_brightness_temp(
                _cc_brightness_raw(percent)) != ESP_OK)
    {
        LOG_W("brightness preview rejected");
    }
}

static void _cc_volume_text(uint8_t percent)
{
    if (s_cc.volume_label != NULL)
    {
        lv_obj_set_style_text_color(s_cc.volume_label,
                                    lv_color_hex(APP_THEME_COLOR_INK), 0);
        lv_label_set_text_fmt(s_cc.volume_label, "%u%%", (unsigned)percent);
    }
}

/* Send the level to the service. The label already shows it, so a rejected
 * request keeps the knob where the user put it and leaves volume_applied stale;
 * the next poll then re-syncs the row from the service instead of the label
 * claiming a level that never applied. */
static esp_err_t _cc_volume_apply(uint8_t percent)
{
    if (s_cc.ops.set_volume == NULL || percent == s_cc.volume_applied)
    {
        return ESP_OK;
    }
    const esp_err_t result = s_cc.ops.set_volume(percent);
    s_cc.volume_attempted = percent;
    s_cc.volume_attempt_failed = result != ESP_OK;
    if (result == ESP_OK)
    {
        s_cc.volume_applied = percent;
    }
    /* Stamp every attempt, refused or not, so a failing service is not retried
     * on every knob step. volume_applied stays stale on failure, which is what
     * lets the poll recover the row. */
    s_cc.volume_apply_at = lv_tick_get();
    return result;
}

/* The speaker level is applied as the knob moves: unlike brightness there is
 * no preview API, and the audible change is the only feedback beyond the value
 * label. The value label therefore tracks every step, while the codec write is
 * rate-limited to CC_VOLUME_APPLY_MS and always forced on release. */
static void _cc_volume_event(lv_event_t *event)
{
    if (s_cc.volume_slider == NULL || s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        /* The knob may already be gone when the surface closes mid-drag. */
        if (s_cc.volume_dragging)
        {
            s_cc.volume_dragging = false;
            /* Throttling may have skipped the final step, so the level the
             * finger stopped on is forced through here — unless the service
             * just refused that same level, in which case the poll owns the
             * recovery and a resend would mask the failure. */
            const uint8_t settled =
                (uint8_t)(int32_t)lv_slider_get_value(s_cc.volume_slider);
            const bool refused_settled = s_cc.volume_attempt_failed &&
                                         settled == s_cc.volume_attempted;
            if (refused_settled)
            {
                return;
            }
            /* Persistence happens once per drag, never while the knob moves:
             * the throttled steps above must stay off the storage. The
             * persisted setter also applies the level, so the resend below is
             * only needed when that callback is absent — otherwise the codec
             * would be written twice for one release. A drag that ends on the
             * level already stored has nothing to send. */
            if (s_cc.volume_persisted_valid &&
                    settled == s_cc.volume_persisted)
            {
                return;
            }
            if (s_cc.ops.set_volume_persisted != NULL)
            {
                if (s_cc.ops.set_volume_persisted(settled) == ESP_OK)
                {
                    s_cc.volume_applied = settled;
                    s_cc.volume_persisted = settled;
                    s_cc.volume_persisted_valid = true;
                }
            }
            else if (settled != s_cc.volume_applied)
            {
                (void)_cc_volume_apply(settled);
            }
        }
        return;
    }
    if (code != LV_EVENT_VALUE_CHANGED ||
            lv_obj_has_state(s_cc.volume_slider, LV_STATE_DISABLED))
    {
        return;
    }
    s_cc.volume_dragging = true;
    const uint8_t percent =
        (uint8_t)(int32_t)lv_slider_get_value(s_cc.volume_slider);
    _cc_volume_text(percent);
    if (percent == s_cc.volume_applied)
    {
        return;
    }
    if ((uint32_t)(lv_tick_get() - s_cc.volume_apply_at) <
            CC_VOLUME_APPLY_MS)
    {
        return;
    }
    /* A new step is a new intent, so an earlier refusal no longer applies. */
    s_cc.volume_attempt_failed = false;
    _cc_volume_apply(percent);
}

/* Long-press destination for one control. The request uses the ordinary
 * navigation gateway, whose execution hides the control center before the
 * Settings page is prepared, so the surface is never left under it. */
static void _cc_open_settings(const char *page_id)
{
    if (page_id == NULL)
    {
        return;
    }
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_OPEN_PAGE,
        .app_id = APP_MANAGER_ID_SETTINGS,
        .page_id = page_id,
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_DEFAULT,
        },
    };
    const esp_err_t result = app_manager_navigate_async(&request, NULL, NULL);
    if (result != ESP_OK)
    {
        LOG_W("settings page admission failed: %s",
              esp_err_to_name(result));
    }
}

static void _cc_tile_pressed(lv_event_t *event)
{
    cc_tile_gesture_t *gesture = lv_event_get_user_data(event);
    if (gesture != NULL)
    {
        gesture->long_press_fired = false;
    }
}

static void _cc_tile_long_press(lv_event_t *event)
{
    cc_tile_gesture_t *gesture = lv_event_get_user_data(event);
    if (s_cc.state != CC_STATE_VISIBLE || gesture == NULL)
    {
        return;
    }
    gesture->long_press_fired = true;
    _cc_open_settings(gesture->settings_page);
}

static void _cc_row_long_press(lv_event_t *event)
{
    if (s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    _cc_open_settings(lv_event_get_user_data(event));
}

static void _cc_wifi_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
            s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    cc_tile_gesture_t *gesture = lv_event_get_user_data(event);
    if (gesture != NULL && gesture->long_press_fired)
    {
        gesture->long_press_fired = false;
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
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
            s_cc.state != CC_STATE_VISIBLE)
    {
        return;
    }
    cc_tile_gesture_t *gesture = lv_event_get_user_data(event);
    if (gesture != NULL && gesture->long_press_fired)
    {
        gesture->long_press_fired = false;
        return;
    }
    if (s_cc.ops.get_bluetooth == NULL || s_cc.ops.set_bluetooth == NULL)
    {
        return;
    }
    if (s_cc.ops.get_bluetooth_transitioning != NULL &&
            s_cc.ops.get_bluetooth_transitioning())
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

/* The brightness row is normally owned by the drag. Reading it back on the
 * poll keeps it honest after another owner changes the panel level, and gives
 * the open path a single conversion from raw hardware units to percent. */
static void _cc_refresh_brightness(void)
{
    if (s_cc.brightness_slider == NULL || s_cc.brightness_dragging)
    {
        return;
    }
    const uint8_t raw = app_manager_screen_get_brightness();
    const int32_t percent = (int32_t)_cc_brightness_percent(raw);
    if (lv_slider_get_value(s_cc.brightness_slider) != percent)
    {
        lv_slider_set_value(s_cc.brightness_slider, percent, LV_ANIM_OFF);
    }
    _cc_brightness_text(percent);
}

/* The speaker row mirrors the service: a rejected or externally changed level
 * (another owner, an error path) is picked up on the next poll. A drag in
 * progress owns the knob, so the poll must not fight it. */
static void _cc_refresh_volume(void)
{
    if (s_cc.volume_slider == NULL || s_cc.ops.get_volume == NULL)
    {
        return;
    }
    if (s_cc.volume_dragging)
    {
        return;
    }
    uint8_t percent = 0U;
    if (s_cc.ops.get_volume(&percent) != ESP_OK)
    {
        /* No audio path (board without a codec, service still starting):
         * grey the control rather than let a drag pretend to work. */
        lv_obj_add_state(s_cc.volume_slider, LV_STATE_DISABLED);
        if (s_cc.volume_label != NULL)
        {
            lv_label_set_text(s_cc.volume_label, "--");
            lv_obj_set_style_text_color(s_cc.volume_label,
                                        lv_color_hex(APP_THEME_COLOR_INK_FAINT), 0);
        }
        return;
    }
    lv_obj_remove_state(s_cc.volume_slider, LV_STATE_DISABLED);
    if (s_cc.volume_label != NULL)
    {
        lv_obj_set_style_text_color(s_cc.volume_label,
                                    lv_color_hex(APP_THEME_COLOR_INK), 0);
    }
    if (percent > 100U)
    {
        percent = 100U;
    }
    s_cc.volume_applied = percent;
    /* A refused request leaves the knob where the finger put it, so the row
     * would otherwise show a level the codec never took. Compare against the
     * knob, not against the cached value, or a rejection between two drags
     * stays invisible. */
    if (lv_slider_get_value(s_cc.volume_slider) != (int32_t)percent)
    {
        lv_slider_set_value(s_cc.volume_slider, (int32_t)percent,
                            LV_ANIM_OFF);
    }
    _cc_volume_text(percent);
}

static void _cc_refresh_values(void)
{
    _cc_refresh_brightness();

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
        _cc_radio_paint(s_cc.wifi_icon, available ? state :
                        APP_MANAGER_RADIO_OFF);
        lv_label_set_text(s_cc.wifi_label,
                          !available ? "未连接" :
                          state == APP_MANAGER_RADIO_CONNECTED ?
                          (ssid != NULL ? ssid : "已连接") :
                          state == APP_MANAGER_RADIO_ON_IDLE ?
                          "未连接" : "已关闭");
    }

    if (s_cc.bt_button != NULL)
    {
        const bool transitioning =
            s_cc.ops.get_bluetooth_transitioning != NULL &&
            s_cc.ops.get_bluetooth_transitioning();

        if (transitioning)
        {
            lv_obj_add_state(s_cc.bt_button, LV_STATE_DISABLED);
            _cc_radio_paint(s_cc.bt_icon, APP_MANAGER_RADIO_ON_IDLE);
            lv_label_set_text(s_cc.bt_label, "切换中");
        }
        else
        {
            app_manager_radio_state_t state = APP_MANAGER_RADIO_OFF;

            lv_obj_remove_state(s_cc.bt_button, LV_STATE_DISABLED);
            if (s_cc.ops.get_bluetooth != NULL &&
                    s_cc.ops.get_bluetooth(&state) == ESP_OK)
            {
                _cc_radio_paint(s_cc.bt_icon, state);
                lv_label_set_text(s_cc.bt_label,
                                  state == APP_MANAGER_RADIO_OFF ? "已关闭" :
                                  state == APP_MANAGER_RADIO_CONNECTED ?
                                  "已连接" : "已开启");
            }
        }
    }
    _cc_refresh_volume();
}

static void _cc_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (_cc_surface_visible())
    {
        _cc_refresh_values();
        _cc_sync_cards();
    }
}

static lv_obj_t *_cc_tile(lv_obj_t *parent, lv_obj_t **tile_out,
                          lv_obj_t **icon_out, lv_obj_t **label_out,
                          uint32_t image_id, lv_event_cb_t callback,
                          cc_tile_gesture_t *gesture, const char *title,
                          const char *icon_fallback)
{
    lv_obj_t *tile = lv_button_create(parent);
    if (tile == NULL)
    {
        return NULL;
    }
    *tile_out = tile;
    lv_obj_set_height(tile, CC_TILE_HEIGHT);
    lv_obj_set_flex_grow(tile, 1);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_THEME_COLOR_PLUME_HI), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_THEME_COLOR_TRACK),
                              LV_STATE_PRESSED);
    /* A transitioning radio stays legible: disabled keeps the normal fill. */
    lv_obj_set_style_bg_color(tile, lv_color_hex(APP_THEME_COLOR_PLUME_HI),
                              LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_style_pad_column(tile, 8, 0);
    /* Icon on the left, name over state on the right; the text column is
     * sized by the row so both lines clear the 36 px icon well. */
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(tile, callback, LV_EVENT_CLICKED, gesture);
    /* Long press opens the matching Settings page. The press is click-only so
     * sliding off the tile cancels it, and the release that ends a long press
     * must not also toggle the radio. */
    if (gesture != NULL && gesture->settings_page != NULL)
    {
        lv_obj_add_event_cb(tile, _cc_tile_pressed, LV_EVENT_PRESSED, gesture);
        lv_obj_add_event_cb(tile, _cc_tile_long_press, LV_EVENT_LONG_PRESSED,
                            gesture);
    }
    lv_obj_set_press_lock(tile, false);

    lv_obj_t *well = lv_obj_create(tile);
    lv_obj_remove_style_all(well);
    lv_obj_set_size(well, CC_WELL_SIZE, CC_WELL_SIZE);
    lv_obj_set_style_radius(well, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(well, lv_color_hex(APP_THEME_COLOR_TRACK), 0);
    lv_obj_set_clickable(well, false);
    lv_obj_t *icon = _cc_icon(well, image_id, 24, icon_fallback);
    if (icon != NULL)
    {
        lv_obj_center(icon);
        _cc_radio_paint(icon, APP_MANAGER_RADIO_OFF);
    }
    *icon_out = icon;

    lv_obj_t *text = lv_obj_create(tile);
    if (text != NULL)
    {
        lv_obj_remove_style_all(text);
        /* Labels are input-transparent but a plain lv_obj is clickable by
         * default: without this the text column becomes the press target and
         * the tile's CLICKED callback never runs when the name/state is
         * tapped. */
        lv_obj_set_clickable(text, false);
        lv_obj_set_width(text, 0);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, 0, 0);
        const lv_font_t *name_font =
            app_manager_get_font(APP_THEME_FONT_BODY);
        const lv_font_t *state_font =
            app_manager_get_font(APP_THEME_FONT_SMALL);
        lv_obj_t *name = lv_label_create(text);
        lv_obj_set_width(name, LV_PCT(100));
        /* DOTS keeps the row to one line even for a 32-byte SSID; a fixed
         * line box stops the label from growing vertically and pushing the
         * state line out of the tile. */
        if (name_font != NULL)
        {
            lv_obj_set_height(name, lv_font_get_line_height(name_font));
        }
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(name, lv_color_hex(APP_THEME_COLOR_INK), 0);
        lv_obj_set_style_text_font(name, name_font, 0);
        lv_label_set_text(name, title);
        lv_obj_t *state = lv_label_create(text);
        lv_obj_set_width(state, LV_PCT(100));
        if (state_font != NULL)
        {
            lv_obj_set_height(state, lv_font_get_line_height(state_font));
        }
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(state,
                                    lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
        lv_obj_set_style_text_font(state, state_font, 0);
        lv_label_set_text(state, "--");
        *label_out = state;
    }
    return tile;
}

/* One level capsule row. Both brightness and speaker use this exact builder:
 * a 68 px pill with the glyph at the leading edge, a full-height capsule
 * track in the middle and a fixed-width HEAD value at the trailing edge. The
 * shared geometry is what removes the old "one row shows a percentage, the
 * other is silent" mismatch. The row background, not the slider, owns the
 * long-press target so a drag never turns into a settings jump. */
static lv_obj_t *_cc_level_row(lv_obj_t *parent, uint32_t image_id,
                               const char *icon_fallback, uint32_t accent,
                               const char *settings_page, int32_t minimum,
                               int32_t maximum, int32_t value,
                               lv_event_cb_t callback, lv_obj_t **slider_out,
                               lv_obj_t **value_label_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    if (row == NULL)
    {
        return NULL;
    }
    *slider_out = NULL;
    if (value_label_out != NULL)
    {
        *value_label_out = NULL;
    }
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, CC_LEVEL_ROW_HEIGHT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(APP_THEME_COLOR_PLUME_HI), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    if (settings_page != NULL)
    {
        lv_obj_add_event_cb(row, _cc_row_long_press, LV_EVENT_LONG_PRESSED,
                            (void *)settings_page);
        lv_obj_set_press_lock(row, false);
    }

    lv_obj_t *icon = _cc_icon(row, image_id, 26, icon_fallback);
    if (icon != NULL)
    {
        _cc_icon_color(icon, accent);
    }

    lv_obj_t *slider = lv_slider_create(row);
    if (slider != NULL)
    {
        lv_obj_set_width(slider, 0);
        lv_obj_set_height(slider, CC_LEVEL_TRACK_H);
        lv_obj_set_flex_grow(slider, 1);
        lv_slider_set_range(slider, minimum, maximum);
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_THEME_COLOR_TRACK),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(accent),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(slider, 0, LV_PART_INDICATOR);
        /* The illuminated fill edge is the handle: the knob stays as LVGL's
         * hit geometry but is not painted, so the capsule reads as one
         * continuous phone-style level control instead of a track plus puck. */
        lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_border_width(slider, 0, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
        lv_obj_set_ext_click_area(slider, 20);
        lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(slider, callback, LV_EVENT_RELEASED, NULL);
        /* A finger that leaves the track mid-drag ends the gesture here; the
         * poll may re-sync the knob only after this. */
        lv_obj_add_event_cb(slider, callback, LV_EVENT_PRESS_LOST, NULL);
    }
    *slider_out = slider;

    if (value_label_out != NULL)
    {
        lv_obj_t *value_label = lv_label_create(row);
        lv_obj_set_width(value_label, CC_VALUE_LABEL_W);
        lv_obj_set_height(value_label, LV_SIZE_CONTENT);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(value_label,
                                    lv_color_hex(APP_THEME_COLOR_INK), 0);
        lv_obj_set_style_text_font(value_label,
                                   app_manager_get_font(APP_THEME_FONT_HEAD), 0);
        lv_label_set_text(value_label, "0%");
        *value_label_out = value_label;
    }
    return row;
}

/* The whole surface is one long page. Keeping the viewport at the maximum
 * usable height gives every section the same scroll owner; short pages simply
 * have no scroll travel. */
static int32_t _cc_panel_height(void)
{
    return s_cc.height - CC_CARD_MARGIN - CC_CARD_BOTTOM_RESERVE;
}

static size_t _cc_collect_active_cards(cc_card_candidate_t *active)
{
    cc_card_candidate_t candidates[CC_CARD_SLOT_MAX];
    size_t count = 0U;
    for (const app_manager_app_desc_t *app = app_manager_builtin_list_open();
            app != NULL;
            app = app_manager_builtin_list_get_next(app))
    {
        const app_manager_control_center_card_ops_t *card =
            app->control_center_card;
        if (card == NULL || !card->is_active())
        {
            continue;
        }
        if (count >= CC_CARD_SLOT_MAX)
        {
            /* Registry validation normally makes this unreachable; keep the
             * local array bounded if an unvalidated descriptor set reaches us. */
            LOG_W("control center card slot limit reached");
            break;
        }
        size_t insert = count;
        while (insert > 0U && candidates[insert - 1U].order > app->launcher_order)
        {
            candidates[insert] = candidates[insert - 1U];
            --insert;
        }
        candidates[insert].ops = card;
        candidates[insert].app_id = app->id;
        candidates[insert].order = app->launcher_order;
        ++count;
    }
    for (size_t index = 0U; index < count; ++index)
    {
        active[index] = candidates[index];
    }
    return count;
}

static bool _cc_cards_match(const cc_card_candidate_t *active, size_t count)
{
    if (count != s_cc.active_card_count)
    {
        return false;
    }
    for (size_t index = 0U; index < count; ++index)
    {
        if (s_cc.card_slots[index].ops != active[index].ops ||
                s_cc.card_slots[index].app_id != active[index].app_id)
        {
            return false;
        }
    }
    return true;
}

static void _cc_apply_panel_height(void)
{
    if (s_cc.panel == NULL)
    {
        return;
    }
    s_cc.card_height = _cc_panel_height();
    lv_obj_set_height(s_cc.panel, s_cc.card_height);
}

static void _cc_update_card_section(void)
{
    if (s_cc.card_section != NULL && lv_obj_is_valid(s_cc.card_section))
    {
        lv_obj_set_hidden(s_cc.card_section, s_cc.mounted_card_count == 0U);
    }
    if (s_cc.cards != NULL && lv_obj_is_valid(s_cc.cards))
    {
        lv_obj_set_hidden(s_cc.cards, s_cc.mounted_card_count == 0U);
    }
}

/* Delete the card trees before asking providers to release their state. */
static void _cc_clear_cards(void)
{
    if (s_cc.cards != NULL && lv_obj_is_valid(s_cc.cards))
    {
        lv_obj_clean(s_cc.cards);
    }
    for (size_t index = 0U; index < s_cc.active_card_count; ++index)
    {
        cc_card_slot_t *slot = &s_cc.card_slots[index];
        if (slot->mounted && slot->ops != NULL && slot->ops->unmount != NULL)
        {
            slot->ops->unmount(slot->state);
        }
    }
    memset(s_cc.card_slots, 0, sizeof(s_cc.card_slots));
    s_cc.active_card_count = 0U;
    s_cc.mounted_card_count = 0U;
    _cc_update_card_section();
    _cc_apply_panel_height();
}

static void _cc_refresh_cards(void)
{
    for (size_t index = 0U; index < s_cc.active_card_count; ++index)
    {
        cc_card_slot_t *slot = &s_cc.card_slots[index];
        if (slot->mounted && slot->ops != NULL)
        {
            slot->ops->refresh(slot->state);
        }
    }
}

static void _cc_schedule_card_retry(cc_card_slot_t *slot)
{
    if (slot->mount_failures < UINT8_MAX)
    {
        ++slot->mount_failures;
    }
    slot->retry_at = slot->mount_failures <= CC_CARD_MOUNT_RETRY_LIMIT ?
                     lv_tick_get() + CC_POLL_MS : 0U;
}

/* Mount one slot without disturbing cards that are already healthy. The host
 * is inserted at the slot's launcher-order position so a successful retry does
 * not jump behind later providers. */
static bool _cc_mount_card_slot(cc_card_slot_t *slot, size_t slot_index)
{
    if (slot->ops == NULL || slot->mounted)
    {
        return false;
    }
    lv_obj_t *host = lv_obj_create(s_cc.cards);
    if (host == NULL)
    {
        _cc_schedule_card_retry(slot);
        return false;
    }
    lv_obj_remove_style_all(host);
    lv_obj_set_width(host, LV_PCT(100));
    lv_obj_set_height(host, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(host, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(host, 0, 0);
    lv_obj_set_scrollable(host, false);
    lv_obj_set_scroll_chain(host, true);
    /* The full-screen transition shield, not this host, owns temporary input
     * rejection. Keeping the host interactive makes a card mounted during the
     * first open usable as soon as the entry animation settles. */
    lv_obj_set_clickable(host, true);
    /* Controller-owned host metadata. Provider children may use user_data for
     * their own semantics; only this host is interpreted as a card identity. */
    lv_obj_set_user_data(host, (void *)slot->app_id);
#ifdef HOST_TEST
    host_lv_set_object_identity(host, slot->app_id);
#endif

    void *state = NULL;
    const esp_err_t result = slot->ops->mount(host, &state);
    if (result != ESP_OK)
    {
        /* The contract requires the host tree to be gone before unmount(). */
        lv_obj_delete(host);
        if (state != NULL)
        {
            slot->ops->unmount(state);
        }
        _cc_schedule_card_retry(slot);
        return false;
    }

    size_t child_index = 0U;
    for (size_t index = 0U; index < slot_index; ++index)
    {
        if (s_cc.card_slots[index].mounted)
        {
            ++child_index;
        }
    }
    lv_obj_move_to_index(host, (int32_t)child_index);
    slot->host = host;
    slot->state = state;
    slot->mounted = true;
    slot->mount_failures = 0U;
    slot->retry_at = 0U;
    ++s_cc.mounted_card_count;
    return true;
}

static void _cc_retry_cards(void)
{
    bool changed = false;
    const uint32_t now = lv_tick_get();
    for (size_t index = 0U; index < s_cc.active_card_count; ++index)
    {
        cc_card_slot_t *slot = &s_cc.card_slots[index];
        if (slot->mounted || slot->retry_at == 0U ||
                (int32_t)(now - slot->retry_at) < 0)
        {
            continue;
        }
        changed = _cc_mount_card_slot(slot, index) || changed;
    }
    if (changed)
    {
        _cc_update_card_section();
        _cc_apply_panel_height();
        _cc_refresh_cards();
    }
}

/* Rebuild only when the set of active providers changes. A provider that is
 * already mounted keeps its objects and receives refresh() in place. Failed
 * providers remain in the slot table and get a bounded delayed retry. */
static void _cc_sync_cards(void)
{
    if (!s_cc.built || s_cc.cards == NULL ||
            !lv_obj_is_valid(s_cc.cards))
    {
        return;
    }
    cc_card_candidate_t active[CC_CARD_SLOT_MAX];
    const size_t active_count = _cc_collect_active_cards(active);
    if (_cc_cards_match(active, active_count))
    {
        _cc_refresh_cards();
        _cc_retry_cards();
        return;
    }

    _cc_clear_cards();
    for (size_t index = 0U; index < active_count; ++index)
    {
        /* Keep failed providers in the slot table. Their host is gone, but the
         * active provider set remains stable and healthy cards are not rebuilt
         * around them. */
        cc_card_slot_t *slot = &s_cc.card_slots[s_cc.active_card_count++];
        slot->ops = active[index].ops;
        slot->app_id = active[index].app_id;
        (void)_cc_mount_card_slot(slot, index);
    }
    _cc_update_card_section();
    _cc_apply_panel_height();
    _cc_refresh_cards();
}

static void _cc_build(void)
{
    const int32_t panel_w = s_cc.width - 2 * CC_CARD_MARGIN;
    const int32_t inner_w = panel_w - 2 * CC_PANEL_PAD;
    const bool has_wifi = s_cc.ops.get_wifi != NULL;
    const bool has_bt = s_cc.ops.get_bluetooth != NULL;
    const bool has_radios = has_wifi || has_bt;
    /* Both halves of the speaker bridge are required: a write-only row could
     * not show the level it applied, and a read-only row could not apply
     * anything. */
    const bool has_volume = s_cc.ops.get_volume != NULL &&
                            s_cc.ops.set_volume != NULL;

    lv_obj_t *panel = lv_obj_create(s_cc.root);
    s_cc.panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, CC_CARD_MARGIN, CC_CARD_MARGIN);
    lv_obj_set_width(panel, panel_w);
    _cc_apply_panel_height();
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_THEME_COLOR_SHEET), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 28, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_scrollable(panel, true);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scroll_elastic(panel, false);
    lv_obj_set_scroll_chain(panel, false);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* The page is content-sized so every section shares the panel's single
     * vertical scroll axis. It remains passive but keeps its vertical scroll
     * chain, allowing drags over labels, sliders and provider cards to climb
     * to the panel naturally. */
    lv_obj_t *page = lv_obj_create(panel);
    s_cc.page = page;
    lv_obj_remove_style_all(page);
    lv_obj_set_width(page, LV_PCT(100));
    lv_obj_set_height(page, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(page, CC_PANEL_PAD, 0);
    lv_obj_set_style_pad_row(page, CC_ROW_GAP, 0);
    lv_obj_set_scrollable(page, false);
    lv_obj_set_clickable(page, false);

    lv_obj_t *header = lv_obj_create(page);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, CC_HEADER_HEIGHT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollable(header, false);
    lv_obj_set_clickable(header, false);

    s_cc.clock_label = lv_label_create(header);
    lv_obj_set_style_text_color(s_cc.clock_label,
                                lv_color_hex(APP_THEME_COLOR_INK), 0);
    lv_obj_set_style_text_font(s_cc.clock_label,
                               app_manager_get_font(APP_THEME_FONT_BIGL), 0);
    lv_label_set_text(s_cc.clock_label, "--:--");

    s_cc.date_label = lv_label_create(header);
    lv_obj_set_style_text_color(s_cc.date_label,
                                lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
    lv_obj_set_style_text_font(s_cc.date_label,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_label_set_text(s_cc.date_label, "时间未同步");

    /* Brightness and speaker share one capsule row; both start on the same
     * 0..100 % scale, and both read the same fixed-width HEAD value. */
    (void)_cc_level_row(page, CC_IMAGE_BRIGHTNESS, LV_SYMBOL_IMAGE,
                        APP_THEME_COLOR_AMBER, APP_MANAGER_ID_SETTINGS_DISPLAY,
                        0, 100, 50, _cc_brightness_event,
                        &s_cc.brightness_slider, &s_cc.brightness_label);
    if (has_volume)
    {
        (void)_cc_level_row(page, CC_IMAGE_VOLUME, LV_SYMBOL_VOLUME_MAX,
                            APP_THEME_COLOR_AZURE, APP_MANAGER_ID_SETTINGS_SOUND,
                            0, 100, 0, _cc_volume_event,
                            &s_cc.volume_slider, &s_cc.volume_label);
    }
    else
    {
        s_cc.volume_slider = NULL;
        s_cc.volume_label = NULL;
    }

    if (has_radios)
    {
        lv_obj_t *tiles = lv_obj_create(page);
        lv_obj_remove_style_all(tiles);
        lv_obj_set_width(tiles, LV_PCT(100));
        lv_obj_set_height(tiles, CC_TILE_HEIGHT);
        lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(tiles, CC_ROW_GAP, 0);
        lv_obj_set_scrollable(tiles, false);
        lv_obj_set_clickable(tiles, false);

        if (has_wifi)
        {
            s_cc.wifi_gesture.settings_page = APP_MANAGER_ID_SETTINGS_WIFI;
            (void)_cc_tile(tiles, &s_cc.wifi_button, &s_cc.wifi_icon,
                           &s_cc.wifi_label, CC_IMAGE_WIFI, _cc_wifi_event,
                           &s_cc.wifi_gesture, "Wi-Fi", LV_SYMBOL_WIFI);
        }
        if (has_bt)
        {
            s_cc.bt_gesture.settings_page =
                APP_MANAGER_ID_SETTINGS_BLUETOOTH;
            (void)_cc_tile(tiles, &s_cc.bt_button, &s_cc.bt_icon,
                           &s_cc.bt_label, CC_IMAGE_BLUETOOTH, _cc_bt_event,
                           &s_cc.bt_gesture, "蓝牙", LV_SYMBOL_BLUETOOTH);
        }
    }

    /* Power actions belong to the system-control group, not to the App-card
     * section. Keep every control in the top flow and leave service-owned
     * cards last. */
    lv_obj_t *actions = lv_obj_create(page);
    s_cc.actions = actions;
    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, CC_ACTION_HEIGHT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, CC_ROW_GAP, 0);
    lv_obj_set_scrollable(actions, false);
    lv_obj_set_clickable(actions, false);

    const bool has_standby = app_manager_pm_is_standby_supported();
    lv_obj_t *off = lv_button_create(actions);
    lv_obj_t *standby = has_standby ? lv_button_create(actions) : NULL;
    lv_obj_t *buttons[2] = { off, standby };
    const uint32_t ids[2] = { CC_IMAGE_POWER, CC_IMAGE_STANDBY };
    const char *titles[2] = { "熄屏", "待机" };
    const char *icon_fallbacks[2] = { LV_SYMBOL_POWER, LV_SYMBOL_PAUSE };
    lv_event_cb_t cbs[2] = { _cc_screen_off_event, _cc_standby_event };
    const int32_t action_w = has_standby ?
                             (inner_w - CC_ROW_GAP) / 2 : inner_w / 2;
    for (size_t i = 0; i < 2U; i++)
    {
        if (buttons[i] == NULL)
        {
            continue;
        }
        lv_obj_set_width(buttons[i], action_w);
        lv_obj_set_height(buttons[i], CC_ACTION_HEIGHT);
        lv_obj_set_style_radius(buttons[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(buttons[i],
                                  lv_color_hex(APP_THEME_COLOR_PLUME_HI), 0);
        lv_obj_set_style_bg_color(buttons[i],
                                  lv_color_hex(APP_THEME_COLOR_TRACK),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_border_width(buttons[i], 0, 0);
        lv_obj_set_style_shadow_width(buttons[i], 0, 0);
        lv_obj_set_flex_flow(buttons[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(buttons[i], LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(buttons[i], 0, 0);
        lv_obj_set_style_pad_column(buttons[i], 8, 0);
        lv_obj_add_event_cb(buttons[i], cbs[i], LV_EVENT_CLICKED, NULL);
        /* Click-only: a press that slides off must not trigger the action. */
        lv_obj_set_press_lock(buttons[i], false);
        lv_obj_t *icon = _cc_icon(buttons[i], ids[i], 22, icon_fallbacks[i]);
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

    /* A real section boundary keeps system controls separate from service-owned
     * cards. The whole section collapses when no provider is active. */
    lv_obj_t *card_section = lv_obj_create(page);
    s_cc.card_section = card_section;
    lv_obj_remove_style_all(card_section);
    lv_obj_set_width(card_section, LV_PCT(100));
    lv_obj_set_height(card_section, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_section, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(card_section, 0, 0);
    lv_obj_set_style_pad_row(card_section, CC_CARD_GAP, 0);
    lv_obj_set_scrollable(card_section, false);
    lv_obj_set_clickable(card_section, false);
    lv_obj_set_hidden(card_section, true);

    lv_obj_t *divider = lv_obj_create(card_section);
    lv_obj_remove_style_all(divider);
    lv_obj_set_width(divider, LV_PCT(100));
    lv_obj_set_height(divider, CC_DIVIDER_H);
    lv_obj_set_style_bg_color(divider, lv_color_hex(APP_THEME_COLOR_TRACK), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_clickable(divider, false);
    lv_obj_set_scrollable(divider, false);

    s_cc.cards = lv_obj_create(card_section);
    lv_obj_remove_style_all(s_cc.cards);
    lv_obj_set_width(s_cc.cards, LV_PCT(100));
    lv_obj_set_height(s_cc.cards, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_cc.cards, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cc.cards, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_cc.cards, 0, 0);
    lv_obj_set_style_pad_row(s_cc.cards, CC_ROW_GAP, 0);
    lv_obj_set_scrollable(s_cc.cards, false);
    lv_obj_set_clickable(s_cc.cards, false);
    lv_obj_set_hidden(s_cc.cards, true);

    /* The bottom reserve is a transparent, independently interactive grab
     * surface. It owns deliberate drag-to-dismiss without stealing gestures
     * from the panel's page scroll. */
    s_cc.dismiss_strip = lv_obj_create(s_cc.root);
    lv_obj_remove_style_all(s_cc.dismiss_strip);
    lv_obj_set_pos(s_cc.dismiss_strip, 0, s_cc.height - CC_CARD_BOTTOM_RESERVE);
    lv_obj_set_size(s_cc.dismiss_strip, s_cc.width, CC_CARD_BOTTOM_RESERVE);
    lv_obj_set_style_bg_opa(s_cc.dismiss_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_cc.dismiss_strip, true);
    lv_obj_set_press_lock(s_cc.dismiss_strip, true);
    lv_obj_set_scrollable(s_cc.dismiss_strip, false);
    lv_obj_set_scroll_chain(s_cc.dismiss_strip, false);
    lv_obj_add_event_cb(s_cc.dismiss_strip, _cc_dismiss_strip_event,
                        LV_EVENT_ALL, NULL);

    /* This has to be a sibling above the panel: LVGL recursively resolves
     * clickable children, so press-locking the card container cannot stop a
     * child button while the panel is sliding. It only receives input during
     * transition states. */
    s_cc.input_shield = lv_obj_create(s_cc.root);
    lv_obj_remove_style_all(s_cc.input_shield);
    lv_obj_set_pos(s_cc.input_shield, 0, 0);
    lv_obj_set_size(s_cc.input_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_cc.input_shield, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_cc.input_shield, false);
    lv_obj_set_press_lock(s_cc.input_shield, false);
    lv_obj_set_hidden(s_cc.input_shield, true);
    lv_obj_move_to_index(s_cc.input_shield,
                         (int32_t)lv_obj_get_child_count(s_cc.root) - 1);
}

static void _cc_open_completed(lv_anim_t *animation)
{
    (void)animation;
    if (s_cc.state == CC_STATE_ENTERING)
    {
        _cc_set_state(CC_STATE_VISIBLE);
    }
}

/* Pull-down entry: the surface is on screen and interactive only after the
 * slide-in settles, which keeps a stray tap from hitting a radio tile while the
 * card is still moving. */
static void _cc_open(void)
{
    if (!s_cc.built)
    {
        _cc_build();
        s_cc.built = true;
    }
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_resume(s_cc.poll_timer);
    }
    _cc_reset_gesture();
    s_cc.brightness_dragging = false;
    s_cc.volume_dragging = false;
    s_cc.volume_attempt_failed = false;
    /* The stored level is unknown after a fresh open, so the first settle must
     * always be written rather than assumed. */
    s_cc.volume_persisted_valid = false;
    /* A stale window from the previous session must not swallow the first
     * step of the first drag after reopening. */
    s_cc.volume_apply_at = lv_tick_get() - CC_VOLUME_APPLY_MS;
    _cc_refresh_values();
    _cc_sync_cards();
    _cc_anim_stop();
    _cc_anim_apply(0);
    if (s_cc.panel != NULL)
    {
        lv_obj_set_hidden(s_cc.panel, false);
        lv_obj_scroll_to_y(s_cc.panel, 0, LV_ANIM_OFF);
    }
    lv_obj_set_hidden(s_cc.root, false);
    _cc_set_state(CC_STATE_ENTERING);
    _cc_refresh_band();
    _cc_anim_start(0, CC_ANIM_PROGRESS_MAX, lv_anim_path_ease_out,
                   _cc_open_completed);
}

static bool _cc_band_ready(void)
{
    return s_cc.initialized && s_cc.policy_enabled &&
           s_cc.state == CC_STATE_HIDDEN &&
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
    const int32_t edge_width = app_manager_back_gesture_edge_width();
    const int32_t left_inset = edge_width > CC_BAND_CONTROL_RESERVE_PX ?
                               edge_width : CC_BAND_CONTROL_RESERVE_PX;
    const int32_t band_width = s_cc.width - left_inset -
                               CC_BAND_CONTROL_RESERVE_PX;
    if (band_width <= 0)
    {
        lv_obj_delete(band);
        return NULL;
    }
    lv_obj_set_pos(band, left_inset, 0);
    lv_obj_set_size(band, band_width, CC_BAND_HEIGHT);
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

    const bool saved_enabled =
        atomic_load_explicit(&s_preinit_policy_configured,
                             memory_order_acquire) ?
        atomic_load_explicit(&s_preinit_policy_enabled,
                             memory_order_acquire) : true;
    app_manager_control_center_ops_t saved_ops = s_cc.ops;
    memset(&s_cc, 0, sizeof(s_cc));
    s_cc.ops = saved_ops;
    s_cc.policy_enabled = saved_enabled;
    atomic_store_explicit(&s_policy_published, saved_enabled,
                          memory_order_release);
    s_cc.display = display;
    s_cc.touch = touch;
    s_cc.width = lv_display_get_horizontal_resolution(disp);
    s_cc.height = lv_display_get_vertical_resolution(disp);
    if (s_cc.width <= 0 || s_cc.height <= 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (lv_indev_add_event_cb((lv_indev_t *)touch, _cc_touch_event,
                              LV_EVENT_ALL, NULL) == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_cc.band = _cc_create_band(system_layer);
    if (s_cc.band == NULL)
    {
        lv_indev_remove_event_cb_with_user_data((lv_indev_t *)touch,
                                                _cc_touch_event, NULL);
        return ESP_ERR_NO_MEM;
    }

    s_cc.root = lv_obj_create(system_layer);
    if (s_cc.root == NULL)
    {
        lv_obj_delete(s_cc.band);
        s_cc.band = NULL;
        lv_indev_remove_event_cb_with_user_data((lv_indev_t *)touch,
                                                _cc_touch_event, NULL);
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
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_pause(s_cc.poll_timer);
    }
    atomic_store_explicit(&s_cc_published, true, memory_order_release);
    _cc_refresh_band();
    return ESP_OK;
}

esp_err_t app_manager_control_center_deinit(void)
{
    if (!s_cc.initialized)
    {
        return ESP_OK;
    }
    if (s_cc.touch != NULL)
    {
        lv_indev_remove_event_cb_with_user_data((lv_indev_t *)s_cc.touch,
                                                _cc_touch_event, NULL);
    }
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_delete(s_cc.poll_timer);
        s_cc.poll_timer = NULL;
    }
    _cc_anim_stop();
    _cc_clear_cards();
    if (s_cc.root != NULL && lv_obj_is_valid(s_cc.root))
    {
        lv_obj_delete(s_cc.root);
    }
    if (s_cc.band != NULL && lv_obj_is_valid(s_cc.band))
    {
        lv_obj_delete(s_cc.band);
    }
    app_manager_control_center_ops_t ops = s_cc.ops;
    atomic_store_explicit(&s_cc_published, false, memory_order_release);
    atomic_store_explicit(&s_policy_published, false, memory_order_release);
    atomic_store_explicit(&s_visible_published, false, memory_order_release);
    memset(&s_cc, 0, sizeof(s_cc));
    s_cc.ops = ops;
    atomic_store_explicit(&s_preinit_policy_configured, false,
                          memory_order_release);
    return ESP_OK;
}

esp_err_t app_manager_control_center_show(void)
{
    if (!s_cc.initialized || s_cc.root == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (_cc_surface_visible())
    {
        _cc_close_animated();
        return ESP_OK;
    }
    /* Also covers a re-open while the slide-out is still running: the entry
     * resets progress from the top and cancels the pending close. */
    _cc_open();
    return ESP_OK;
}

esp_err_t app_manager_control_center_hide(void)
{
    if (!s_cc.initialized)
    {
        return ESP_OK;
    }
    _cc_close_now();
    return ESP_OK;
}

void app_manager_control_center_navigation_begin(void)
{
    if (!s_cc.initialized)
    {
        return;
    }
    s_cc.band_armed = false;
    _cc_close_now();
}

bool app_manager_control_center_is_visible(void)
{
    return atomic_load_explicit(&s_cc_published, memory_order_acquire) &&
           atomic_load_explicit(&s_visible_published, memory_order_acquire);
}

const char *app_manager_control_center_card_identity(const lv_obj_t *host)
{
    if (host == NULL)
    {
        return NULL;
    }
    for (size_t index = 0U; index < s_cc.active_card_count; ++index)
    {
        const cc_card_slot_t *slot = &s_cc.card_slots[index];
        if (slot->mounted && slot->host == host)
        {
            return slot->app_id;
        }
    }
    return NULL;
}

void app_manager_control_center_screen_suspend(void)
{
    if (!s_cc.initialized || !app_manager_mailbox_is_worker())
    {
        return;
    }
    _cc_close_now();
}

void app_manager_control_center_screen_resume(void)
{
    _cc_refresh_band();
}

static esp_err_t _cc_set_enabled_on_ui(void *context)
{
    if (!s_cc.initialized || context == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const bool enabled = *(const bool *)context;
    s_cc.policy_enabled = enabled;
    atomic_store_explicit(&s_policy_published, enabled, memory_order_release);
    if (!enabled)
    {
        _cc_close_now();
    }
    _cc_refresh_band();
    return ESP_OK;
}

esp_err_t app_manager_control_center_set_enabled(bool enabled)
{
    if (app_manager_mailbox_is_worker())
    {
        if (!s_cc.initialized)
        {
            atomic_store(&s_preinit_policy_enabled, enabled);
            atomic_store(&s_preinit_policy_configured, true);
            return ESP_OK;
        }
        return _cc_set_enabled_on_ui(&enabled);
    }
    const esp_err_t result = app_manager_ui_call(_cc_set_enabled_on_ui,
        &enabled, UINT32_MAX);
    if (result == ESP_ERR_INVALID_STATE)
    {
        /* Before init or during teardown: retain for the next init. */
        atomic_store(&s_preinit_policy_enabled, enabled);
        atomic_store(&s_preinit_policy_configured, true);
        return ESP_OK;
    }
    return result;
}

bool app_manager_control_center_is_enabled(void)
{
    return atomic_load_explicit(&s_cc_published, memory_order_acquire) &&
           atomic_load_explicit(&s_policy_published, memory_order_acquire);
}