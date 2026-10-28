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
 * bezel missed the band and scrolled the page instead. The right end stops at
 * the page action-button reserve so the band never covers a page's own
 * top-right control. */
#define CC_BAND_HEIGHT            40
#define CC_BAND_EDGE_FALLBACK_PX  30
/* Reserved for app_ui_page_set_action(): a 42 px button plus the 16 px page
 * margin. Keep in sync with layers/apps/common/src/app_ui.c. */
#define CC_BAND_ACTION_RESERVE_PX 58
#define CC_BAND_TRIGGER_PX   48
#define CC_CARD_MARGIN       8
/* Card geometry. The card is sized from its rows at build time (see
 * _cc_build()): 70 px header block (the clock and date are aligned inside it),
 * then a 60 px slider row per slider, the 80 px tile row and the 56 px action
 * row, separated by 12 px gaps; the gap after the last row is the bottom
 * padding. Rows without an ops bridge are not built and the card shrinks with
 * them. */
#define CC_HEADER_HEIGHT     70
#define CC_ROW_GAP           12
#define CC_SLIDER_ROW_HEIGHT 60
#define CC_TILE_HEIGHT       80
#define CC_ACTION_HEIGHT     56
#define CC_WELL_SIZE         40
/* Widest reading is "100%": measured ink is 49 px at the SMALL role, so 52 px
 * keeps the track length stable without clipping. */
#define CC_VALUE_LABEL_W     52
#define CC_POLL_MS           1000U
/* Applying the speaker level is a synchronous codec control transfer (one I2C
 * register write, ~0.3 ms at the 100 kHz bus the codec opens with) taken under
 * the audio service lock, so a drag that crosses 100 steps would otherwise hold
 * that lock 100 times inside render callbacks. 60 ms keeps the audible change
 * continuous while capping the rate; the label still updates on every step.
 * Nothing in this path persists: storage is written once per drag through
 * set_volume_persisted, matching how brightness keeps its NVS write on release. */
#define CC_VOLUME_APPLY_MS   60U
#define CC_SCRIM_OPA         225

/* The surface transition follows the pull direction: the card slides down from
 * above while the scrim fades in. Duration matches the task switcher so the two
 * overlays feel alike. */
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
    lv_obj_t *clock_label;
    lv_obj_t *date_label;
    lv_obj_t *brightness_slider;
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

static void _cc_refresh_band(void);
static void _cc_refresh_values(void);

static void _cc_set_state(cc_state_t state)
{
    s_cc.state = state;
    atomic_store_explicit(&s_visible_published,
                          state == CC_STATE_ENTERING ||
                          state == CC_STATE_VISIBLE,
                          memory_order_release);
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
    s_cc.volume_dragging = false;
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

static void _cc_close_completed(lv_anim_t *animation)
{
    (void)animation;
    if (s_cc.state != CC_STATE_LEAVING)
    {
        return;
    }
    if (s_cc.root != NULL)
    {
        lv_obj_set_hidden(s_cc.root, true);
    }
    _cc_anim_apply(CC_ANIM_PROGRESS_MAX);
    _cc_set_state(CC_STATE_HIDDEN);
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

static void _cc_brightness_event(lv_event_t *event)
{
    if (s_cc.brightness_slider == NULL || s_cc.state != CC_STATE_VISIBLE)
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

static void _cc_volume_text(uint8_t percent)
{
    if (s_cc.volume_label != NULL)
    {
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
        }
        return;
    }
    lv_obj_remove_state(s_cc.volume_slider, LV_STATE_DISABLED);
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
    lv_obj_set_style_pad_column(tile, 10, 0);
    /* Icon on the left, name over state on the right; a stacked column cannot
     * fit both text lines plus the icon inside CC_TILE_HEIGHT. */
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
    lv_obj_t *icon = _cc_icon(well, image_id, 28, icon_fallback);
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
        lv_obj_set_style_pad_row(text, 1, 0);
        lv_obj_t *name = lv_label_create(text);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(name, lv_color_hex(APP_THEME_COLOR_INK), 0);
        lv_obj_set_style_text_font(name,
                                   app_manager_get_font(APP_THEME_FONT_BODY), 0);
        lv_label_set_text(name, title);
        lv_obj_t *state = lv_label_create(text);
        lv_obj_set_width(state, LV_PCT(100));
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(state,
                                    lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
        lv_obj_set_style_text_font(state,
                                   app_manager_get_font(APP_THEME_FONT_SMALL), 0);
        lv_label_set_text(state, "--");
        *label_out = state;
    }
    return tile;
}

/* Shared slider row: a 60 px rounded block holding the glyph, a growing
 * slider and an optional right-aligned value label. Both the brightness and
 * the speaker row use it so the two stay visually identical. */
static lv_obj_t *_cc_slider_row(lv_obj_t *panel, int32_t width, int32_t y,
                                uint32_t image_id, const char *icon_fallback,
                                uint32_t icon_color, const char *settings_page,
                                int32_t minimum, int32_t maximum, int32_t value,
                                lv_event_cb_t callback, lv_obj_t **slider_out,
                                lv_obj_t **value_label_out)
{
    lv_obj_t *row = lv_obj_create(panel);
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
    lv_obj_set_size(row, width, CC_SLIDER_ROW_HEIGHT);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 12, y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(APP_THEME_COLOR_PLUME_HI), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 30, 0);
    lv_obj_set_scrollable(row, false);
    /* The glyph and the row background are the long-press target; the slider
     * itself keeps its own press so a drag never turns into a settings jump.
     * Click-only, so sliding off the row cancels it. */
    if (settings_page != NULL)
    {
        lv_obj_add_event_cb(row, _cc_row_long_press, LV_EVENT_LONG_PRESSED,
                            (void *)settings_page);
        lv_obj_set_press_lock(row, false);
    }
    lv_obj_t *icon = _cc_icon(row, image_id, 24, icon_fallback);
    if (icon != NULL)
    {
        _cc_icon_color(icon, icon_color);
    }

    lv_obj_t *slider = lv_slider_create(row);
    if (slider != NULL)
    {
        lv_obj_set_flex_grow(slider, 1);
        lv_obj_set_height(slider, 20);
        lv_slider_set_range(slider, minimum, maximum);
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_THEME_COLOR_TRACK),
                                  LV_PART_MAIN);
        /* The 20 px knob overhangs the track by 2 px of knob padding on each
         * side; inset the track by the knob radius so the handle stays inside
         * the slider box at both ends instead of reaching into the value
         * label. */
        lv_obj_set_style_pad_left(slider, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_right(slider, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_THEME_COLOR_AZURE),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_hex(APP_THEME_COLOR_INK),
                                  LV_PART_KNOB);
        lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
        lv_obj_set_ext_click_area(slider, 16);
        lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(slider, callback, LV_EVENT_RELEASED, NULL);
        /* A finger that leaves the track mid-drag ends the gesture here; the
         * poll may re-sync the knob only after this. */
        lv_obj_add_event_cb(slider, callback, LV_EVENT_PRESS_LOST, NULL);
    }
    *slider_out = slider;

    if (value_label_out != NULL)
    {
        /* Fixed width built from the widest reading ("100%"), so the track
         * never changes length while the level moves. */
        lv_obj_t *value_label = lv_label_create(row);
        lv_obj_set_width(value_label, CC_VALUE_LABEL_W);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(value_label,
                                    lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
        lv_obj_set_style_text_font(value_label,
                                   app_manager_get_font(APP_THEME_FONT_SMALL), 0);
        lv_label_set_text(value_label, "--");
        *value_label_out = value_label;
    }
    return row;
}

static void _cc_build(void)
{
    const int32_t panel_w = s_cc.width - 2 * CC_CARD_MARGIN;
    const int32_t inner_w = panel_w - 24;
    const bool has_wifi = s_cc.ops.get_wifi != NULL;
    const bool has_bt = s_cc.ops.get_bluetooth != NULL;
    /* Both halves of the speaker bridge are required: a write-only row could
     * not show the level it applied, and a read-only row could not apply
     * anything. */
    const bool has_volume = s_cc.ops.get_volume != NULL &&
                            s_cc.ops.set_volume != NULL;

    /* Rows stack on one cursor; the trailing gap doubles as the bottom
     * padding, so the card ends exactly with its last row. */
    int32_t cursor = CC_HEADER_HEIGHT;
    cursor += CC_SLIDER_ROW_HEIGHT + CC_ROW_GAP;
    if (has_volume)
    {
        cursor += CC_SLIDER_ROW_HEIGHT + CC_ROW_GAP;
    }
    if (has_wifi || has_bt)
    {
        cursor += CC_TILE_HEIGHT + CC_ROW_GAP;
    }
    cursor += CC_ACTION_HEIGHT + CC_ROW_GAP;
    s_cc.card_height = cursor;

    lv_obj_t *panel = lv_obj_create(s_cc.root);
    s_cc.panel = panel;
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, CC_CARD_MARGIN, CC_CARD_MARGIN);
    lv_obj_set_size(panel, panel_w, s_cc.card_height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_THEME_COLOR_SHEET), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 28, 0);
    lv_obj_set_scrollable(panel, false);

    s_cc.clock_label = lv_label_create(panel);
    lv_obj_set_style_text_color(s_cc.clock_label,
                                lv_color_hex(APP_THEME_COLOR_INK), 0);
    lv_obj_set_style_text_font(s_cc.clock_label,
                               app_manager_get_font(APP_THEME_FONT_BIGL), 0);
    lv_obj_align(s_cc.clock_label, LV_ALIGN_TOP_LEFT, 20, 14);
    lv_label_set_text(s_cc.clock_label, "--:--");

    s_cc.date_label = lv_label_create(panel);
    lv_obj_set_style_text_color(s_cc.date_label,
                                lv_color_hex(APP_THEME_COLOR_INK_SOFT), 0);
    lv_obj_set_style_text_font(s_cc.date_label,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_obj_align(s_cc.date_label, LV_ALIGN_TOP_RIGHT, -20, 26);
    lv_label_set_text(s_cc.date_label, "时间未同步");

    int32_t y = CC_HEADER_HEIGHT;

    /* Brightness row. */
    (void)_cc_slider_row(panel, inner_w, y, CC_IMAGE_BRIGHTNESS,
                         LV_SYMBOL_IMAGE, APP_THEME_COLOR_AMBER,
                         APP_MANAGER_ID_SETTINGS_DISPLAY,
                         16, 255, 128, _cc_brightness_event,
                         &s_cc.brightness_slider, NULL);
    y += CC_SLIDER_ROW_HEIGHT + CC_ROW_GAP;

    /* Speaker volume row. */
    if (has_volume)
    {
        (void)_cc_slider_row(panel, inner_w, y, CC_IMAGE_VOLUME,
                             LV_SYMBOL_VOLUME_MAX, APP_THEME_COLOR_AZURE,
                             APP_MANAGER_ID_SETTINGS_SOUND,
                             0, 100, 0, _cc_volume_event,
                             &s_cc.volume_slider, &s_cc.volume_label);
        y += CC_SLIDER_ROW_HEIGHT + CC_ROW_GAP;
    }
    else
    {
        s_cc.volume_slider = NULL;
        s_cc.volume_label = NULL;
    }

    /* Wi-Fi / Bluetooth tiles. */
    if (has_wifi || has_bt)
    {
        lv_obj_t *tiles = lv_obj_create(panel);
        lv_obj_remove_style_all(tiles);
        lv_obj_set_size(tiles, inner_w, CC_TILE_HEIGHT);
        lv_obj_align(tiles, LV_ALIGN_TOP_LEFT, 12, y);
        y += CC_TILE_HEIGHT + CC_ROW_GAP;
        lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(tiles, 12, 0);
        lv_obj_set_scrollable(tiles, false);

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

    /* Power actions. */
    lv_obj_t *actions = lv_obj_create(panel);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, inner_w, CC_ACTION_HEIGHT);
    lv_obj_align(actions, LV_ALIGN_TOP_LEFT, 12, y);
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
    const char *icon_fallbacks[2] = { LV_SYMBOL_POWER, LV_SYMBOL_PAUSE };
    lv_event_cb_t cbs[2] = { _cc_screen_off_event, _cc_standby_event };
    for (size_t i = 0; i < 2U; i++)
    {
        if (buttons[i] == NULL)
        {
            continue;
        }
        lv_obj_set_height(buttons[i], CC_ACTION_HEIGHT);
        lv_obj_set_flex_grow(buttons[i], 1);
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
    const uint8_t brightness = app_manager_screen_get_brightness();
    if (s_cc.brightness_slider != NULL && brightness != UINT8_MAX)
    {
        lv_slider_set_value(s_cc.brightness_slider,
                            brightness < 16 ? 16 : (int32_t)brightness,
                            LV_ANIM_OFF);
    }
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_resume(s_cc.poll_timer);
    }
    s_cc.volume_dragging = false;
    s_cc.volume_attempt_failed = false;
    /* The stored level is unknown after a fresh open, so the first settle must
     * always be written rather than assumed. */
    s_cc.volume_persisted_valid = false;
    /* A stale window from the previous session must not swallow the first
     * step of the first drag after reopening. */
    s_cc.volume_apply_at = lv_tick_get() - CC_VOLUME_APPLY_MS;
    _cc_refresh_values();
    _cc_anim_stop();
    _cc_anim_apply(0);
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
    const int32_t left_inset = edge_width > 0 ?
                               edge_width : CC_BAND_EDGE_FALLBACK_PX;
    const int32_t band_width = s_cc.width - left_inset -
                               CC_BAND_ACTION_RESERVE_PX;
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

    s_cc.band = _cc_create_band(system_layer);
    if (s_cc.band == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_cc.root = lv_obj_create(system_layer);
    if (s_cc.root == NULL)
    {
        lv_obj_delete(s_cc.band);
        s_cc.band = NULL;
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
    if (s_cc.poll_timer != NULL)
    {
        lv_timer_delete(s_cc.poll_timer);
        s_cc.poll_timer = NULL;
    }
    _cc_anim_stop();
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