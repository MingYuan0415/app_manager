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

#include "esp_heap_caps.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Palette mirrors layers/apps/common/include/app_ui_theme.h; app_manager has
 * no dependency on the apps layer, so the tokens are duplicated here. */
#define TS_COLOR_BACKGROUND     0x090D0F
#define TS_COLOR_SURFACE        0x151B1F
#define TS_COLOR_SURFACE_HI     0x20282D
#define TS_COLOR_TEXT           0xF2F5F6
#define TS_COLOR_MUTED          0x93A0A6
#define TS_COLOR_RAIN           0x4FC4D8
#define TS_COLOR_WARNING        0xFF756C
#define TS_COLOR_ICON_BG        0x0E1315
#define TS_COLOR_ICON_FALLBACK  0x4A555A

#define TS_HEADER_HEIGHT  52
#define TS_RAM_HEIGHT     92
#define TS_CARD_HEIGHT    92
#define TS_CARD_GAP       10
#define TS_SIDE_PAD       16
#define TS_ICON_SIZE      56
#define TS_CLOSE_SIZE     40
#define TS_FADE_MS        180
#define TS_MEM_WARN_PCT   85

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
} app_manager_task_switcher_card_t;

typedef struct app_manager_task_switcher_state_data
{
    bool initialized;
    lv_obj_t *root;
    lv_obj_t *content;
    lv_obj_t *busy;
    lv_obj_t *clear_button;
    lv_obj_t *empty_label;
    lv_obj_t *ram_psram_row;
    lv_obj_t *ram_internal_bar;
    lv_obj_t *ram_internal_value;
    lv_obj_t *ram_psram_bar;
    lv_obj_t *ram_psram_value;
    app_manager_task_switcher_card_t cards[APP_MANAGER_MAX_RECENT_TASKS];
    size_t card_count;
    app_manager_task_switcher_state_t state;
    app_manager_recent_task_t tasks[APP_MANAGER_MAX_RECENT_TASKS];
} app_manager_task_switcher_state_data_t;

static app_manager_task_switcher_state_data_t s_switcher;

static void _switcher_rebuild(void);
static void _switcher_exit_complete(esp_err_t result, void *context);
static void _switcher_clear_complete(esp_err_t result, void *context);
static void _switcher_update_memory(void);

static void _switcher_destroy_cards(void)
{
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
    /* Remove stale task objects before the lifecycle changes residency. */
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
                           &request, _switcher_exit_complete, NULL);
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

/* Both actions delete the card tree (hide/close), so they must not run inside
 * the CLICKED callback of the very object being destroyed. Defer to the next
 * idle cycle via lv_async_call; the card struct lives in the static array and
 * app_id points at stable descriptor storage, so the captured pointers stay
 * valid until the deferred call runs. */
static void _switcher_card_activate_async(void *user_data)
{
    _switcher_card_clicked((app_manager_task_switcher_card_t *)user_data);
}

static void _switcher_close_task_async(void *user_data)
{
    _switcher_close_task((const char *)user_data);
}

/* The card is a plain clickable button inside a vertically scrollable list.
 * LVGL suppresses CLICKED once a drag turns into a scroll, so a tap selects
 * and a swipe scrolls without any custom gesture classification. */
static void _switcher_card_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    (void)lv_async_call(_switcher_card_activate_async,
                        lv_event_get_user_data(event));
}

static void _switcher_card_close_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    const app_manager_task_switcher_card_t *card =
        lv_event_get_user_data(event);
    (void)lv_async_call(_switcher_close_task_async, (void *)card->app_id);
}

static void _switcher_format_mem(size_t free_bytes, size_t total_bytes,
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
                       (unsigned)(free_bytes / 1048576U),
                       (unsigned)((free_bytes % 1048576U) * 10U / 1048576U),
                       (unsigned)(total_bytes / 1048576U),
                       (unsigned)((total_bytes % 1048576U) * 10U / 1048576U));
    }
    else
    {
        (void)snprintf(out, out_size, "%uk/%uk",
                       (unsigned)(free_bytes / 1024U),
                       (unsigned)(total_bytes / 1024U));
    }
}

static void _switcher_set_memory_row(lv_obj_t *bar, lv_obj_t *value,
                                     size_t free_bytes, size_t total_bytes,
                                     bool megabytes)
{
    char text[40];
    _switcher_format_mem(free_bytes, total_bytes, megabytes, text, sizeof(text));
    lv_label_set_text(value, text);
    const int used = total_bytes != 0U ?
                     (int)(((total_bytes - free_bytes) * 100U) / total_bytes) :
                     0;
    lv_bar_set_value(bar, used, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar,
                              lv_color_hex(used >= TS_MEM_WARN_PCT ?
                                           TS_COLOR_WARNING : TS_COLOR_RAIN),
                              LV_PART_INDICATOR);
}

static void _switcher_update_memory(void)
{
    const size_t internal_total = heap_caps_get_total_size(
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_free = heap_caps_get_free_size(
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_total = heap_caps_get_total_size(
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
            lv_obj_add_flag(s_switcher.ram_psram_row, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_remove_flag(s_switcher.ram_psram_row, LV_OBJ_FLAG_HIDDEN);
            _switcher_set_memory_row(s_switcher.ram_psram_bar,
                                     s_switcher.ram_psram_value,
                                     psram_free, psram_total, true);
        }
    }
}

static lv_obj_t *_switcher_make_passive_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return row;
}

static void _switcher_build_memory_row(lv_obj_t *parent, const char *title,
                                       lv_obj_t **bar_out,
                                       lv_obj_t **value_out,
                                       lv_obj_t **row_out)
{
    lv_obj_t *row = _switcher_make_passive_row(parent);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_width(name, 76);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, lv_color_hex(TS_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(name, app_manager_get_font(APP_THEME_FONT_BODY),
                               0);
    lv_label_set_text(name, title);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_width(bar, 0);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, 6);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TS_COLOR_SURFACE_HI),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TS_COLOR_RAIN),
                              LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, 108);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(value, lv_color_hex(TS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(value,
                               app_manager_get_font(APP_THEME_FONT_BODY), 0);
    lv_label_set_text(value, "--");

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
    lv_obj_set_size(button, LV_PCT(100), TS_CARD_HEIGHT);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(TS_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(TS_COLOR_SURFACE_HI),
                              LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_hor(button, 14, 0);
    lv_obj_set_style_pad_ver(button, 12, 0);
    lv_obj_set_style_pad_column(button, 12, 0);
    if (card->current)
    {
        lv_obj_set_style_border_width(button, 2, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(TS_COLOR_RAIN), 0);
        lv_obj_set_style_border_post(button, true, 0);
    }
    else
    {
        lv_obj_set_style_border_width(button, 0, 0);
    }
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                       LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    /* Click-only: releasing off the card after a drag that did not scroll must
     * not select it. */
    lv_obj_remove_flag(button, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(button, _switcher_card_event, LV_EVENT_CLICKED, card);

    lv_obj_t *icon = lv_obj_create(button);
    if (icon != NULL)
    {
        lv_obj_set_size(icon, TS_ICON_SIZE, TS_ICON_SIZE);
        lv_obj_set_style_radius(icon, 10, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(TS_COLOR_ICON_BG), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                           LV_OBJ_FLAG_CLICK_FOCUSABLE |
                           LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                           LV_OBJ_FLAG_SCROLL_ELASTIC |
                           LV_OBJ_FLAG_SCROLL_MOMENTUM);
        const lv_image_dsc_t *icon_image = NULL;
        if (task->icon_id != 0U &&
                app_manager_get_image(task->icon_id, &icon_image) == ESP_OK &&
                icon_image != NULL)
        {
            lv_obj_t *image = lv_image_create(icon);
            if (image != NULL)
            {
                lv_image_set_src(image, icon_image);
                lv_obj_set_size(image, 44, 44);
                lv_obj_center(image);
                lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE |
                                   LV_OBJ_FLAG_CLICKABLE |
                                   LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                   LV_OBJ_FLAG_GESTURE_BUBBLE |
                                   LV_OBJ_FLAG_SCROLL_CHAIN |
                                   LV_OBJ_FLAG_SCROLL_ELASTIC |
                                   LV_OBJ_FLAG_SCROLL_MOMENTUM);
            }
        }
        else
        {
            lv_obj_t *symbol = lv_label_create(icon);
            if (symbol != NULL)
            {
                lv_obj_set_style_text_font(symbol, LV_FONT_DEFAULT, 0);
                lv_obj_set_style_text_color(symbol,
                                            lv_color_hex(TS_COLOR_ICON_FALLBACK),
                                            0);
                lv_label_set_text(symbol, LV_SYMBOL_IMAGE);
                lv_obj_center(symbol);
            }
        }
    }

    lv_obj_t *text = lv_obj_create(button);
    if (text != NULL)
    {
        lv_obj_remove_style_all(text);
        lv_obj_set_width(text, 0);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, 3, 0);
        lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                           LV_OBJ_FLAG_CLICK_FOCUSABLE);

        lv_obj_t *name = lv_label_create(text);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(name, lv_color_hex(TS_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name,
                                   app_manager_get_font(APP_THEME_FONT_SMALL), 0);
        lv_label_set_text(name, task->display_name != NULL ?
                          task->display_name : task->app_id);

        lv_obj_t *status = lv_label_create(text);
        lv_obj_set_width(status, LV_PCT(100));
        lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(status,
                                    lv_color_hex(card->current ? TS_COLOR_RAIN :
                                            TS_COLOR_MUTED), 0);
        lv_obj_set_style_text_font(status,
                                   app_manager_get_font(APP_THEME_FONT_BODY), 0);
        lv_label_set_text(status, card->current ? "当前" : "点按切换");
    }

    if (!card->current)
    {
        lv_obj_t *close = lv_button_create(button);
        if (close != NULL)
        {
            lv_obj_set_size(close, TS_CLOSE_SIZE, TS_CLOSE_SIZE);
            lv_obj_set_style_radius(close, 8, 0);
            lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(close, lv_color_hex(TS_COLOR_BACKGROUND),
                                      LV_STATE_PRESSED);
            lv_obj_set_style_border_width(close, 0, 0);
            lv_obj_set_style_shadow_width(close, 0, 0);
            lv_obj_set_style_pad_all(close, 0, 0);
            lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE |
                               LV_OBJ_FLAG_SCROLL_CHAIN |
                               LV_OBJ_FLAG_SCROLL_ELASTIC |
                               LV_OBJ_FLAG_SCROLL_MOMENTUM);
            lv_obj_remove_flag(close, LV_OBJ_FLAG_PRESS_LOCK);
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
            }
        }
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
    /* Rebuild from the latest lifecycle snapshot. */
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
    lv_obj_update_layout(s_switcher.content);
    lv_obj_scroll_to_y(s_switcher.content, 0, LV_ANIM_OFF);
    _switcher_update_memory();
}

static void _switcher_content_event(lv_event_t *event)
{
    /* A tap on the empty backdrop dismisses the switcher. LVGL withholds
     * CLICKED while a drag scrolls the list, so scrolling never hides it. */
    if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (s_switcher.state == APP_MANAGER_TASK_SWITCHER_VISIBLE)
    {
        _switcher_hide();
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
    /* Remove stale task objects before activating Home. */
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

    const int32_t width = lv_display_get_horizontal_resolution(disp);
    const int32_t height = lv_display_get_vertical_resolution(disp);
    const int32_t list_height = height - TS_HEADER_HEIGHT - TS_RAM_HEIGHT;

    s_switcher.root = lv_obj_create(system_layer);
    if (s_switcher.root == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_switcher.root);
    lv_obj_set_size(s_switcher.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_switcher.root, lv_color_hex(TS_COLOR_BACKGROUND),
                              0);
    lv_obj_set_style_bg_opa(s_switcher.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_switcher.root, LV_OBJ_FLAG_HIDDEN |
                    LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_switcher.root, LV_OBJ_FLAG_SCROLLABLE |
                       LV_OBJ_FLAG_CLICKABLE);

    /* Header: title on the left, clear-all on the right. */
    lv_obj_t *header = lv_obj_create(s_switcher.root);
    if (header == NULL)
    {
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, width, TS_HEADER_HEIGHT);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, TS_SIDE_PAD, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_color(title, lv_color_hex(TS_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title,
                               app_manager_get_font(APP_THEME_FONT_HEAD), 0);
    lv_label_set_text(title, "最近任务");

    s_switcher.clear_button = lv_button_create(header);
    if (s_switcher.clear_button == NULL)
    {
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_switcher.clear_button, 72, 40);
    lv_obj_set_style_radius(s_switcher.clear_button, 8, 0);
    lv_obj_set_style_bg_color(s_switcher.clear_button,
                              lv_color_hex(TS_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(s_switcher.clear_button,
                              lv_color_hex(TS_COLOR_SURFACE_HI),
                              LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_switcher.clear_button, 0, 0);
    lv_obj_add_event_cb(s_switcher.clear_button, _switcher_clear_clicked,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_text = lv_label_create(s_switcher.clear_button);
    if (clear_text != NULL)
    {
        lv_obj_set_style_text_color(clear_text, lv_color_hex(TS_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(clear_text,
                                   app_manager_get_font(APP_THEME_FONT_BODY), 0);
        lv_label_set_text(clear_text, "清除");
        lv_obj_center(clear_text);
    }

    /* Scrollable vertical list of retained tasks. */
    s_switcher.content = lv_obj_create(s_switcher.root);
    if (s_switcher.content == NULL)
    {
        lv_obj_delete(s_switcher.root);
        memset(&s_switcher, 0, sizeof(s_switcher));
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_switcher.content);
    lv_obj_set_size(s_switcher.content, width, list_height);
    lv_obj_align_to(s_switcher.content, header, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
    lv_obj_set_flex_flow(s_switcher.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_switcher.content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_switcher.content, TS_SIDE_PAD, 0);
    lv_obj_set_style_pad_ver(s_switcher.content, 8, 0);
    lv_obj_set_style_pad_row(s_switcher.content, TS_CARD_GAP, 0);
    lv_obj_set_scrollbar_mode(s_switcher.content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_switcher.content, LV_DIR_VER);
    lv_obj_remove_flag(s_switcher.content, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_switcher.content, _switcher_content_event,
                        LV_EVENT_CLICKED, NULL);

    s_switcher.empty_label = lv_label_create(s_switcher.root);
    if (s_switcher.empty_label != NULL)
    {
        lv_obj_set_width(s_switcher.empty_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_switcher.empty_label, LV_TEXT_ALIGN_CENTER,
                                    0);
        lv_obj_set_style_text_color(s_switcher.empty_label,
                                    lv_color_hex(TS_COLOR_MUTED), 0);
        lv_obj_set_style_text_font(s_switcher.empty_label,
                                   app_manager_get_font(APP_THEME_FONT_BODY), 0);
        lv_label_set_text(s_switcher.empty_label, "暂无任务");
        /* Child of the root, not the list: lv_obj_clean(content) must never
         * free it. Center it over the list viewport. */
        lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_IGNORE_LAYOUT |
                        LV_OBJ_FLAG_FLOATING);
        lv_obj_align_to(s_switcher.empty_label, s_switcher.content,
                        LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(s_switcher.empty_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* Footer: device memory usage (internal RAM and PSRAM). */
    lv_obj_t *ram = lv_obj_create(s_switcher.root);
    if (ram != NULL)
    {
        lv_obj_set_size(ram, width, TS_RAM_HEIGHT);
        lv_obj_align(ram, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(ram, lv_color_hex(TS_COLOR_SURFACE), 0);
        lv_obj_set_style_bg_opa(ram, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ram, 0, 0);
        lv_obj_set_style_radius(ram, 0, 0);
        lv_obj_set_style_pad_all(ram, 14, 0);
        lv_obj_set_style_pad_row(ram, 10, 0);
        lv_obj_set_flex_flow(ram, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(ram, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(ram, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        _switcher_build_memory_row(ram, "内部内存",
                                   &s_switcher.ram_internal_bar,
                                   &s_switcher.ram_internal_value, NULL);
        _switcher_build_memory_row(ram, "PSRAM",
                                   &s_switcher.ram_psram_bar,
                                   &s_switcher.ram_psram_value,
                                   &s_switcher.ram_psram_row);
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
    lv_obj_scroll_to_y(s_switcher.content, 0, LV_ANIM_OFF);
    _switcher_update_memory();

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
