#define DBG_TAG "task_switcher"
#include "mt_log.h"

#include "app_manager.h"
#include "app_theme_colors.h"
#include "app_manager_lifecycle_internal.h"
#include "app_manager_navigation.h"
#include "app_manager_task_switcher.h"

#ifndef HOST_TEST
    #include "app_manager_mailbox.h"
#endif

#include "esp_heap_caps.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Single source of raw colours: app_theme (app_core REQUIRES app_theme, so
 * the shell shares the exact tokens every application page renders with). */
#define TS_COLOR_BACKGROUND     APP_THEME_COLOR_VOID
#define TS_COLOR_SURFACE        APP_THEME_COLOR_PLUME
#define TS_COLOR_SURFACE_HI     APP_THEME_COLOR_PLUME_HI
#define TS_COLOR_TEXT           APP_THEME_COLOR_INK
#define TS_COLOR_MUTED          APP_THEME_COLOR_INK_SOFT
#define TS_COLOR_FAINT          APP_THEME_COLOR_INK_FAINT
#define TS_COLOR_ACCENT         APP_THEME_COLOR_AZURE
#define TS_COLOR_ACCENT_TINT    APP_THEME_COLOR_AZURE_TINT
#define TS_COLOR_WARNING        APP_THEME_COLOR_CORAL
#define TS_COLOR_WARNING_TINT   APP_THEME_COLOR_CORAL_TINT
#define TS_COLOR_PENDING        APP_THEME_COLOR_AMBER
#define TS_COLOR_TRACK          APP_THEME_COLOR_TRACK

#define TS_HEADER_HEIGHT        52
#define TS_FOOTER_HEIGHT        64
#define TS_SIDE_PAD             16
#define TS_ROW_GAP              8
#define TS_CONTENT_PAD_VERT     6
#define TS_ROW_MIN_HEIGHT       74
#define TS_ROW_PAD_VERT         6
#define TS_ROW_PAD_HOR          12
#define TS_ICON_WELL            56
#define TS_ICON_SIZE            52
#define TS_CLOSE_SIZE           52
#define TS_CLEAR_WIDTH          108
#define TS_COUNT_WIDTH          28
#define TS_MEM_ROW_HEIGHT       24
#define TS_MEM_LABEL_WIDTH      84
#define TS_MEM_VALUE_WIDTH      112
#define TS_MEM_BAR_HEIGHT       6
#define TS_FOOTER_TOP_INSET     0
#define TS_FOOTER_BOTTOM_INSET  12  /* Keep clear of the panel's bottom corners. */
#define TS_MEM_WARN_PCT         85
#define TS_FADE_MS              140
#define TS_DRAG_SLOP            10
#define TS_SWIPE_CLOSE          64
#define TS_CLEAR_CONFIRM_MS     3000U
#define TS_CLEAR_TICK_MS        100U

typedef enum app_manager_task_switcher_state
{
    APP_MANAGER_TASK_SWITCHER_HIDDEN = 0,
    APP_MANAGER_TASK_SWITCHER_VISIBLE,
    APP_MANAGER_TASK_SWITCHER_MANAGING,
} app_manager_task_switcher_state_t;

typedef struct app_manager_task_switcher_card
{
    const char *app_id;
    lv_obj_t *root;
    lv_obj_t *status;
    int32_t press_x;
    int32_t press_y;
    bool press_moved;
    bool current;
} app_manager_task_switcher_card_t;

typedef struct app_manager_task_switcher_state_data
{
    bool initialized;
    lv_obj_t *root;
    lv_obj_t *content;
    lv_obj_t *busy;
    lv_obj_t *clear_button;
    lv_obj_t *clear_label;
    lv_obj_t *count_label;
    lv_obj_t *empty_group;
    lv_obj_t *ram_psram_row;
    lv_obj_t *ram_internal_bar;
    lv_obj_t *ram_internal_value;
    lv_obj_t *ram_psram_bar;
    lv_obj_t *ram_psram_value;
    app_manager_task_switcher_card_t cards[APP_MANAGER_MAX_RECENT_TASKS];
    size_t card_count;
    app_manager_task_switcher_state_t state;
    lv_timer_t *memory_timer;
    lv_timer_t *clear_timer;
    bool clear_armed;
    uint32_t clear_deadline_ms;
    int32_t backdrop_press_x;
    int32_t backdrop_press_y;
    bool backdrop_press_moved;
    app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
} app_manager_task_switcher_state_data_t;

static app_manager_task_switcher_state_data_t s_switcher;
static uint32_t s_switcher_generation;
static atomic_bool s_visible_published;

static void _switcher_rebuild(void);
static void _switcher_exit_complete(esp_err_t result, void *context);
static void _switcher_clear_complete(esp_err_t result, void *context);
static void _switcher_update_memory(void);
static void _switcher_clear_tick(lv_timer_t *timer);

static void _switcher_make_passive(lv_obj_t *object)
{
    lv_obj_set_clickable(object, false);
    lv_obj_set_click_focusable(object, false);
    lv_obj_set_scrollable(object, false);
}

static void _switcher_set_clear_confirm(bool armed)
{
    s_switcher.clear_armed = armed;
    if (s_switcher.clear_timer != NULL)
    {
        if (armed)
        {
            lv_timer_resume(s_switcher.clear_timer);
        }
        else
        {
            lv_timer_pause(s_switcher.clear_timer);
        }
    }
    if (s_switcher.clear_button != NULL)
    {
        lv_obj_set_style_bg_color(s_switcher.clear_button,
                                  lv_color_hex(armed ? TS_COLOR_WARNING_TINT :
                                               TS_COLOR_SURFACE_HI), 0);
    }
    if (s_switcher.clear_label != NULL)
    {
        lv_label_set_text(s_switcher.clear_label, armed ? "确认清除" : "清除");
        lv_obj_set_style_text_color(s_switcher.clear_label,
                                    lv_color_hex(armed ? TS_COLOR_WARNING :
                                            TS_COLOR_TEXT), 0);
    }
}

static void _switcher_cancel_clear_confirm(void)
{
    s_switcher.clear_deadline_ms = 0U;
    _switcher_set_clear_confirm(false);
}

static void _switcher_check_clear_confirm(void)
{
    if (!s_switcher.clear_armed)
    {
        return;
    }
    const uint32_t now = lv_tick_get();
    if ((int32_t)(now - s_switcher.clear_deadline_ms) >= 0)
    {
        _switcher_cancel_clear_confirm();
    }
}

static void _switcher_clear_tick(lv_timer_t *timer)
{
    (void)timer;
    _switcher_check_clear_confirm();
}

static void _switcher_destroy_cards(void)
{
    s_switcher_generation++;
    for (size_t index = 0U; index < s_switcher.card_count; index++)
    {
        if (s_switcher.cards[index].root != NULL &&
                lv_obj_is_valid(s_switcher.cards[index].root))
        {
            lv_obj_delete(s_switcher.cards[index].root);
        }
    }
    memset(s_switcher.cards, 0, sizeof(s_switcher.cards));
    s_switcher.card_count = 0U;
}

static void _switcher_set_busy(bool busy)
{
    if (busy && s_switcher.busy == NULL)
    {
        s_switcher.busy = lv_obj_create(s_switcher.root);
        if (s_switcher.busy == NULL)
        {
            return;
        }
        lv_obj_remove_style_all(s_switcher.busy);
        lv_obj_set_size(s_switcher.busy, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_switcher.busy,
                                  lv_color_hex(TS_COLOR_BACKGROUND), 0);
        lv_obj_set_style_bg_opa(s_switcher.busy, LV_OPA_60, 0);
        lv_obj_set_ignore_layout(s_switcher.busy, true);
        lv_obj_set_floating(s_switcher.busy, true);
        lv_obj_set_scrollable(s_switcher.busy, false);
        lv_obj_set_clickable(s_switcher.busy, true);
        lv_obj_move_to_index(s_switcher.busy, -1);

        lv_obj_t *label = lv_label_create(s_switcher.busy);
        if (label != NULL)
        {
            lv_obj_set_style_text_font(label,
                                       app_manager_get_font(APP_THEME_FONT_BODY), 0);
            lv_obj_set_style_text_color(label, lv_color_hex(TS_COLOR_MUTED), 0);
            lv_label_set_text(label, "处理中…");
            lv_obj_center(label);
            _switcher_make_passive(label);
        }
    }
    else if (!busy && s_switcher.busy != NULL)
    {
        lv_obj_delete(s_switcher.busy);
        s_switcher.busy = NULL;
    }
}

static void _switcher_hide(void)
{
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_HIDDEN)
    {
        return;
    }
    s_switcher.state = APP_MANAGER_TASK_SWITCHER_HIDDEN;
    s_switcher.backdrop_press_moved = false;
    atomic_store_explicit(&s_visible_published, false, memory_order_release);
    if (s_switcher.memory_timer != NULL)
    {
        lv_timer_pause(s_switcher.memory_timer);
    }
    _switcher_cancel_clear_confirm();
    if (s_switcher.root != NULL)
    {
        lv_anim_delete(s_switcher.root, NULL);
    }
    _switcher_destroy_cards();
    _switcher_set_busy(false);
    if (s_switcher.content != NULL)
    {
        lv_obj_clean(s_switcher.content);
    }
    if (s_switcher.empty_group != NULL)
    {
        lv_obj_set_hidden(s_switcher.empty_group, true);
    }
    if (s_switcher.root != NULL)
    {
        lv_obj_set_hidden(s_switcher.root, true);
    }
}

static void _switcher_fade_exec(void *variable, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)variable, (lv_opa_t)value, 0);
}

static void _switcher_start_fade(void)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, s_switcher.root);
    lv_anim_set_exec_cb(&animation, _switcher_fade_exec);
    lv_anim_set_values(&animation, 0, LV_OPA_COVER);
    lv_anim_set_duration(&animation, TS_FADE_MS);
    lv_anim_set_early_apply(&animation, true);
    (void)lv_anim_start(&animation);
}

static void _switcher_close_task(const char *app_id)
{
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    s_switcher.state = APP_MANAGER_TASK_SWITCHER_MANAGING;
    _switcher_cancel_clear_confirm();
    /* Remove stale task objects before the lifecycle changes residency. */
    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    if (s_switcher.empty_group != NULL)
    {
        lv_obj_set_hidden(s_switcher.empty_group, true);
    }
    if (s_switcher.clear_button != NULL)
    {
        lv_obj_set_hidden(s_switcher.clear_button, true);
    }
    _switcher_set_busy(true);
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT,
        .app_id = app_id,
    };
    esp_err_t result = app_manager_navigation_submit_switcher_exit(
                           &request, _switcher_exit_complete,
                           (void *)app_id);
    if (result != ESP_OK)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
    }
}

static void _switcher_activate_complete(esp_err_t result, void *context)
{
    if (result == ESP_OK)
    {
        return;
    }
    const char *app_id = (const char *)context;
    LOG_W("task switch failed: %s", esp_err_to_name(result));
    if (!app_manager_task_switcher_is_visible())
    {
        if (app_manager_task_switcher_show() != ESP_OK)
        {
            return;
        }
    }
    for (size_t index = 0U; index < s_switcher.card_count; ++index)
    {
        app_manager_task_switcher_card_t *card = &s_switcher.cards[index];
        if (card->status != NULL && card->app_id == app_id)
        {
            lv_label_set_text(card->status, "切换失败");
            lv_obj_set_style_text_color(card->status,
                                        lv_color_hex(TS_COLOR_WARNING), 0);
        }
    }
}

static void _switcher_activate_task(const char *app_id, bool current)
{
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    if (current)
    {
        _switcher_hide();
        return;
    }
    _switcher_cancel_clear_confirm();
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = app_id,
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_DEFAULT,
        },
    };
    esp_err_t admitted = app_manager_navigate_async(&request,
        _switcher_activate_complete, (void *)app_id);
    if (admitted != ESP_OK)
    {
        LOG_W("task switch admission failed: %s", esp_err_to_name(admitted));
    }
}

/* Both actions delete the row tree (hide/close), so they must not run inside
 * the CLICKED callback of the very object being destroyed. Capture the values
 * (app_id points at stable descriptor storage) plus the list generation at
 * event time and defer via lv_async_call: an exit/clear completion can rebuild
 * the list within the same frame, which would rebind a captured slot. */
typedef enum switcher_click_action
{
    SWITCHER_CLICK_ACTIVATE = 0,
    SWITCHER_CLICK_CLOSE,
} switcher_click_action_t;

typedef struct
{
    const char *app_id;
    bool current;
    uint32_t generation;
    switcher_click_action_t action;
} switcher_click_capture_t;

#define TS_CLICK_CAPTURE_SLOTS 4

static switcher_click_capture_t s_click_captures[TS_CLICK_CAPTURE_SLOTS];
static size_t s_click_capture_next;

static void _switcher_click_async(void *user_data)
{
    const switcher_click_capture_t *capture = user_data;
    if (capture->generation != s_switcher_generation)
    {
        return; /* List rebuilt since the click. */
    }
    if (capture->action == SWITCHER_CLICK_CLOSE)
    {
        _switcher_close_task(capture->app_id);
        return;
    }
    _switcher_activate_task(capture->app_id, capture->current);
}

static void _switcher_capture_click(
    const app_manager_task_switcher_card_t *card,
    switcher_click_action_t action)
{
    switcher_click_capture_t *capture =
        &s_click_captures[s_click_capture_next];
    s_click_capture_next =
        (s_click_capture_next + 1U) % TS_CLICK_CAPTURE_SLOTS;
    capture->app_id = card->app_id;
    capture->current = card->current;
    capture->generation = s_switcher_generation;
    capture->action = action;
    (void)lv_async_call(_switcher_click_async, capture);
}

/* LVGL withholds CLICKED only when a drag turns into a scroll, so a swipe or
 * a vertical drag released on the same row would still activate it. Track
 * press displacement and drop the click beyond a small slop. */
static void _switcher_drag_guard_event(lv_event_t *event)
{
    app_manager_task_switcher_card_t *card = lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL)
    {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED)
    {
        card->press_x = point.x;
        card->press_y = point.y;
        card->press_moved = false;
        return;
    }
    const int32_t dx = point.x - card->press_x;
    const int32_t dy = point.y - card->press_y;
    if (dx * dx + dy * dy > TS_DRAG_SLOP * TS_DRAG_SLOP)
    {
        card->press_moved = true;
    }
}

static void _switcher_card_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    app_manager_task_switcher_card_t *card = lv_event_get_user_data(event);
    if (card->press_moved)
    {
        return;
    }
    if (s_switcher.clear_armed)
    {
        _switcher_cancel_clear_confirm();
        return;
    }
    _switcher_capture_click(card, SWITCHER_CLICK_ACTIVATE);
}

/* The root and the scroll viewport also dismiss the switcher on a tap. Keep a
 * drag that starts and ends on the backdrop from turning into a dismissal:
 * LVGL's PRESS_LOCK keeps the original target, so CLICKED still arrives after
 * a move that never became a scroll. */
static void _switcher_backdrop_guard_event(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event))
    {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL)
    {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED)
    {
        s_switcher.backdrop_press_x = point.x;
        s_switcher.backdrop_press_y = point.y;
        s_switcher.backdrop_press_moved = false;
        return;
    }
    const int32_t dx = point.x - s_switcher.backdrop_press_x;
    const int32_t dy = point.y - s_switcher.backdrop_press_y;
    if (dx * dx + dy * dy > TS_DRAG_SLOP * TS_DRAG_SLOP)
    {
        s_switcher.backdrop_press_moved = true;
    }
}

static void _switcher_card_close_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    const app_manager_task_switcher_card_t *card =
        lv_event_get_user_data(event);
    if (card->press_moved || s_switcher.clear_armed)
    {
        if (s_switcher.clear_armed)
        {
            _switcher_cancel_clear_confirm();
        }
        return;
    }
    _switcher_capture_click(card, SWITCHER_CLICK_CLOSE);
}

static bool _switcher_card_is_left_swipe(
    const app_manager_task_switcher_card_t *card)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL)
    {
        return false;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int32_t dx = point.x - card->press_x;
    const int32_t dy = point.y - card->press_y;
    return dx <= -TS_SWIPE_CLOSE &&
           dy * dy <= TS_SWIPE_CLOSE * TS_SWIPE_CLOSE;
}

/* A leftward drag on any row closes it. The click is already suppressed by
 * _switcher_drag_guard_event once the drag passes the slop, so this only fires
 * for a real swipe, not a tap. */
static void _switcher_card_release_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED)
    {
        return;
    }
    const app_manager_task_switcher_card_t *card =
        lv_event_get_user_data(event);
    if (s_switcher.clear_armed)
    {
        return;
    }
    if (_switcher_card_is_left_swipe(card))
    {
        _switcher_capture_click(card, SWITCHER_CLICK_CLOSE);
    }
}

/* A row without PRESS_LOCK loses the pointer when a swipe leaves its bounds.
 * Catch that hand-off so a left swipe that ends on the backdrop still closes
 * the row that was originally pressed. */
static void _switcher_card_press_lost_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESS_LOST)
    {
        return;
    }
    const app_manager_task_switcher_card_t *card =
        lv_event_get_user_data(event);
    if (s_switcher.clear_armed)
    {
        return;
    }
    if (_switcher_card_is_left_swipe(card))
    {
        _switcher_capture_click(card, SWITCHER_CLICK_CLOSE);
    }
}

static void _switcher_format_mem(size_t used_bytes, size_t total_bytes,
                                 bool megabytes, char *out, size_t out_size)
{
    if (total_bytes == 0U)
    {
        (void)snprintf(out, out_size, "--");
        return;
    }
    if (megabytes)
    {
        (void)snprintf(out, out_size, "%u.%u/%u.%uM",
                       (unsigned)(used_bytes / 1048576U),
                       (unsigned)((used_bytes % 1048576U) * 10U / 1048576U),
                       (unsigned)(total_bytes / 1048576U),
                       (unsigned)((total_bytes % 1048576U) * 10U / 1048576U));
    }
    else
    {
        (void)snprintf(out, out_size, "%uk/%uk",
                       (unsigned)(used_bytes / 1024U),
                       (unsigned)(total_bytes / 1024U));
    }
}

static void _switcher_set_memory_row(lv_obj_t *bar, lv_obj_t *value,
                                     size_t free_bytes, size_t total_bytes,
                                     bool megabytes)
{
    char text[40];
    /* Bar and text both speak "used": a full bar next to a large free number
     * read as a contradiction. */
    const size_t used_bytes = total_bytes > free_bytes ?
                              total_bytes - free_bytes : 0U;
    _switcher_format_mem(used_bytes, total_bytes, megabytes, text,
                         sizeof(text));
    lv_label_set_text(value, text);
    const int used = total_bytes != 0U ?
                     (int)((used_bytes * 100U) / total_bytes) : 0;
    lv_bar_set_value(bar, used, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar,
                              lv_color_hex(used >= TS_MEM_WARN_PCT ?
                                           TS_COLOR_WARNING : TS_COLOR_ACCENT),
                              LV_PART_INDICATOR);
}

static void _switcher_update_memory(void)
{
    const size_t reported_internal_total = heap_caps_get_total_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT);
    const size_t psram_total = heap_caps_get_total_size(
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t internal_total = reported_internal_total;
    size_t internal_free = heap_caps_get_free_size(
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if CONFIG_SPIRAM
    if (psram_total != 0U)
    {
        /* The DMA/internal reserve pool is carved out of the general heap and
         * re-registered, so heap_caps reports it in both total and free size.
         * Remove it from both figures to keep the displayed used/total pair on
         * the same physical pool. */
        const size_t reserve =
            (size_t)CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL;
        internal_total = reported_internal_total > reserve ?
                         reported_internal_total - reserve :
                         reported_internal_total;
        internal_free = internal_free > reserve ?
                        internal_free - reserve : 0U;
    }
#endif
    const size_t psram_free = heap_caps_get_free_size(
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_switcher.ram_internal_bar != NULL)
    {
        _switcher_set_memory_row(s_switcher.ram_internal_bar,
                                 s_switcher.ram_internal_value,
                                 internal_free, internal_total, false);
    }
    if (s_switcher.ram_psram_row != NULL)
    {
        if (psram_total == 0U)
        {
            lv_obj_set_hidden(s_switcher.ram_psram_row, true);
        }
        else
        {
            lv_obj_set_hidden(s_switcher.ram_psram_row, false);
            _switcher_set_memory_row(s_switcher.ram_psram_bar,
                                     s_switcher.ram_psram_value,
                                     psram_free, psram_total, true);
        }
    }
}

static void _switcher_memory_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_HIDDEN)
    {
        _switcher_update_memory();
    }
}

static lv_obj_t *_switcher_make_passive_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    if (row == NULL)
    {
        return NULL;
    }
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, TS_MEM_ROW_HEIGHT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    _switcher_make_passive(row);
    return row;
}

static bool _switcher_build_memory_row(lv_obj_t *parent, const char *title,
                                       lv_obj_t **bar_out,
                                       lv_obj_t **value_out,
                                       lv_obj_t **row_out)
{
    lv_obj_t *row = _switcher_make_passive_row(parent);
    if (row == NULL)
    {
        return false;
    }

    lv_obj_t *name = lv_label_create(row);
    if (name == NULL)
    {
        return false;
    }
    lv_obj_set_width(name, TS_MEM_LABEL_WIDTH);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, lv_color_hex(TS_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(name,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_label_set_text(name, title);
    _switcher_make_passive(name);

    lv_obj_t *bar = lv_bar_create(row);
    if (bar == NULL)
    {
        return false;
    }
    lv_obj_set_width(bar, 0);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, TS_MEM_BAR_HEIGHT);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TS_COLOR_TRACK),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TS_COLOR_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    _switcher_make_passive(bar);

    lv_obj_t *value = lv_label_create(row);
    if (value == NULL)
    {
        return false;
    }
    lv_obj_set_width(value, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(value, TS_MEM_VALUE_WIDTH, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(value, lv_color_hex(TS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(value,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_label_set_text(value, "--");
    _switcher_make_passive(value);

    if (bar_out != NULL)
    {
        *bar_out = bar;
    }
    if (value_out != NULL)
    {
        *value_out = value;
    }
    if (row_out != NULL)
    {
        *row_out = row;
    }
    return true;
}

static lv_obj_t *_switcher_create_icon(lv_obj_t *parent, uint32_t icon_id,
                                       bool current)
{
    lv_obj_t *well = lv_obj_create(parent);
    if (well == NULL)
    {
        return NULL;
    }
    lv_obj_set_size(well, TS_ICON_WELL, TS_ICON_WELL);
    lv_obj_set_style_radius(well, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(well,
                              lv_color_hex(current ? TS_COLOR_ACCENT_TINT :
                                           TS_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(well, 0, 0);
    lv_obj_set_style_shadow_width(well, 0, 0);
    lv_obj_set_style_pad_all(well, 0, 0);
    _switcher_make_passive(well);
    lv_obj_set_gesture_bubble(well, false);

    const lv_image_dsc_t *icon_image = NULL;
    if (icon_id != 0U &&
            app_manager_get_image(icon_id, &icon_image) == ESP_OK &&
            icon_image != NULL)
    {
        lv_obj_t *image = lv_image_create(well);
        if (image != NULL)
        {
            lv_image_set_src(image, icon_image);
            lv_obj_set_size(image, TS_ICON_SIZE, TS_ICON_SIZE);
            lv_obj_center(image);
            _switcher_make_passive(image);
            lv_obj_set_gesture_bubble(image, false);
        }
    }
    else
    {
        lv_obj_t *symbol = lv_label_create(well);
        if (symbol != NULL)
        {
            lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
            lv_obj_set_style_text_color(symbol,
                                        lv_color_hex(TS_COLOR_FAINT), 0);
            lv_label_set_text(symbol, LV_SYMBOL_IMAGE);
            lv_obj_center(symbol);
            _switcher_make_passive(symbol);
        }
    }
    return well;
}

static void _switcher_apply_status(app_manager_task_switcher_card_t *card,
                                   const app_manager_recent_task_t *task)
{
    const char *text;
    uint32_t color;
    if (task->cleanup_pending)
    {
        text = "清理待重试";
        color = TS_COLOR_PENDING;
    }
    else if (card->current)
    {
        text = "前台运行";
        color = TS_COLOR_ACCENT;
    }
    else
    {
        text = "后台运行";
        color = TS_COLOR_MUTED;
    }
    lv_label_set_text(card->status, text);
    lv_obj_set_style_text_color(card->status, lv_color_hex(color), 0);
}

static void _switcher_add_card(const app_manager_recent_task_t *task)
{
    app_manager_task_switcher_card_t *card =
        &s_switcher.cards[s_switcher.card_count];
    card->app_id = task->app_id;
    card->current = task->active;
    card->status = NULL;

    lv_obj_t *button = lv_button_create(s_switcher.content);
    if (button == NULL)
    {
        return;
    }
    card->root = button;
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(button, TS_ROW_MIN_HEIGHT, 0);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(TS_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(TS_COLOR_SURFACE_HI),
                              LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_ver(button, TS_ROW_PAD_VERT, 0);
    lv_obj_set_style_pad_hor(button, TS_ROW_PAD_HOR, 0);
    lv_obj_set_style_pad_column(button, 12, 0);
    if (card->current)
    {
        /* Outline draws outside the content box, unlike border, so the
         * highlighted row keeps the same height as every other row. */
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_outline_width(button, 2, 0);
        lv_obj_set_style_outline_color(button, lv_color_hex(TS_COLOR_ACCENT), 0);
        lv_obj_set_style_outline_opa(button, LV_OPA_COVER, 0);
    }
    else
    {
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_outline_width(button, 0, 0);
    }
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(button, false);
    lv_obj_set_press_lock(button, false);
    lv_obj_add_event_cb(button, _switcher_drag_guard_event, LV_EVENT_PRESSED,
                        card);
    lv_obj_add_event_cb(button, _switcher_drag_guard_event, LV_EVENT_PRESSING,
                        card);
    lv_obj_add_event_cb(button, _switcher_card_release_event,
                        LV_EVENT_RELEASED, card);
    lv_obj_add_event_cb(button, _switcher_card_press_lost_event,
                        LV_EVENT_PRESS_LOST, card);
    lv_obj_add_event_cb(button, _switcher_card_event, LV_EVENT_CLICKED, card);

    (void)_switcher_create_icon(button, task->icon_id, card->current);

    /* Text block: name over state, both left aligned in the flexible middle. */
    lv_obj_t *text = lv_obj_create(button);
    if (text != NULL)
    {
        lv_obj_remove_style_all(text);
        lv_obj_set_width(text, 0);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(text, 2, 0);
        _switcher_make_passive(text);

        lv_obj_t *name = lv_label_create(text);
        if (name != NULL)
        {
            lv_obj_set_width(name, LV_PCT(100));
            lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(name, lv_color_hex(TS_COLOR_TEXT), 0);
            lv_obj_set_style_text_font(name,
                                       app_manager_get_font(APP_THEME_FONT_HEAD), 0);
            lv_label_set_text(name, task->display_name != NULL ?
                              task->display_name : task->app_id);
            _switcher_make_passive(name);
        }

        lv_obj_t *status = lv_label_create(text);
        if (status != NULL)
        {
            lv_obj_set_width(status, LV_PCT(100));
            lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(status,
                                       app_manager_get_font(APP_THEME_FONT_SMALL), 0);
            _switcher_make_passive(status);
            card->status = status;
            _switcher_apply_status(card, task);
        }
    }

    lv_obj_t *close = lv_button_create(button);
    if (close != NULL)
    {
        lv_obj_set_size(close, TS_CLOSE_SIZE, TS_CLOSE_SIZE);
        lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(close, lv_color_hex(TS_COLOR_SURFACE_HI),
                                  0);
        lv_obj_set_style_bg_color(close, lv_color_hex(TS_COLOR_TRACK),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_border_width(close, 0, 0);
        lv_obj_set_style_shadow_width(close, 0, 0);
        lv_obj_set_style_pad_all(close, 0, 0);
        lv_obj_set_style_transform_width(close, 0, LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(close, 0, LV_STATE_PRESSED);
        lv_obj_set_scrollable(close, false);
        lv_obj_set_press_lock(close, false);
        lv_obj_set_event_bubble(close, false);
        lv_obj_set_gesture_bubble(close, false);
        lv_obj_add_event_cb(close, _switcher_drag_guard_event,
                            LV_EVENT_PRESSED, card);
        lv_obj_add_event_cb(close, _switcher_drag_guard_event,
                            LV_EVENT_PRESSING, card);
        lv_obj_add_event_cb(close, _switcher_card_close_event,
                            LV_EVENT_CLICKED, card);
        lv_obj_t *mark = lv_label_create(close);
        if (mark != NULL)
        {
            lv_obj_set_style_text_font(mark, LV_FONT_DEFAULT, 0);
            lv_obj_set_style_text_color(mark, lv_color_hex(TS_COLOR_MUTED),
                                        0);
            lv_label_set_text(mark, LV_SYMBOL_CLOSE);
            lv_obj_center(mark);
            _switcher_make_passive(mark);
        }
    }

    s_switcher.card_count++;
}

static void _switcher_update_scroll_policy(void)
{
    if (s_switcher.content == NULL)
    {
        return;
    }
    lv_obj_update_layout(s_switcher.content);
    int32_t content_height = 2 * TS_CONTENT_PAD_VERT;
    const uint32_t child_count = lv_obj_get_child_count(s_switcher.content);
    for (uint32_t index = 0U; index < child_count; index++)
    {
        lv_obj_t *child = lv_obj_get_child(s_switcher.content,
                                           (int32_t)index);
        if (child == NULL)
        {
            continue;
        }
        if (index > 0U)
        {
            content_height += TS_ROW_GAP;
        }
        content_height += lv_obj_get_height(child);
    }
    const int32_t viewport_height = lv_obj_get_height(s_switcher.content);
    const bool scrollable = viewport_height > 0 &&
                            content_height > viewport_height;
    if (scrollable)
    {
        lv_obj_set_scrollable(s_switcher.content, true);
        lv_obj_set_scroll_dir(s_switcher.content, LV_DIR_VER);
    }
    else
    {
        lv_obj_set_scrollable(s_switcher.content, false);
        lv_obj_set_scroll_dir(s_switcher.content, LV_DIR_NONE);
    }
}

static void _switcher_rebuild(void)
{
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_HIDDEN ||
            s_switcher.content == NULL)
    {
        return;
    }
    /* Rebuild from the latest lifecycle snapshot. */
    _switcher_cancel_clear_confirm();
    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    app_manager_lifecycle_request_t request =
    {
        .tasks = s_switcher.tasks,
        .task_capacity = APP_MANAGER_MAX_RECENT_TASKS,
    };
    size_t count = 0U;
    if (app_manager_lifecycle_recent_tasks_call(&request) == ESP_OK)
    {
        count = request.tasks_filled;
        if (count > APP_MANAGER_MAX_RECENT_TASKS)
        {
            count = APP_MANAGER_MAX_RECENT_TASKS;
        }
    }
    if (s_switcher.count_label != NULL)
    {
        lv_label_set_text_fmt(s_switcher.count_label, "%u", (unsigned)count);
    }
    if (s_switcher.empty_group != NULL)
    {
        lv_obj_set_hidden(s_switcher.empty_group, count != 0U);
    }
    if (s_switcher.clear_button != NULL)
    {
        if (count == 0U ||
                s_switcher.state == APP_MANAGER_TASK_SWITCHER_MANAGING)
        {
            lv_obj_set_hidden(s_switcher.clear_button, true);
        }
        else
        {
            lv_obj_set_hidden(s_switcher.clear_button, false);
        }
    }
    for (size_t index = 0U; index < count; index++)
    {
        _switcher_add_card(&s_switcher.tasks[index]);
    }
    lv_obj_update_layout(s_switcher.content);
    _switcher_update_scroll_policy();
    lv_obj_scroll_to_y(s_switcher.content, 0, LV_ANIM_OFF);
    if (s_switcher.empty_group != NULL)
    {
        lv_obj_align_to(s_switcher.empty_group, s_switcher.content,
                        LV_ALIGN_CENTER, 0, 0);
    }
    _switcher_update_memory();
}

static void _switcher_backdrop_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
            lv_event_get_target(event) != lv_event_get_current_target(event) ||
            s_switcher.backdrop_press_moved ||
            s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    if (s_switcher.clear_armed)
    {
        _switcher_cancel_clear_confirm();
        return;
    }
    _switcher_hide();
}

static void _switcher_clear_clicked(lv_event_t *event)
{
    (void)event;
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    if (!s_switcher.clear_armed)
    {
        _switcher_set_clear_confirm(true);
        s_switcher.clear_deadline_ms = lv_tick_get() + TS_CLEAR_CONFIRM_MS;
        return;
    }
    if ((int32_t)(lv_tick_get() - s_switcher.clear_deadline_ms) >= 0)
    {
        _switcher_cancel_clear_confirm();
        return;
    }
    _switcher_cancel_clear_confirm();
    s_switcher.state = APP_MANAGER_TASK_SWITCHER_MANAGING;
    /* Remove stale task objects before activating Home. */
    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    if (s_switcher.empty_group != NULL)
    {
        lv_obj_set_hidden(s_switcher.empty_group, true);
    }
    if (s_switcher.clear_button != NULL)
    {
        lv_obj_set_hidden(s_switcher.clear_button, true);
    }
    _switcher_set_busy(true);
    esp_err_t result = app_manager_navigation_submit_system_op(
                           APP_MANAGER_NAV_SYSTEM_TASK_SWITCHER_CLEAR_ALL,
                           _switcher_clear_complete, NULL);
    if (result != ESP_OK)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
    }
}

static void _switcher_exit_complete(esp_err_t result, void *context)
{
    const char *app_id = (const char *)context;

    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_MANAGING)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
        if (result != ESP_OK)
        {
            for (size_t index = 0U; index < s_switcher.card_count; ++index)
            {
                app_manager_task_switcher_card_t *card =
                    &s_switcher.cards[index];
                if (card->status != NULL && card->app_id == app_id)
                {
                    lv_label_set_text(card->status, "关闭失败");
                    lv_obj_set_style_text_color(
                        card->status, lv_color_hex(TS_COLOR_WARNING), 0);
                }
            }
        }
    }
}

static void _switcher_clear_complete(esp_err_t result, void *context)
{
    (void)context;
    if (result == ESP_OK)
    {
        _switcher_hide();
        return;
    }
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_MANAGING)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
    }
}

static bool _switcher_build_header(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    if (header == NULL)
    {
        return false;
    }
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, TS_HEADER_HEIGHT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, TS_SIDE_PAD, 0);
    lv_obj_set_style_pad_column(header, 8, 0);
    _switcher_make_passive(header);

    lv_obj_t *title = lv_label_create(header);
    if (title == NULL)
    {
        return false;
    }
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(title, lv_color_hex(TS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title,
                               app_manager_get_font(APP_THEME_FONT_HEAD), 0);
    lv_label_set_text(title, "最近任务");
    _switcher_make_passive(title);

    s_switcher.count_label = lv_label_create(header);
    if (s_switcher.count_label == NULL)
    {
        return false;
    }
    lv_obj_set_size(s_switcher.count_label, TS_COUNT_WIDTH, TS_COUNT_WIDTH);
    lv_obj_set_style_bg_color(s_switcher.count_label,
                              lv_color_hex(TS_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(s_switcher.count_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_switcher.count_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_align(s_switcher.count_label, LV_TEXT_ALIGN_CENTER,
                                0);
    lv_obj_set_style_text_color(s_switcher.count_label,
                                lv_color_hex(TS_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_switcher.count_label,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_obj_set_style_pad_all(s_switcher.count_label, 0, 0);
    lv_label_set_text(s_switcher.count_label, "0");
    _switcher_make_passive(s_switcher.count_label);

    lv_obj_t *spacer = lv_obj_create(header);
    if (spacer == NULL)
    {
        return false;
    }
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 0);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    _switcher_make_passive(spacer);

    s_switcher.clear_button = lv_button_create(header);
    if (s_switcher.clear_button == NULL)
    {
        return false;
    }
    lv_obj_set_size(s_switcher.clear_button, TS_CLEAR_WIDTH, TS_CLOSE_SIZE);
    lv_obj_set_style_radius(s_switcher.clear_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_switcher.clear_button,
                              lv_color_hex(TS_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_color(s_switcher.clear_button,
                              lv_color_hex(TS_COLOR_TRACK),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_switcher.clear_button, 0, 0);
    lv_obj_set_style_shadow_width(s_switcher.clear_button, 0, 0);
    lv_obj_set_style_pad_all(s_switcher.clear_button, 0, 0);
    lv_obj_set_style_transform_width(s_switcher.clear_button, 0,
                                     LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(s_switcher.clear_button, 0,
                                      LV_STATE_PRESSED);
    lv_obj_set_scrollable(s_switcher.clear_button, false);
    lv_obj_set_press_lock(s_switcher.clear_button, false);
    lv_obj_add_event_cb(s_switcher.clear_button, _switcher_clear_clicked,
                        LV_EVENT_CLICKED, NULL);

    s_switcher.clear_label = lv_label_create(s_switcher.clear_button);
    if (s_switcher.clear_label == NULL)
    {
        return false;
    }
    lv_obj_set_style_text_color(s_switcher.clear_label,
                                lv_color_hex(TS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_switcher.clear_label,
                               app_manager_get_font(APP_THEME_FONT_BODY), 0);
    lv_label_set_text(s_switcher.clear_label, "清除");
    lv_obj_center(s_switcher.clear_label);
    _switcher_make_passive(s_switcher.clear_label);
    return true;
}

static bool _switcher_build_content(lv_obj_t *parent)
{
    s_switcher.content = lv_obj_create(parent);
    if (s_switcher.content == NULL)
    {
        return false;
    }
    lv_obj_remove_style_all(s_switcher.content);
    lv_obj_set_width(s_switcher.content, LV_PCT(100));
    lv_obj_set_height(s_switcher.content, 0);
    lv_obj_set_flex_grow(s_switcher.content, 1);
    lv_obj_set_flex_flow(s_switcher.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_switcher.content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(s_switcher.content, TS_SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(s_switcher.content, TS_CONTENT_PAD_VERT, 0);
    lv_obj_set_style_pad_row(s_switcher.content, TS_ROW_GAP, 0);
    lv_obj_set_scroll_dir(s_switcher.content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_switcher.content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_elastic(s_switcher.content, false);
    lv_obj_set_scroll_momentum(s_switcher.content, false);
    lv_obj_set_scrollable(s_switcher.content, true);
    lv_obj_add_event_cb(s_switcher.content, _switcher_backdrop_guard_event,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_switcher.content, _switcher_backdrop_guard_event,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_switcher.content, _switcher_backdrop_event,
                        LV_EVENT_CLICKED, NULL);
    return true;
}

static bool _switcher_build_footer(lv_obj_t *parent)
{
    lv_obj_t *footer = lv_obj_create(parent);
    if (footer == NULL)
    {
        return false;
    }
    lv_obj_remove_style_all(footer);
    lv_obj_set_width(footer, LV_PCT(100));
    lv_obj_set_height(footer, TS_FOOTER_HEIGHT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(footer, TS_SIDE_PAD, 0);
    lv_obj_set_style_pad_top(footer, TS_FOOTER_TOP_INSET, 0);
    lv_obj_set_style_pad_bottom(footer, TS_FOOTER_BOTTOM_INSET, 0);
    lv_obj_set_style_pad_row(footer, 4, 0);
    _switcher_make_passive(footer);

    if (!_switcher_build_memory_row(footer, "RAM",
                                    &s_switcher.ram_internal_bar,
                                    &s_switcher.ram_internal_value, NULL))
    {
        return false;
    }
    if (!_switcher_build_memory_row(footer, "PSRAM",
                                    &s_switcher.ram_psram_bar,
                                    &s_switcher.ram_psram_value,
                                    &s_switcher.ram_psram_row))
    {
        return false;
    }
    return true;
}

static bool _switcher_build_empty(lv_obj_t *parent)
{
    s_switcher.empty_group = lv_obj_create(parent);
    if (s_switcher.empty_group == NULL)
    {
        return false;
    }
    lv_obj_remove_style_all(s_switcher.empty_group);
    lv_obj_set_width(s_switcher.empty_group, LV_PCT(100));
    lv_obj_set_height(s_switcher.empty_group, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_switcher.empty_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_switcher.empty_group, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_switcher.empty_group, 0, 0);
    lv_obj_set_style_pad_row(s_switcher.empty_group, 6, 0);
    lv_obj_set_ignore_layout(s_switcher.empty_group, true);
    lv_obj_set_floating(s_switcher.empty_group, true);
    _switcher_make_passive(s_switcher.empty_group);

    lv_obj_t *badge = lv_obj_create(s_switcher.empty_group);
    if (badge == NULL)
    {
        return false;
    }
    lv_obj_set_size(badge, TS_ICON_WELL, TS_ICON_WELL);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(TS_COLOR_SURFACE_HI), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_shadow_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    _switcher_make_passive(badge);

    lv_obj_t *symbol = lv_label_create(badge);
    if (symbol != NULL)
    {
        lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(symbol, lv_color_hex(TS_COLOR_FAINT), 0);
        lv_label_set_text(symbol, LV_SYMBOL_LIST);
        lv_obj_center(symbol);
        _switcher_make_passive(symbol);
    }

    lv_obj_t *title = lv_label_create(s_switcher.empty_group);
    if (title == NULL)
    {
        return false;
    }
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(TS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title,
                               app_manager_get_font(APP_THEME_FONT_HEAD), 0);
    lv_label_set_text(title, "暂无最近任务");
    _switcher_make_passive(title);

    lv_obj_t *hint = lv_label_create(s_switcher.empty_group);
    if (hint == NULL)
    {
        return false;
    }
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(TS_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(hint,
                               app_manager_get_font(APP_THEME_FONT_SMALL), 0);
    lv_label_set_text(hint, "双击 HOME 打开任务切换");
    _switcher_make_passive(hint);

    lv_obj_set_hidden(s_switcher.empty_group, true);
    return true;
}

esp_err_t app_manager_task_switcher_init(void *display)
{
    if (s_switcher.initialized)
    {
        return ESP_OK;
    }
#ifndef HOST_TEST
    if (!app_manager_mailbox_is_worker())
    {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (display == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    lv_display_t *disp = (lv_display_t *)display;
    lv_obj_t *system_layer = lv_display_get_layer_sys(disp);
    if (system_layer == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (lv_display_get_horizontal_resolution(disp) <= 0 ||
            lv_display_get_vertical_resolution(disp) <= 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_switcher, 0, sizeof(s_switcher));

    s_switcher.root = lv_obj_create(system_layer);
    if (s_switcher.root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_switcher.root);
    lv_obj_set_size(s_switcher.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_switcher.root,
                              lv_color_hex(TS_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_switcher.root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_switcher.root, 0, 0);
    lv_obj_set_hidden(s_switcher.root, true);
    lv_obj_set_ignore_layout(s_switcher.root, true);
    lv_obj_set_floating(s_switcher.root, true);
    lv_obj_set_flex_flow(s_switcher.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_switcher.root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(s_switcher.root, false);
    lv_obj_set_clickable(s_switcher.root, true);
    lv_obj_add_event_cb(s_switcher.root, _switcher_backdrop_guard_event,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_switcher.root, _switcher_backdrop_guard_event,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_switcher.root, _switcher_backdrop_event,
                        LV_EVENT_CLICKED, NULL);

    if (!_switcher_build_header(s_switcher.root) ||
            !_switcher_build_content(s_switcher.root) ||
            !_switcher_build_footer(s_switcher.root) ||
            !_switcher_build_empty(s_switcher.root))
    {
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_obj_update_layout(s_switcher.root);
    lv_obj_align_to(s_switcher.empty_group, s_switcher.content,
                    LV_ALIGN_CENTER, 0, 0);

    s_switcher.state = APP_MANAGER_TASK_SWITCHER_HIDDEN;
    s_switcher.memory_timer = lv_timer_create(_switcher_memory_tick, 1000U,
        NULL);
    s_switcher.clear_timer = lv_timer_create(_switcher_clear_tick,
        TS_CLEAR_TICK_MS, NULL);
    if (s_switcher.memory_timer == NULL || s_switcher.clear_timer == NULL)
    {
        if (s_switcher.memory_timer != NULL)
        {
            lv_timer_delete(s_switcher.memory_timer);
        }
        if (s_switcher.clear_timer != NULL)
        {
            lv_timer_delete(s_switcher.clear_timer);
        }
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_switcher.memory_timer);
    lv_timer_pause(s_switcher.clear_timer);
    s_switcher.initialized = true;
    atomic_store_explicit(&s_visible_published, false, memory_order_release);
    return ESP_OK;
}

esp_err_t app_manager_task_switcher_deinit(void)
{
    if (!s_switcher.initialized)
    {
        return ESP_OK;
    }
    if (s_switcher.memory_timer != NULL)
    {
        lv_timer_delete(s_switcher.memory_timer);
        s_switcher.memory_timer = NULL;
    }
    if (s_switcher.clear_timer != NULL)
    {
        lv_timer_delete(s_switcher.clear_timer);
        s_switcher.clear_timer = NULL;
    }
    if (s_switcher.root != NULL)
    {
        lv_anim_delete(s_switcher.root, NULL);
        lv_obj_delete(s_switcher.root);
    }
    atomic_store_explicit(&s_visible_published, false, memory_order_release);
    memset(&s_switcher, 0, sizeof(s_switcher));
    return ESP_OK;
}

esp_err_t app_manager_task_switcher_show(void)
{
    if (!s_switcher.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_HIDDEN)
    {
        _switcher_hide();
        return ESP_OK;
    }
    s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
    _switcher_rebuild();

    atomic_store_explicit(&s_visible_published, true, memory_order_release);
    if (s_switcher.memory_timer != NULL)
    {
        lv_timer_resume(s_switcher.memory_timer);
    }
    lv_obj_set_hidden(s_switcher.root, false);
    lv_obj_move_to_index(s_switcher.root, -1);
    _switcher_start_fade();
    return ESP_OK;
}

void app_manager_task_switcher_navigation_begin(void)
{
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_HIDDEN)
    {
        return;
    }
    _switcher_hide();
}

bool app_manager_task_switcher_is_visible(void)
{
    return atomic_load_explicit(&s_visible_published, memory_order_acquire);
}

void app_manager_task_switcher_screen_suspend(void)
{
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_HIDDEN)
    {
        return;
    }
    _switcher_hide();
}

void app_manager_task_switcher_clear_all_finished(esp_err_t result)
{
    if (result == ESP_OK)
    {
        _switcher_hide();
        return;
    }
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_MANAGING)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
    }
}
