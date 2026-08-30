#define DBG_TAG "task_switcher"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_manager.h"
#include "app_manager_lifecycle_internal.h"
#include "app_manager_navigation.h"
#include "app_manager_task_switcher.h"

#ifndef HOST_TEST
    #include "app_manager_mailbox.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APP_MANAGER_TASK_SWITCHER_CARD_WIDTH  230
#define APP_MANAGER_TASK_SWITCHER_CARD_HEIGHT 336
#define APP_MANAGER_TASK_SWITCHER_CARD_GAP    12
#define APP_MANAGER_TASK_SWITCHER_CARD_PITCH  \
    (APP_MANAGER_TASK_SWITCHER_CARD_WIDTH + APP_MANAGER_TASK_SWITCHER_CARD_GAP)
#define APP_MANAGER_TASK_SWITCHER_SWIPE_UP_PX   40
#define APP_MANAGER_TASK_SWITCHER_SWIPE_X_SLOP_PX 60
#define APP_MANAGER_TASK_SWITCHER_PAGE_SWIPE_PX 40
#define APP_MANAGER_TASK_SWITCHER_TAP_SLOP_PX   10
#define APP_MANAGER_TASK_SWITCHER_FADE_MS     180

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
    bool current;
    bool click_suppressed;
} app_manager_task_switcher_card_t;

typedef struct app_manager_task_switcher_state_data
{
    bool initialized;
    lv_obj_t *root;
    lv_obj_t *content;
    lv_obj_t *busy;
    lv_obj_t *clear_button;
    lv_obj_t *empty_label;
    app_manager_task_switcher_card_t cards[APP_MANAGER_MAX_RECENT_TASKS];
    size_t card_count;
    app_manager_task_switcher_state_t state;
    app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
    bool gesture_active;
    app_manager_task_switcher_card_t *gesture_card;
    lv_point_t gesture_start;
    int32_t gesture_max_abs_dx;
    int32_t gesture_max_abs_dy;
    lv_point_t content_press_start;
    int32_t content_max_abs_dx;
    int32_t content_max_abs_dy;
    bool content_gesture_active;
    bool content_click_suppressed;
    size_t page_index;
    int32_t page_scroll_x;
} app_manager_task_switcher_state_data_t;

static app_manager_task_switcher_state_data_t s_switcher;

static void _switcher_rebuild(void);
static void _switcher_exit_complete(esp_err_t result, void *context);
static void _switcher_clear_complete(esp_err_t result, void *context);
static void _switcher_gesture_cancel(void);
static void _switcher_scroll_page(bool next);

static void _switcher_destroy_cards(void)
{
    _switcher_gesture_cancel();
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

static void _switcher_gesture_cancel(void)
{
    s_switcher.gesture_active = false;
    if (s_switcher.gesture_card != NULL &&
            s_switcher.gesture_card->root != NULL &&
            lv_obj_is_valid(s_switcher.gesture_card->root))
    {
        s_switcher.gesture_card->click_suppressed = false;
    }
    s_switcher.gesture_card = NULL;
    s_switcher.gesture_max_abs_dx = 0;
    s_switcher.gesture_max_abs_dy = 0;
    /* A background press must never survive a hide: its late CLICKED would
     * otherwise be accepted by the next visible session. */
    s_switcher.content_gesture_active = false;
    s_switcher.content_click_suppressed = false;
    s_switcher.content_max_abs_dx = 0;
    s_switcher.content_max_abs_dy = 0;
    if (s_switcher.content != NULL)
    {
        lv_obj_stop_scroll_anim(s_switcher.content);
        lv_obj_scroll_to_x(s_switcher.content, s_switcher.page_scroll_x,
                           LV_ANIM_OFF);
    }
}

static void _switcher_scroll_page(bool next)
{
    size_t target_page = s_switcher.page_index;
    if (next && target_page + 1U < s_switcher.card_count)
    {
        target_page++;
    }
    else if (!next && target_page > 0U)
    {
        target_page--;
    }
    else
    {
        return;
    }
    s_switcher.page_index = target_page;
    const int32_t target_x =
        (int32_t)target_page * APP_MANAGER_TASK_SWITCHER_CARD_PITCH;
    lv_obj_scroll_to_x(s_switcher.content, target_x, LV_ANIM_ON);
    lv_point_t end;
    lv_obj_get_scroll_end(s_switcher.content, &end);
    s_switcher.page_scroll_x = end.x;
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
        lv_obj_set_style_bg_opa(s_switcher.busy, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_switcher.busy, LV_OBJ_FLAG_IGNORE_LAYOUT |
                        LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(s_switcher.busy, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_to_index(s_switcher.busy, -1);
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
    if (s_switcher.root != NULL)
    {
        lv_obj_add_flag(s_switcher.root, LV_OBJ_FLAG_HIDDEN);
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
    lv_anim_set_duration(&animation, APP_MANAGER_TASK_SWITCHER_FADE_MS);
    lv_anim_set_early_apply(&animation, true);
    (void)lv_anim_start(&animation);
}

static void _switcher_close_task(app_manager_task_switcher_card_t *card)
{
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    s_switcher.state = APP_MANAGER_TASK_SWITCHER_MANAGING;
    /* Drop every preview-bearing card first so the EXIT transition can never
     * overwrite or invalidate a slot still bound to a visible image. */
    const char *app_id = card->app_id;
    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    if (s_switcher.empty_label != NULL)
    {
        lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_switcher.clear_button != NULL)
    {
        lv_obj_add_flag(s_switcher.clear_button, LV_OBJ_FLAG_HIDDEN);
    }
    _switcher_set_busy(true);
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_EXIT,
        .app_id = app_id,
    };
    esp_err_t result = app_manager_navigation_submit_switcher_exit(
                           &request, _switcher_exit_complete, card);
    if (result != ESP_OK)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
    }
}

static void _switcher_card_clicked(app_manager_task_switcher_card_t *card)
{
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    if (card->current)
    {
        _switcher_hide();
        return;
    }
    const app_manager_nav_request_t request =
    {
        .operation = APP_MANAGER_NAV_OP_RUN,
        .app_id = card->app_id,
        .transition =
        {
            .effect = APP_MANAGER_TRANSITION_DEFAULT,
        },
    };
    (void)app_manager_navigate_async(&request, NULL, NULL);
}

static void _switcher_card_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    app_manager_task_switcher_card_t *card = lv_event_get_user_data(event);

    if (code == LV_EVENT_PRESSED)
    {
        if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
        {
            return;
        }
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL)
        {
            return;
        }
        /* LVGL picked this card as the pointer target before the in-flight
         * page animation is settled, so the finger is on the card that is
         * about to move away. Sample the animation state before the cancel
         * below stops it, then ignore the whole sequence instead of letting
         * RELEASED/CLICKED act on the wrong card. */
        const bool page_animating = lv_obj_is_scrolling(s_switcher.content);
        _switcher_gesture_cancel();
        if (page_animating)
        {
            card->click_suppressed = true;
            return;
        }
        s_switcher.gesture_active = true;
        s_switcher.gesture_card = card;
        s_switcher.gesture_max_abs_dx = 0;
        s_switcher.gesture_max_abs_dy = 0;
        card->click_suppressed = false;
        lv_indev_get_point(indev, &s_switcher.gesture_start);
    }
    else if (code == LV_EVENT_PRESSING)
    {
        if (!s_switcher.gesture_active || s_switcher.gesture_card != card)
        {
            return;
        }
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL)
        {
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        const int32_t dx = point.x - s_switcher.gesture_start.x;
        const int32_t dy = point.y - s_switcher.gesture_start.y;
        const int32_t abs_dx = dx < 0 ? -dx : dx;
        const int32_t abs_dy = dy < 0 ? -dy : dy;
        if (abs_dx > s_switcher.gesture_max_abs_dx)
        {
            s_switcher.gesture_max_abs_dx = abs_dx;
        }
        if (abs_dy > s_switcher.gesture_max_abs_dy)
        {
            s_switcher.gesture_max_abs_dy = abs_dy;
        }
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (!s_switcher.gesture_active || s_switcher.gesture_card != card)
        {
            return;
        }
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL)
        {
            _switcher_gesture_cancel();
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        const int32_t dx = point.x - s_switcher.gesture_start.x;
        const int32_t dy = point.y - s_switcher.gesture_start.y;
        const int32_t abs_dx = dx < 0 ? -dx : dx;
        const int32_t abs_dy = dy < 0 ? -dy : dy;
        /* The release sample can carry movement that never produced a
         * PRESSING event, so fold it into the maxima before the slop test. */
        if (abs_dx > s_switcher.gesture_max_abs_dx)
        {
            s_switcher.gesture_max_abs_dx = abs_dx;
        }
        if (abs_dy > s_switcher.gesture_max_abs_dy)
        {
            s_switcher.gesture_max_abs_dy = abs_dy;
        }
        const bool close =
            dy <= -APP_MANAGER_TASK_SWITCHER_SWIPE_UP_PX &&
            abs_dy >= abs_dx &&
            abs_dx <= APP_MANAGER_TASK_SWITCHER_SWIPE_X_SLOP_PX;
        const bool page =
            abs_dx >= APP_MANAGER_TASK_SWITCHER_PAGE_SWIPE_PX &&
            abs_dx > abs_dy;
        /* Release gesture ownership first so the paths below that destroy
         * cards or start animations can never see a dangling reference. */
        s_switcher.gesture_active = false;
        s_switcher.gesture_card = NULL;
        card->click_suppressed =
            s_switcher.gesture_max_abs_dx >=
            APP_MANAGER_TASK_SWITCHER_TAP_SLOP_PX ||
            s_switcher.gesture_max_abs_dy >=
            APP_MANAGER_TASK_SWITCHER_TAP_SLOP_PX;
        if (close)
        {
            _switcher_close_task(card);
            return;
        }
        if (page)
        {
            _switcher_scroll_page(dx < 0);
        }
    }
    else if (code == LV_EVENT_CLICKED)
    {
        if (card->click_suppressed)
        {
            card->click_suppressed = false;
            return;
        }
        _switcher_card_clicked(card);
    }
    else if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL ||
             code == LV_EVENT_INDEV_RESET)
    {
        _switcher_gesture_cancel();
    }
}

static void _switcher_add_card(const app_manager_recent_task_t *task)
{
    app_manager_task_switcher_card_t *card =
        &s_switcher.cards[s_switcher.card_count];
    card->app_id = task->app_id;
    card->current = task->active;

    lv_obj_t *button = lv_button_create(s_switcher.content);
    if (button == NULL)
    {
        return;
    }
    card->root = button;
    lv_obj_set_size(button, APP_MANAGER_TASK_SWITCHER_CARD_WIDTH,
                    APP_MANAGER_TASK_SWITCHER_CARD_HEIGHT);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x171E22), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1D262B),
                              LV_STATE_PRESSED);
    /* The default theme grows pressed buttons by 2 px per side, but the
     * children and the post-drawn border are still clipped to the original
     * card rectangle, which would cut the highlight border while pressed.
     * The pressed background color above is the only feedback wanted. */
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 8, 0);
    lv_obj_set_style_pad_gap(button, 8, 0);
    if (card->current)
    {
        lv_obj_set_style_border_width(button, 2, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x4FC3F7), 0);
        /* The preview is as wide as the card and overflows the 210 px
         * content area by 10 px per side, covering the border during the
         * main draw pass. Drawing the highlight border after the children
         * keeps both vertical edges visible at rest; the default theme's
         * pressed grow would otherwise make the border pop in and out. */
        lv_obj_set_style_border_post(button, true, 0);
    }
    else
    {
        lv_obj_set_style_border_width(button, 0, 0);
    }
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_SCROLL_CHAIN |
                       LV_OBJ_FLAG_SCROLL_ELASTIC |
                       LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_event_cb(button, _switcher_card_event, LV_EVENT_ALL, card);

    if (task->preview_valid && task->preview != NULL)
    {
        lv_obj_t *preview = lv_image_create(button);
        if (preview != NULL)
        {
            lv_image_set_src(preview, task->preview);
            lv_obj_set_size(preview, APP_MANAGER_RECENT_PREVIEW_WIDTH,
                            APP_MANAGER_RECENT_PREVIEW_HEIGHT);
            lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
    else
    {
        lv_obj_t *placeholder = lv_obj_create(button);
        if (placeholder != NULL)
        {
            lv_obj_set_size(placeholder, APP_MANAGER_RECENT_PREVIEW_WIDTH,
                            APP_MANAGER_RECENT_PREVIEW_HEIGHT);
            lv_obj_set_style_bg_color(placeholder, lv_color_hex(0x101517), 0);
            lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(placeholder, 2, 0);
            lv_obj_remove_flag(placeholder,
                               LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *symbol = lv_label_create(placeholder);
            if (symbol != NULL)
            {
                lv_label_set_text(symbol, LV_SYMBOL_IMAGE);
                lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
                lv_obj_set_style_text_color(symbol, lv_color_hex(0x4A555A), 0);
                lv_obj_center(symbol);
            }
        }
    }

    /* The icon is an overlay so the existing preview/card geometry remains
     * stable.  Missing or zero IDs intentionally use the same neutral symbol
     * fallback as an unavailable preview. */
    lv_obj_t *icon = lv_obj_create(button);
    if (icon != NULL)
    {
        lv_obj_set_size(icon, 44, 44);
        lv_obj_set_pos(icon, 14, 14);
        lv_obj_set_style_radius(icon, 6, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(0x101517), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_90, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        const lv_image_dsc_t *icon_image = NULL;
        if (task->icon_id != 0U &&
                app_manager_get_image(task->icon_id, &icon_image) == ESP_OK &&
                icon_image != NULL)
        {
            lv_obj_t *image = lv_image_create(icon);
            if (image != NULL)
            {
                lv_image_set_src(image, icon_image);
                lv_obj_set_size(image, 40, 40);
                lv_obj_center(image);
                lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_CLICKABLE);
            }
        }
        else
        {
            lv_obj_t *symbol = lv_label_create(icon);
            if (symbol != NULL)
            {
                lv_label_set_text(symbol, LV_SYMBOL_IMAGE);
                lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
                lv_obj_set_style_text_color(symbol, lv_color_hex(0x4A555A), 0);
                lv_obj_center(symbol);
            }
        }
    }

    lv_obj_t *name = lv_label_create(button);
    if (name != NULL)
    {
        lv_label_set_text_fmt(name, "%s%s",
                              task->display_name != NULL ?
                              task->display_name : task->app_id,
                              card->current ? " · 当前" : "");
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xF4F7F8), 0);
        lv_obj_set_style_text_font(name, app_manager_get_font(APP_THEME_FONT_BODY), 0);
    }

    s_switcher.card_count++;
}

static void _switcher_rebuild(void)
{
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_HIDDEN ||
            s_switcher.content == NULL)
    {
        return;
    }
    /* Destroy every preview-bearing card before the capture so a freed slot
     * can never be overwritten while an image still references it. */
    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    (void)app_manager_lifecycle_capture_active_recents();
    app_manager_lifecycle_request_t request =
    {
        .tasks = s_switcher.tasks,
        .task_capacity = APP_MANAGER_MAX_RECENT_TASKS,
    };
    size_t count = 0U;
    if (app_manager_lifecycle_recent_tasks_call(&request) == ESP_OK)
    {
        count = request.tasks_filled;
    }
    if (s_switcher.empty_label != NULL)
    {
        if (count == 0U)
        {
            lv_obj_remove_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_switcher.clear_button != NULL)
    {
        if (count == 0U ||
                s_switcher.state == APP_MANAGER_TASK_SWITCHER_MANAGING)
        {
            lv_obj_add_flag(s_switcher.clear_button, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_remove_flag(s_switcher.clear_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t index = 0U; index < count; index++)
    {
        _switcher_add_card(&s_switcher.tasks[index]);
    }
    /* A changed list restarts at the first MRU card: a stale page index
     * could point at a task that no longer exists. */
    lv_obj_update_layout(s_switcher.content);
    s_switcher.page_index = 0U;
    lv_obj_scroll_to_x(s_switcher.content, 0, LV_ANIM_OFF);
    s_switcher.page_scroll_x = lv_obj_get_scroll_x(s_switcher.content);
}

static void _switcher_content_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED)
    {
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL)
        {
            return;
        }
        lv_indev_get_point(indev, &s_switcher.content_press_start);
        s_switcher.content_max_abs_dx = 0;
        s_switcher.content_max_abs_dy = 0;
        s_switcher.content_click_suppressed = false;
        s_switcher.content_gesture_active = true;
    }
    else if (code == LV_EVENT_PRESSING)
    {
        if (!s_switcher.content_gesture_active)
        {
            return;
        }
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL)
        {
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        const int32_t dx = point.x - s_switcher.content_press_start.x;
        const int32_t dy = point.y - s_switcher.content_press_start.y;
        const int32_t abs_dx = dx < 0 ? -dx : dx;
        const int32_t abs_dy = dy < 0 ? -dy : dy;
        if (abs_dx > s_switcher.content_max_abs_dx)
        {
            s_switcher.content_max_abs_dx = abs_dx;
        }
        if (abs_dy > s_switcher.content_max_abs_dy)
        {
            s_switcher.content_max_abs_dy = abs_dy;
        }
    }
    else if (code == LV_EVENT_RELEASED)
    {
        if (!s_switcher.content_gesture_active)
        {
            return;
        }
        lv_indev_t *indev = lv_event_get_indev(event);
        if (indev == NULL)
        {
            return;
        }
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        const int32_t dx = point.x - s_switcher.content_press_start.x;
        const int32_t dy = point.y - s_switcher.content_press_start.y;
        const int32_t abs_dx = dx < 0 ? -dx : dx;
        const int32_t abs_dy = dy < 0 ? -dy : dy;
        if (abs_dx > s_switcher.content_max_abs_dx)
        {
            s_switcher.content_max_abs_dx = abs_dx;
        }
        if (abs_dy > s_switcher.content_max_abs_dy)
        {
            s_switcher.content_max_abs_dy = abs_dy;
        }
        s_switcher.content_click_suppressed =
            s_switcher.content_max_abs_dx >=
            APP_MANAGER_TASK_SWITCHER_TAP_SLOP_PX ||
            s_switcher.content_max_abs_dy >=
            APP_MANAGER_TASK_SWITCHER_TAP_SLOP_PX;
    }
    else if (code == LV_EVENT_CLICKED)
    {
        /* A hide cancels the sequence; a late CLICKED from a press that
         * survived the hide must never dismiss the next session. */
        if (!s_switcher.content_gesture_active)
        {
            return;
        }
        s_switcher.content_gesture_active = false;
        if (s_switcher.content_click_suppressed)
        {
            s_switcher.content_click_suppressed = false;
            return;
        }
        if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_VISIBLE)
        {
            _switcher_hide();
        }
    }
    else if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL ||
             code == LV_EVENT_INDEV_RESET)
    {
        s_switcher.content_gesture_active = false;
        s_switcher.content_click_suppressed = false;
        s_switcher.content_max_abs_dx = 0;
        s_switcher.content_max_abs_dy = 0;
    }
}

static void _switcher_clear_clicked(lv_event_t *event)
{
    (void)event;
    if (s_switcher.state != APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        return;
    }
    s_switcher.state = APP_MANAGER_TASK_SWITCHER_MANAGING;
    /* Drop every preview-bearing card before the Home activation captures
     * the leaving foreground task. */
    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    if (s_switcher.empty_label != NULL)
    {
        lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_switcher.clear_button != NULL)
    {
        lv_obj_add_flag(s_switcher.clear_button, LV_OBJ_FLAG_HIDDEN);
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
    (void)result;
    (void)context;
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_MANAGING)
    {
        s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
        _switcher_set_busy(false);
        _switcher_rebuild();
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

    memset(&s_switcher, 0, sizeof(s_switcher));

    s_switcher.root = lv_obj_create(system_layer);
    if (s_switcher.root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_switcher.root);
    lv_obj_set_size(s_switcher.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_switcher.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_switcher.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_switcher.root, LV_OBJ_FLAG_HIDDEN |
                    LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_switcher.root, LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_CLICKABLE);

    s_switcher.content = lv_obj_create(s_switcher.root);
    if (s_switcher.content == NULL)
    {
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_switcher.content);
    lv_obj_set_size(s_switcher.content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_switcher.content, LV_FLEX_FLOW_ROW);
    /* Start-aligned row with half-viewport side padding: the first card is
     * centered and every page target (index * card pitch) is exactly
     * reachable, so programmatic paging never clamps mid-page. */
    lv_obj_set_flex_align(s_switcher.content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Native pointer scrolling is disabled for the whole gesture: card
     * swipes are classified from the cumulative displacement and paged
     * programmatically on release, so no scroll object can ever take the
     * gesture over or leave a residual offset. */
    lv_obj_set_scrollbar_mode(s_switcher.content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_switcher.content, LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_SCROLL_CHAIN |
                       LV_OBJ_FLAG_SCROLL_ELASTIC |
                       LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_pad_column(s_switcher.content,
                                APP_MANAGER_TASK_SWITCHER_CARD_GAP, 0);
    const int32_t edge_pad =
        (lv_display_get_horizontal_resolution(disp) -
         APP_MANAGER_TASK_SWITCHER_CARD_WIDTH) / 2;
    lv_obj_set_style_pad_left(s_switcher.content, edge_pad, 0);
    lv_obj_set_style_pad_right(s_switcher.content, edge_pad, 0);
    lv_obj_remove_flag(s_switcher.content, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_switcher.content, _switcher_content_event,
                        LV_EVENT_ALL, NULL);

    s_switcher.clear_button = lv_button_create(s_switcher.root);
    if (s_switcher.clear_button == NULL)
    {
        lv_obj_delete(s_switcher.content);
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_switcher.clear_button, 64, 48);
    lv_obj_align(s_switcher.clear_button, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_radius(s_switcher.clear_button, 6, 0);
    lv_obj_set_style_bg_color(s_switcher.clear_button, lv_color_hex(0x171E22),
                              0);
    lv_obj_set_style_bg_color(s_switcher.clear_button, lv_color_hex(0x1D262B),
                              LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_switcher.clear_button, 0, 0);
    lv_obj_add_flag(s_switcher.clear_button, LV_OBJ_FLAG_IGNORE_LAYOUT |
                    LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(s_switcher.clear_button, _switcher_clear_clicked,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_text = lv_label_create(s_switcher.clear_button);
    if (clear_text != NULL)
    {
        lv_label_set_text(clear_text, "清除");
        lv_obj_set_style_text_color(clear_text, lv_color_hex(0xF4F7F8), 0);
        lv_obj_set_style_text_font(clear_text,
                                   app_manager_get_font(APP_THEME_FONT_SMALL), 0);
        lv_obj_center(clear_text);
    }

    s_switcher.empty_label = lv_label_create(s_switcher.root);
    if (s_switcher.empty_label != NULL)
    {
        lv_label_set_text(s_switcher.empty_label, "暂无任务");
        lv_obj_set_width(s_switcher.empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_switcher.empty_label,
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_switcher.empty_label,
                                    lv_color_hex(0xAAB5BA), 0);
        lv_obj_set_style_text_font(s_switcher.empty_label,
                                   app_manager_get_font(APP_THEME_FONT_BODY),
                                   0);
        lv_obj_align(s_switcher.empty_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
    }

    s_switcher.state = APP_MANAGER_TASK_SWITCHER_HIDDEN;
    s_switcher.initialized = true;
    return ESP_OK;
}

esp_err_t app_manager_task_switcher_deinit(void)
{
    if (!s_switcher.initialized)
    {
        return ESP_OK;
    }
    if (s_switcher.root != NULL)
    {
        lv_anim_delete(s_switcher.root, NULL);
        lv_obj_delete(s_switcher.root);
    }
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
    (void)app_manager_lifecycle_capture_active_recents();
    app_manager_lifecycle_request_t request =
    {
        .tasks = s_switcher.tasks,
        .task_capacity = APP_MANAGER_MAX_RECENT_TASKS,
    };
    size_t count = 0U;
    esp_err_t query_result = app_manager_lifecycle_recent_tasks_call(&request);
    if (query_result == ESP_OK)
    {
        count = request.tasks_filled;
    }

    _switcher_destroy_cards();
    lv_obj_clean(s_switcher.content);
    s_switcher.card_count = 0U;
    if (s_switcher.empty_label != NULL)
    {
        if (count == 0U)
        {
            lv_obj_remove_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_switcher.clear_button != NULL)
    {
        if (count == 0U)
        {
            lv_obj_add_flag(s_switcher.clear_button, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_remove_flag(s_switcher.clear_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t index = 0U; index < count; index++)
    {
        _switcher_add_card(&s_switcher.tasks[index]);
    }

    lv_obj_update_layout(s_switcher.content);
    s_switcher.page_index = 0U;
    lv_obj_scroll_to_x(s_switcher.content, 0, LV_ANIM_OFF);
    s_switcher.page_scroll_x = lv_obj_get_scroll_x(s_switcher.content);

    s_switcher.state = APP_MANAGER_TASK_SWITCHER_VISIBLE;
    lv_obj_remove_flag(s_switcher.root, LV_OBJ_FLAG_HIDDEN);
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
    return s_switcher.state != APP_MANAGER_TASK_SWITCHER_HIDDEN;
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
