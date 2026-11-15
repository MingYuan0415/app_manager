#define DBG_TAG "app_back_gesture"

#ifndef HOST_TEST
    #include "mt_log.h"
    #include "esp_attr.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #define APP_MANAGER_EXT_BSS EXT_RAM_BSS_ATTR
#else
    #include <stdio.h>
    #include <unistd.h>
    #define LOG_W(fmt, ...) fprintf(stderr, "[W][%s] " fmt "\n", DBG_TAG, ##__VA_ARGS__)
    #define APP_MANAGER_EXT_BSS
#endif

#include "app_manager.h"
#include "app_theme_colors.h"
#include "app_manager_back_gesture.h"
#include "app_manager_control_center.h"
#include "app_manager_lifecycle.h"
#include "app_manager_mailbox.h"
#include "app_manager_navigation.h"
#include "app_manager_task_switcher.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(LV_USE_CANVAS) || LV_USE_CANVAS == 0
    #error "app_manager back gesture requires CONFIG_LV_USE_CANVAS=y"
#endif

#define APP_MANAGER_GESTURE_EDGE_PERCENT          8
#define APP_MANAGER_GESTURE_EDGE_MIN_PX          24
#define APP_MANAGER_GESTURE_EDGE_MAX_PX          32
#define APP_MANAGER_GESTURE_TRIGGER_PERCENT      15
#define APP_MANAGER_GESTURE_TRIGGER_MIN_PX       48
#define APP_MANAGER_GESTURE_TRIGGER_MAX_PX       80
#define APP_MANAGER_GESTURE_SLOP_PERCENT          3
#define APP_MANAGER_GESTURE_SLOP_MIN_PX          10
#define APP_MANAGER_GESTURE_SLOP_MAX_PX          16
#define APP_MANAGER_GESTURE_HYSTERESIS_DIVISOR    5
#define APP_MANAGER_GESTURE_HYSTERESIS_MIN_PX    10
#define APP_MANAGER_GESTURE_HYSTERESIS_MAX_PX    18
#define APP_MANAGER_GESTURE_INDICATOR_WIDTH      64
#define APP_MANAGER_GESTURE_INDICATOR_HEIGHT    144
#define APP_MANAGER_GESTURE_INDICATOR_MIN_DEPTH  28
#define APP_MANAGER_GESTURE_INDICATOR_MAX_DEPTH  62
#define APP_MANAGER_GESTURE_INDICATOR_OPA        220
#define APP_MANAGER_GESTURE_INDICATOR_MARGIN      8
#define APP_MANAGER_GESTURE_INDICATOR_COLOR \
    APP_THEME_COLOR_PLUME_HI /* edge pill sits on VOID, not pure grey */
#define APP_MANAGER_GESTURE_ARROW_COLOR APP_THEME_COLOR_INK
#define APP_MANAGER_GESTURE_Q15_SHIFT             15U
#define APP_MANAGER_GESTURE_Q15_ONE               (1U << APP_MANAGER_GESTURE_Q15_SHIFT)
#define APP_MANAGER_GESTURE_Q15_HALF              (APP_MANAGER_GESTURE_Q15_ONE / 2U)

typedef enum app_manager_back_gesture_phase
{
    APP_MANAGER_GESTURE_IDLE = 0,
    /* A touch is down in the edge but has not yet travelled far enough to be
     * classified as a back gesture. The covered widget still owns the press
     * and may complete an ordinary click. */
    APP_MANAGER_GESTURE_PENDING,
    APP_MANAGER_GESTURE_TRACKING,
    APP_MANAGER_GESTURE_ARMED,
    APP_MANAGER_GESTURE_SUPPRESSED,
    APP_MANAGER_GESTURE_NAV_PENDING,
} app_manager_back_gesture_phase_t;

typedef enum app_manager_back_gesture_side
{
    APP_MANAGER_GESTURE_SIDE_LEFT = 0,
    APP_MANAGER_GESTURE_SIDE_RIGHT,
} app_manager_back_gesture_side_t;

typedef enum app_manager_back_gesture_overlay
{
    APP_MANAGER_GESTURE_OVERLAY_NONE = 0,
    APP_MANAGER_GESTURE_OVERLAY_CONTROL_CENTER,
    APP_MANAGER_GESTURE_OVERLAY_TASK_SWITCHER,
} app_manager_back_gesture_overlay_t;

typedef struct app_manager_back_gesture_state
{
    bool effective;
    bool direction_locked;
    bool cancelling;
    bool indicator_shape_valid;
    bool read_cb_installed;
    bool touch_pressed;
    bool ignore_until_release;
    app_manager_back_gesture_phase_t phase;
    app_manager_back_gesture_side_t side;
    app_manager_back_gesture_overlay_t overlay_target;
    app_manager_back_gesture_side_t indicator_shape_side;
    uint32_t generation;
    uint32_t pending_generation;
    int32_t width;
    int32_t height;
    int32_t edge_width;
    int32_t start_distance;
    int32_t trigger_distance;
    int32_t direction_slop;
    int32_t hysteresis;
    int32_t indicator_y;
    lv_point_t start_point;
    lv_display_t *display;
    lv_indev_t *touch;
    lv_indev_read_cb_t original_read_cb;
    lv_obj_t *left_marker;
    lv_obj_t *right_marker;
    lv_obj_t *indicator;
    lv_obj_t *arrow;
    app_manager_navigation_gesture_token_t navigation_token;
} app_manager_back_gesture_state_t;

static app_manager_back_gesture_state_t s_gesture;
static atomic_bool s_initialized;
static atomic_bool s_accepting;
static atomic_bool s_policy_enabled;
static atomic_uint s_api_users;

static _Alignas(LV_DRAW_BUF_ALIGN) APP_MANAGER_EXT_BSS uint8_t
s_indicator_draw_buf_data[
    LV_DRAW_BUF_SIZE(APP_MANAGER_GESTURE_INDICATOR_WIDTH,
                     APP_MANAGER_GESTURE_INDICATOR_HEIGHT,
                     LV_COLOR_FORMAT_A8)];
static lv_draw_buf_t s_indicator_draw_buf;

static void _back_gesture_refresh(void);

static int32_t _back_gesture_clamp(int32_t value, int32_t minimum,
                                   int32_t maximum)
{
    if (value < minimum)
    {
        value = minimum;
    }
    else if (value > maximum)
    {
        value = maximum;
    }
    return value;
}

static int32_t _back_gesture_percent(int32_t value, int32_t percent)
{
    return (value * percent + 50) / 100;
}

static bool _back_gesture_access_begin(void)
{
    bool admitted = atomic_load_explicit(&s_initialized,
                                         memory_order_acquire) &&
                    atomic_load_explicit(&s_accepting, memory_order_acquire);
    if (admitted)
    {
        atomic_fetch_add_explicit(&s_api_users, 1U, memory_order_acq_rel);
        admitted = atomic_load_explicit(&s_initialized,
                                        memory_order_acquire) &&
                   atomic_load_explicit(&s_accepting, memory_order_acquire);
        if (!admitted)
        {
            atomic_fetch_sub_explicit(&s_api_users, 1U,
                                      memory_order_acq_rel);
        }
    }
    return admitted;
}

static void _back_gesture_access_end(void)
{
    atomic_fetch_sub_explicit(&s_api_users, 1U, memory_order_acq_rel);
}

static void _back_gesture_wait_for_api_users(void)
{
    while (atomic_load_explicit(&s_api_users, memory_order_acquire) != 0U)
    {
#ifndef HOST_TEST
        vTaskDelay(1);
#else
        usleep(1000);
#endif
    }
}

static void _back_gesture_hide_indicator(void)
{
    if (s_gesture.indicator != NULL)
    {
        lv_obj_set_hidden(s_gesture.indicator, true);
    }
    if (s_gesture.arrow != NULL)
    {
        lv_obj_set_hidden(s_gesture.arrow, true);
    }
}

static void _back_gesture_set_effective(bool enabled)
{
    s_gesture.effective = enabled;
    if (s_gesture.left_marker != NULL)
    {
        lv_obj_set_hidden(s_gesture.left_marker, !enabled);
    }
    if (s_gesture.right_marker != NULL)
    {
        lv_obj_set_hidden(s_gesture.right_marker, !enabled);
    }
}

static bool _back_gesture_pointer_active(void)
{
    return s_gesture.phase == APP_MANAGER_GESTURE_PENDING ||
           s_gesture.phase == APP_MANAGER_GESTURE_TRACKING ||
           s_gesture.phase == APP_MANAGER_GESTURE_ARMED ||
           s_gesture.phase == APP_MANAGER_GESTURE_SUPPRESSED;
}

static void _back_gesture_cancel_pointer(bool wait_release)
{
    const bool pointer_active = _back_gesture_pointer_active();

    _back_gesture_hide_indicator();
    _back_gesture_set_effective(false);
    s_gesture.direction_locked = false;
    s_gesture.overlay_target = APP_MANAGER_GESTURE_OVERLAY_NONE;
    s_gesture.touch_pressed = false;
    if (pointer_active)
    {
        s_gesture.ignore_until_release = true;
    }
    if (s_gesture.phase != APP_MANAGER_GESTURE_NAV_PENDING)
    {
        s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
    }

    if (pointer_active && wait_release && s_gesture.touch != NULL &&
            !s_gesture.cancelling)
    {
        s_gesture.cancelling = true;
        lv_indev_wait_release(s_gesture.touch);
        lv_indev_reset(s_gesture.touch, NULL);
        s_gesture.cancelling = false;
    }
}

static bool _back_gesture_environment_available(void)
{
    return atomic_load_explicit(&s_accepting, memory_order_acquire) &&
           atomic_load_explicit(&s_policy_enabled, memory_order_acquire) &&
           app_manager_lifecycle_screen_mutation_allowed() &&
           !app_manager_is_transitioning();
}

/**
 * @brief Check whether a modal sheet owns the point below the edge gesture.
 *
 * A sheet scrim swallows every pointer event in the normal LVGL hit-test, but
 * the pass-through read callback runs before that hit-test. Sheets therefore
 * mark their scrim with user flag 1 and the read gate must decline before it
 * claims the press, leaving the sheet and its buttons untouched.
 */
static bool _back_gesture_modal_sheet_in_layer(lv_obj_t *layer,
        const lv_point_t *point)
{
    if (layer == NULL || point == NULL)
    {
        return false;
    }
    lv_point_t hit = *point;
    lv_obj_t *target = lv_indev_search_obj(layer, &hit);
    while (target != NULL)
    {
        if (lv_obj_get_user_flag(target, 1U))
        {
            return true;
        }
        target = lv_obj_get_parent(target);
    }
    return false;
}

static bool _back_gesture_modal_sheet_at(const lv_point_t *point)
{
    if (s_gesture.display == NULL || point == NULL)
    {
        return false;
    }
    /* An app-owned sheet lives under the active Screen. System surfaces such
     * as the task switcher live on the top or Sys layer; scan all three so a
     * scrim there also blocks edge gesture capture. */
    lv_obj_t *screen = lv_display_get_screen_active(s_gesture.display);
    if (_back_gesture_modal_sheet_in_layer(screen, point))
    {
        return true;
    }
    lv_obj_t *top = lv_display_get_layer_top(s_gesture.display);
    if (top != screen && _back_gesture_modal_sheet_in_layer(top, point))
    {
        return true;
    }
    lv_obj_t *sys = lv_display_get_layer_sys(s_gesture.display);
    return sys != screen && sys != top &&
           _back_gesture_modal_sheet_in_layer(sys, point);
}

static bool _back_gesture_overlay_visible(
    app_manager_back_gesture_overlay_t *target)
{
    if (app_manager_control_center_is_visible())
    {
        if (target != NULL)
        {
            *target = APP_MANAGER_GESTURE_OVERLAY_CONTROL_CENTER;
        }
        return true;
    }
    if (app_manager_task_switcher_is_visible())
    {
        if (target != NULL)
        {
            *target = APP_MANAGER_GESTURE_OVERLAY_TASK_SWITCHER;
        }
        return true;
    }
    return false;
}

static bool _back_gesture_target_allowed(
    const app_manager_navigation_gesture_token_t *token)
{
    const char *source_app_id = NULL;

    if (token == NULL ||
            !app_manager_navigation_gesture_token_is_current(token))
    {
        return false;
    }
    app_manager_lifecycle_back_target_t target =
        app_manager_lifecycle_back_target(&source_app_id);
    /* Home is the base App: it may leave a sub-page for its previous Page,
     * but its root must not gesture back into the App history. */
    const bool home = source_app_id != NULL &&
                      strcmp(source_app_id, APP_MANAGER_ID_HOME) == 0;
    return target != APP_MANAGER_LIFECYCLE_BACK_TARGET_NONE &&
           source_app_id != NULL &&
           (!home || target == APP_MANAGER_LIFECYCLE_BACK_TARGET_PAGE);
}

static bool _back_gesture_available(void)
{
    app_manager_navigation_gesture_token_t token;

    if (!_back_gesture_environment_available())
    {
        return false;
    }
    if (_back_gesture_overlay_visible(NULL))
    {
        return true;
    }
    if (!app_manager_navigation_gesture_token_acquire(&token))
    {
        return false;
    }
    return _back_gesture_target_allowed(&token);
}

static void _back_gesture_refresh(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            !app_manager_mailbox_is_worker())
    {
        return;
    }
    if (s_gesture.phase == APP_MANAGER_GESTURE_NAV_PENDING)
    {
        _back_gesture_set_effective(false);
        return;
    }
    if (_back_gesture_pointer_active())
    {
        return;
    }

    s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
    _back_gesture_hide_indicator();
    _back_gesture_set_effective(_back_gesture_available());
}

static int32_t _back_gesture_inward(const lv_point_t *point)
{
    return s_gesture.side == APP_MANAGER_GESTURE_SIDE_LEFT ?
           point->x - s_gesture.start_point.x :
           s_gesture.start_point.x - point->x;
}

static uint32_t _back_gesture_q15_multiply(uint32_t left, uint32_t right)
{
    return (uint32_t)(((uint64_t)left * right +
                       APP_MANAGER_GESTURE_Q15_HALF) >>
                      APP_MANAGER_GESTURE_Q15_SHIFT);
}

static uint32_t _back_gesture_indicator_depth_q15(int32_t y)
{
    const uint32_t denominator = APP_MANAGER_GESTURE_INDICATOR_HEIGHT - 1U;
    const uint32_t distance = (uint32_t)abs(2 * y - (int32_t)denominator);
    uint32_t normalized = (uint32_t)(((uint64_t)distance *
                                      APP_MANAGER_GESTURE_Q15_ONE +
                                      denominator / 2U) / denominator);
    uint32_t squared = _back_gesture_q15_multiply(normalized, normalized);
    uint32_t smooth = _back_gesture_q15_multiply(
                          squared, 3U * APP_MANAGER_GESTURE_Q15_ONE -
                          2U * normalized);

    smooth = smooth > APP_MANAGER_GESTURE_Q15_ONE ?
             APP_MANAGER_GESTURE_Q15_ONE : smooth;
    return APP_MANAGER_GESTURE_INDICATOR_MAX_DEPTH *
           APP_MANAGER_GESTURE_Q15_ONE -
           (APP_MANAGER_GESTURE_INDICATOR_MAX_DEPTH -
            APP_MANAGER_GESTURE_INDICATOR_MIN_DEPTH) * smooth;
}

static void _back_gesture_generate_indicator(
    app_manager_back_gesture_side_t side)
{
    if (s_gesture.indicator_shape_valid &&
            s_gesture.indicator_shape_side == side)
    {
        return;
    }

    const uint32_t stride = s_indicator_draw_buf.header.stride;
    memset(s_indicator_draw_buf.data, 0,
           stride * APP_MANAGER_GESTURE_INDICATOR_HEIGHT);
    for (int32_t y = 0; y < APP_MANAGER_GESTURE_INDICATOR_HEIGHT; y++)
    {
        const uint32_t depth_q15 = _back_gesture_indicator_depth_q15(y);
        const uint32_t full_pixels = depth_q15 >>
                                     APP_MANAGER_GESTURE_Q15_SHIFT;
        const uint32_t fraction = depth_q15 &
                                  (APP_MANAGER_GESTURE_Q15_ONE - 1U);
        uint8_t *row = s_indicator_draw_buf.data + (uint32_t)y * stride;

        for (uint32_t x = 0; x < full_pixels; x++)
        {
            const uint32_t target_x =
                side == APP_MANAGER_GESTURE_SIDE_LEFT ? x :
                APP_MANAGER_GESTURE_INDICATOR_WIDTH - 1U - x;
            row[target_x] = LV_OPA_COVER;
        }
        if (full_pixels < APP_MANAGER_GESTURE_INDICATOR_WIDTH && fraction != 0U)
        {
            const uint32_t target_x =
                side == APP_MANAGER_GESTURE_SIDE_LEFT ? full_pixels :
                APP_MANAGER_GESTURE_INDICATOR_WIDTH - 1U - full_pixels;
            row[target_x] = (uint8_t)((fraction * LV_OPA_COVER +
                                       APP_MANAGER_GESTURE_Q15_HALF) >>
                                      APP_MANAGER_GESTURE_Q15_SHIFT);
        }
    }

    lv_draw_buf_flush_cache(&s_indicator_draw_buf, NULL);
    s_gesture.indicator_shape_side = side;
    s_gesture.indicator_shape_valid = true;
    if (s_gesture.indicator != NULL)
    {
        lv_obj_invalidate(s_gesture.indicator);
    }
}

static void _back_gesture_render(int32_t inward)
{
    int32_t progress = _back_gesture_clamp(inward, 0,
                                           s_gesture.trigger_distance);
    int32_t visible_depth =
        (APP_MANAGER_GESTURE_INDICATOR_WIDTH * progress +
         s_gesture.trigger_distance / 2) / s_gesture.trigger_distance;
    int32_t x = s_gesture.side == APP_MANAGER_GESTURE_SIDE_LEFT ?
                visible_depth - APP_MANAGER_GESTURE_INDICATOR_WIDTH :
                s_gesture.width - visible_depth;

    lv_obj_set_pos(s_gesture.indicator, x, s_gesture.indicator_y);
    lv_obj_set_hidden(s_gesture.indicator, false);
}

static void _back_gesture_update(const lv_point_t *point)
{
    const int32_t inward = _back_gesture_inward(point);
    const int32_t vertical = abs(point->y - s_gesture.start_point.y);
    const int32_t positive_inward = inward > 0 ? inward : 0;

    if (s_gesture.overlay_target == APP_MANAGER_GESTURE_OVERLAY_NONE &&
            !app_manager_navigation_gesture_token_is_current(
                &s_gesture.navigation_token))
    {
        s_gesture.phase = APP_MANAGER_GESTURE_SUPPRESSED;
        _back_gesture_hide_indicator();
        return;
    }

    if (!s_gesture.direction_locked)
    {
        if (inward <= -s_gesture.direction_slop)
        {
            s_gesture.phase = APP_MANAGER_GESTURE_SUPPRESSED;
            _back_gesture_hide_indicator();
            return;
        }
        if (inward >= s_gesture.direction_slop &&
                vertical <= 2 * inward)
        {
            s_gesture.direction_locked = true;
        }
        else if (vertical >= 2 * s_gesture.direction_slop &&
                 vertical > 2 * positive_inward)
        {
            s_gesture.phase = APP_MANAGER_GESTURE_SUPPRESSED;
            _back_gesture_hide_indicator();
            return;
        }
    }
    if (s_gesture.direction_locked && inward < -s_gesture.direction_slop)
    {
        s_gesture.phase = APP_MANAGER_GESTURE_SUPPRESSED;
        _back_gesture_hide_indicator();
        return;
    }

    _back_gesture_render(inward);
    if (s_gesture.phase == APP_MANAGER_GESTURE_ARMED)
    {
        if (inward < s_gesture.trigger_distance - s_gesture.hysteresis)
        {
            s_gesture.phase = APP_MANAGER_GESTURE_TRACKING;
            lv_obj_set_hidden(s_gesture.arrow, true);
        }
    }
    else if (inward >= s_gesture.trigger_distance)
    {
        s_gesture.phase = APP_MANAGER_GESTURE_ARMED;
        lv_obj_set_hidden(s_gesture.arrow, false);
    }
}

static void _back_gesture_complete(esp_err_t result, void *context)
{
    const uint32_t generation = (uint32_t)(uintptr_t)context;
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            s_gesture.phase != APP_MANAGER_GESTURE_NAV_PENDING ||
            generation == 0U || generation != s_gesture.pending_generation)
    {
        return;
    }

    s_gesture.pending_generation = 0U;
    s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
    if (result != ESP_OK)
    {
        LOG_W("BACK completed with error: %d", (int)result);
    }
    _back_gesture_refresh();
}

static void _back_gesture_release(void)
{
    const bool armed = s_gesture.phase == APP_MANAGER_GESTURE_ARMED;
    const app_manager_back_gesture_overlay_t overlay =
        s_gesture.overlay_target;
    app_manager_navigation_gesture_token_t token =
        s_gesture.navigation_token;

    _back_gesture_hide_indicator();
    s_gesture.direction_locked = false;
    if (!armed)
    {
        s_gesture.overlay_target = APP_MANAGER_GESTURE_OVERLAY_NONE;
        s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
        _back_gesture_refresh();
        return;
    }

    if (overlay != APP_MANAGER_GESTURE_OVERLAY_NONE)
    {
        s_gesture.overlay_target = APP_MANAGER_GESTURE_OVERLAY_NONE;
        s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
        if (overlay == APP_MANAGER_GESTURE_OVERLAY_CONTROL_CENTER)
        {
            const esp_err_t result = app_manager_control_center_hide();
            if (result != ESP_OK)
            {
                LOG_W("control center dismiss failed: %d", (int)result);
            }
        }
        else
        {
            app_manager_task_switcher_navigation_begin();
        }
        _back_gesture_refresh();
        return;
    }

    uint32_t generation = s_gesture.generation + 1U;
    if (generation == 0U)
    {
        generation = 1U;
    }
    s_gesture.generation = generation;
    s_gesture.pending_generation = generation;
    s_gesture.phase = APP_MANAGER_GESTURE_NAV_PENDING;
    _back_gesture_set_effective(false);

    esp_err_t result = app_manager_navigation_submit_gesture_back(
                           &token, _back_gesture_complete,
                           (void *)(uintptr_t)generation);
    if (result != ESP_OK)
    {
        s_gesture.pending_generation = 0U;
        s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
        if (result != ESP_ERR_INVALID_STATE)
        {
            LOG_W("BACK admission failed: %d", (int)result);
        }
        _back_gesture_refresh();
    }
}

static bool _back_gesture_begin(const lv_point_t *point)
{
    if (point == NULL || s_gesture.phase != APP_MANAGER_GESTURE_IDLE ||
            point->x < 0 || point->x >= s_gesture.width ||
            point->y < 0 || point->y >= s_gesture.height)
    {
        return false;
    }

    const bool left_edge = point->x < s_gesture.edge_width;
    const bool right_edge = point->x >= s_gesture.width - s_gesture.edge_width;
    if (!left_edge && !right_edge)
    {
        return false;
    }
    if (!_back_gesture_environment_available())
    {
        return false;
    }

    app_manager_back_gesture_overlay_t overlay =
        APP_MANAGER_GESTURE_OVERLAY_NONE;
    if (_back_gesture_overlay_visible(&overlay))
    {
        /* System surfaces own the next release even when the page below has
         * no back target (for example Home root with Control Center open). */
    }
    else if (!s_gesture.effective || _back_gesture_modal_sheet_at(point) ||
             !app_manager_navigation_gesture_token_acquire(
                 &s_gesture.navigation_token) ||
             !_back_gesture_target_allowed(&s_gesture.navigation_token))
    {
        return false;
    }

    s_gesture.overlay_target = overlay;
    s_gesture.side = left_edge ? APP_MANAGER_GESTURE_SIDE_LEFT :
                     APP_MANAGER_GESTURE_SIDE_RIGHT;
    s_gesture.start_point = *point;
    s_gesture.indicator_y = _back_gesture_clamp(
                                point->y -
                                APP_MANAGER_GESTURE_INDICATOR_HEIGHT / 2,
                                APP_MANAGER_GESTURE_INDICATOR_MARGIN,
                                s_gesture.height -
                                APP_MANAGER_GESTURE_INDICATOR_HEIGHT -
                                APP_MANAGER_GESTURE_INDICATOR_MARGIN);
    s_gesture.direction_locked = false;
    s_gesture.phase = APP_MANAGER_GESTURE_PENDING;
    lv_obj_set_hidden(s_gesture.arrow, true);
    _back_gesture_generate_indicator(s_gesture.side);
    _back_gesture_render(0);
    _back_gesture_hide_indicator();
    return true;
}

static void _back_gesture_move(const lv_point_t *point)
{
    if (point == NULL)
    {
        return;
    }

    if (s_gesture.phase == APP_MANAGER_GESTURE_PENDING)
    {
        const int32_t inward = _back_gesture_inward(point);
        const int32_t vertical = abs(point->y - s_gesture.start_point.y);
        const int32_t positive_inward = inward > 0 ? inward : 0;

        /* Until the finger has travelled a deliberate distance, a tap or a
         * vertical scroll must keep the original widget's input intact. */
        if (inward <= -s_gesture.direction_slop ||
                (vertical >= 2 * s_gesture.direction_slop &&
                 vertical > 2 * positive_inward))
        {
            s_gesture.phase = APP_MANAGER_GESTURE_SUPPRESSED;
            return;
        }
        if (inward < s_gesture.start_distance)
        {
            return;
        }
        if (vertical > 2 * inward)
        {
            s_gesture.phase = APP_MANAGER_GESTURE_SUPPRESSED;
            return;
        }

        s_gesture.phase = APP_MANAGER_GESTURE_TRACKING;
        if (s_gesture.touch != NULL)
        {
            /* The gesture is real: cancel the widget press without letting
             * the eventual release turn into a click. */
            lv_indev_wait_release(s_gesture.touch);
        }
    }

    if (s_gesture.phase == APP_MANAGER_GESTURE_TRACKING ||
            s_gesture.phase == APP_MANAGER_GESTURE_ARMED)
    {
        _back_gesture_update(point);
    }
}

static void _back_gesture_touch_sample(bool pressed, const lv_point_t *point)
{
    if (!pressed)
    {
        /* Repeated RELEASED samples are normal while the read timer is idle.
         * Never let a later one clear a BACK that is waiting for its
         * completion callback. */
        if (s_gesture.touch_pressed &&
                s_gesture.phase != APP_MANAGER_GESTURE_IDLE &&
                s_gesture.phase != APP_MANAGER_GESTURE_NAV_PENDING)
        {
            _back_gesture_release();
        }
        s_gesture.touch_pressed = false;
        s_gesture.ignore_until_release = false;
        return;
    }

    if (!s_gesture.touch_pressed)
    {
        s_gesture.touch_pressed = true;
        if (s_gesture.ignore_until_release)
        {
            return;
        }
        (void)_back_gesture_begin(point);
        return;
    }

    if (!s_gesture.ignore_until_release)
    {
        _back_gesture_move(point);
    }
}

static void _back_gesture_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (s_gesture.original_read_cb != NULL)
    {
        s_gesture.original_read_cb(indev, data);
    }
    if (indev == s_gesture.touch && data != NULL)
    {
        _back_gesture_touch_sample(
            data->state == LV_INDEV_STATE_PRESSED, &data->point);
    }
}

static void _back_gesture_indev_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_INDEV_RESET ||
            s_gesture.cancelling)
    {
        return;
    }

    const bool was_pressed = s_gesture.touch_pressed;
    _back_gesture_cancel_pointer(false);
    s_gesture.touch_pressed = false;
    if (was_pressed)
    {
        s_gesture.ignore_until_release = true;
    }
    _back_gesture_refresh();
}

static lv_obj_t *_back_gesture_create_edge_marker(lv_obj_t *system_layer,
        int32_t x)
{
    lv_obj_t *marker = lv_obj_create(system_layer);
    if (marker == NULL)
    {
        return NULL;
    }
    lv_obj_remove_style_all(marker);
    lv_obj_set_pos(marker, x, 0);
    lv_obj_set_size(marker, s_gesture.edge_width, s_gesture.height);
    lv_obj_set_ignore_layout(marker, true);
    lv_obj_set_floating(marker, true);
    lv_obj_set_hidden(marker, true);
    /* Diagnostic availability marker only. It must never own pointer input:
     * the read callback classifies real gestures and ordinary taps continue
     * to reach the widget beneath the edge. */
    lv_obj_set_clickable(marker, false);
    lv_obj_set_click_focusable(marker, false);
    lv_obj_set_scrollable(marker, false);
    lv_obj_set_scroll_elastic(marker, false);
    lv_obj_set_scroll_momentum(marker, false);
    lv_obj_set_scroll_chain(marker, false);
    lv_obj_set_scroll_with_arrow(marker, false);
    lv_obj_set_snappable(marker, false);
    lv_obj_set_event_bubble(marker, false);
    lv_obj_set_gesture_bubble(marker, false);
    return marker;
}

static esp_err_t _back_gesture_create_objects(lv_obj_t *system_layer)
{
    s_gesture.left_marker = _back_gesture_create_edge_marker(system_layer, 0);
    if (s_gesture.left_marker == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    s_gesture.right_marker = _back_gesture_create_edge_marker(
                                 system_layer,
                                 s_gesture.width - s_gesture.edge_width);
    if (s_gesture.right_marker == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_gesture.indicator = lv_canvas_create(system_layer);
    if (s_gesture.indicator == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_draw_buf_init(&s_indicator_draw_buf,
                     APP_MANAGER_GESTURE_INDICATOR_WIDTH,
                     APP_MANAGER_GESTURE_INDICATOR_HEIGHT,
                     LV_COLOR_FORMAT_A8,
                     LV_DRAW_BUF_STRIDE(APP_MANAGER_GESTURE_INDICATOR_WIDTH,
                                        LV_COLOR_FORMAT_A8),
                     s_indicator_draw_buf_data,
                     sizeof(s_indicator_draw_buf_data));
#ifndef HOST_TEST
    lv_draw_buf_set_flag(&s_indicator_draw_buf, LV_IMAGE_FLAGS_MODIFIABLE);
#endif
    lv_canvas_set_draw_buf(s_gesture.indicator, &s_indicator_draw_buf);
    lv_obj_remove_style_all(s_gesture.indicator);
    lv_obj_set_size(s_gesture.indicator,
                    APP_MANAGER_GESTURE_INDICATOR_WIDTH,
                    APP_MANAGER_GESTURE_INDICATOR_HEIGHT);
    lv_obj_set_style_image_recolor(
        s_gesture.indicator,
        lv_color_hex(APP_MANAGER_GESTURE_INDICATOR_COLOR), 0);
    lv_obj_set_style_image_opa(s_gesture.indicator,
                               APP_MANAGER_GESTURE_INDICATOR_OPA, 0);
    lv_obj_set_ignore_layout(s_gesture.indicator, true);
    lv_obj_set_floating(s_gesture.indicator, true);
    lv_obj_set_hidden(s_gesture.indicator, true);
    lv_obj_set_clickable(s_gesture.indicator, false);
    lv_obj_set_click_focusable(s_gesture.indicator, false);
    lv_obj_set_scrollable(s_gesture.indicator, false);
    lv_obj_set_event_bubble(s_gesture.indicator, false);
    lv_obj_set_gesture_bubble(s_gesture.indicator, false);
    _back_gesture_generate_indicator(APP_MANAGER_GESTURE_SIDE_LEFT);

    s_gesture.arrow = lv_label_create(s_gesture.indicator);
    if (s_gesture.arrow == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_style_text_color(s_gesture.arrow,
                                lv_color_hex(APP_MANAGER_GESTURE_ARROW_COLOR),
                                0);
    lv_obj_set_style_text_font(s_gesture.arrow, LV_FONT_DEFAULT, 0);
    lv_label_set_text(s_gesture.arrow, LV_SYMBOL_LEFT);
    lv_obj_set_ignore_layout(s_gesture.arrow, true);
    lv_obj_set_floating(s_gesture.arrow, true);
    lv_obj_set_hidden(s_gesture.arrow, true);
    lv_obj_set_clickable(s_gesture.arrow, false);
    lv_obj_set_click_focusable(s_gesture.arrow, false);
    lv_obj_set_scrollable(s_gesture.arrow, false);
    lv_obj_set_event_bubble(s_gesture.arrow, false);
    lv_obj_set_gesture_bubble(s_gesture.arrow, false);
    lv_obj_center(s_gesture.arrow);
    return ESP_OK;
}

static void _back_gesture_delete_objects(void)
{
    if (s_gesture.indicator != NULL &&
            lv_obj_is_valid(s_gesture.indicator))
    {
        lv_obj_delete(s_gesture.indicator);
    }
    if (s_gesture.right_marker != NULL &&
            lv_obj_is_valid(s_gesture.right_marker))
    {
        lv_obj_delete(s_gesture.right_marker);
    }
    if (s_gesture.left_marker != NULL &&
            lv_obj_is_valid(s_gesture.left_marker))
    {
        lv_obj_delete(s_gesture.left_marker);
    }
    s_gesture.arrow = NULL;
    s_gesture.indicator = NULL;
    s_gesture.right_marker = NULL;
    s_gesture.left_marker = NULL;
}

esp_err_t app_manager_back_gesture_init(lv_display_t *display,
                                        lv_indev_t *touch)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    lv_obj_t *system_layer = NULL;

    if (atomic_load_explicit(&s_initialized, memory_order_acquire))
    {
        return ESP_OK;
    }
    if (!app_manager_mailbox_is_worker() || display == NULL || touch == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_gesture, 0, sizeof(s_gesture));
    s_gesture.display = display;
    s_gesture.touch = touch;
    s_gesture.width = lv_display_get_horizontal_resolution(display);
    s_gesture.height = lv_display_get_vertical_resolution(display);
    if (s_gesture.width <= 0 || s_gesture.height <= 0)
    {
        goto fail;
    }
    s_gesture.edge_width = _back_gesture_clamp(
                               _back_gesture_percent(s_gesture.width,
                                   APP_MANAGER_GESTURE_EDGE_PERCENT),
                               APP_MANAGER_GESTURE_EDGE_MIN_PX,
                               APP_MANAGER_GESTURE_EDGE_MAX_PX);
    /* A gesture is not classified until the finger has travelled the same
     * distance as the edge region. A tap, a vertical scroll or a tiny jitter
     * therefore stays with the widget that received the press. */
    s_gesture.start_distance = s_gesture.edge_width;
    s_gesture.trigger_distance = _back_gesture_clamp(
                                     _back_gesture_percent(s_gesture.width,
                                         APP_MANAGER_GESTURE_TRIGGER_PERCENT),
                                     APP_MANAGER_GESTURE_TRIGGER_MIN_PX,
                                     APP_MANAGER_GESTURE_TRIGGER_MAX_PX);
    s_gesture.direction_slop = _back_gesture_clamp(
                                   _back_gesture_percent(s_gesture.width,
                                       APP_MANAGER_GESTURE_SLOP_PERCENT),
                                   APP_MANAGER_GESTURE_SLOP_MIN_PX,
                                   APP_MANAGER_GESTURE_SLOP_MAX_PX);
    s_gesture.hysteresis = _back_gesture_clamp(
                               s_gesture.trigger_distance /
                               APP_MANAGER_GESTURE_HYSTERESIS_DIVISOR,
                               APP_MANAGER_GESTURE_HYSTERESIS_MIN_PX,
                               APP_MANAGER_GESTURE_HYSTERESIS_MAX_PX);

    system_layer = lv_display_get_layer_sys(display);
    if (system_layer == NULL)
    {
        goto fail;
    }
    result = _back_gesture_create_objects(system_layer);
    if (result != ESP_OK)
    {
        goto fail;
    }

    s_gesture.original_read_cb = lv_indev_get_read_cb(touch);
    lv_indev_set_read_cb(touch, _back_gesture_read_cb);
    s_gesture.read_cb_installed = true;
    if (lv_indev_add_event_cb(touch, _back_gesture_indev_event,
                              LV_EVENT_INDEV_RESET, NULL) == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    s_gesture.phase = APP_MANAGER_GESTURE_IDLE;
    atomic_store_explicit(&s_api_users, 0U, memory_order_release);
    atomic_store_explicit(&s_policy_enabled, true, memory_order_release);
    atomic_store_explicit(&s_initialized, true, memory_order_release);
    atomic_store_explicit(&s_accepting, true, memory_order_release);
    _back_gesture_refresh();
    return ESP_OK;

fail:
    if (s_gesture.touch != NULL && s_gesture.read_cb_installed)
    {
        lv_indev_remove_event_cb_with_user_data(
            s_gesture.touch, _back_gesture_indev_event, NULL);
        lv_indev_set_read_cb(s_gesture.touch, s_gesture.original_read_cb);
    }
    _back_gesture_delete_objects();
    memset(&s_gesture, 0, sizeof(s_gesture));
    return result;
}

static esp_err_t _back_gesture_shutdown_on_ui(void *context)
{
    (void)context;
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            !app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
    _back_gesture_cancel_pointer(true);
    return ESP_OK;
}

esp_err_t app_manager_back_gesture_shutdown_begin(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire))
    {
        return ESP_OK;
    }
    if (app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_accepting, false, memory_order_release);
    _back_gesture_wait_for_api_users();
    return app_manager_ui_call(_back_gesture_shutdown_on_ui, NULL, UINT32_MAX);
}

esp_err_t app_manager_back_gesture_deinit(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire))
    {
        return ESP_OK;
    }
    if (!app_manager_mailbox_is_worker() ||
            atomic_load_explicit(&s_accepting, memory_order_acquire) ||
            atomic_load_explicit(&s_api_users, memory_order_acquire) != 0U)
    {
        return ESP_ERR_INVALID_STATE;
    }

    _back_gesture_cancel_pointer(true);
    if (s_gesture.touch != NULL)
    {
        lv_indev_remove_event_cb_with_user_data(
            s_gesture.touch, _back_gesture_indev_event, NULL);
        if (s_gesture.read_cb_installed)
        {
            lv_indev_set_read_cb(s_gesture.touch, s_gesture.original_read_cb);
            s_gesture.read_cb_installed = false;
        }
    }
    _back_gesture_delete_objects();
    memset(&s_gesture, 0, sizeof(s_gesture));
    atomic_store_explicit(&s_policy_enabled, false, memory_order_release);
    atomic_store_explicit(&s_initialized, false, memory_order_release);
    return ESP_OK;
}

void app_manager_back_gesture_navigation_begin(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            !app_manager_mailbox_is_worker())
    {
        return;
    }
    _back_gesture_cancel_pointer(true);
}

void app_manager_back_gesture_navigation_settled(void)
{
    _back_gesture_refresh();
}

void app_manager_back_gesture_screen_suspend(void)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            !app_manager_mailbox_is_worker())
    {
        return;
    }
    _back_gesture_cancel_pointer(true);
}

void app_manager_back_gesture_screen_resume(void)
{
    /* Touch disable may happen before a release sample is delivered. Drop the
     * synthetic held state so the first press after resume is evaluated as a
     * fresh gesture instead of being treated as the old one. */
    s_gesture.touch_pressed = false;
    s_gesture.ignore_until_release = false;
    s_gesture.overlay_target = APP_MANAGER_GESTURE_OVERLAY_NONE;
    _back_gesture_refresh();
}

static esp_err_t _back_gesture_set_enabled_on_ui(void *context)
{
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            !app_manager_mailbox_is_worker() || context == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const bool enabled = *(const bool *)context;
    atomic_store_explicit(&s_policy_enabled, enabled, memory_order_release);
    if (!enabled)
    {
        _back_gesture_cancel_pointer(true);
    }
    else
    {
        _back_gesture_refresh();
    }
    return ESP_OK;
}

esp_err_t app_manager_back_gesture_set_enabled(bool enabled)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    bool screen_access_owned = false;

    if (!_back_gesture_access_begin())
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_manager_lifecycle_screen_access_begin())
    {
        atomic_store_explicit(&s_policy_enabled, enabled,
                              memory_order_release);
        result = ESP_OK;
        goto exit;
    }
    screen_access_owned = true;
    result = app_manager_ui_call(_back_gesture_set_enabled_on_ui,
                                 &enabled, UINT32_MAX);

exit:
    if (screen_access_owned)
    {
        app_manager_lifecycle_screen_access_end();
    }
    _back_gesture_access_end();
    return result;
}

bool app_manager_back_gesture_is_enabled(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire) &&
           atomic_load_explicit(&s_policy_enabled, memory_order_acquire);
}

int32_t app_manager_back_gesture_edge_width(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire) ?
           s_gesture.edge_width : 0;
}
