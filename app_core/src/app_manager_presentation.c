#include "app_manager_presentation.h"
#include "app_manager_display_diagnostics_priv.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION) && \
    CONFIG_APP_MANAGER_PRESENTATION_SNAPSHOT_ANIMATION
    #define APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED 1
    #include "esp_heap_caps.h"
    #ifndef HOST_TEST
        #include "esp_log.h"
    #endif
#else
    #define APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED 0
#endif

#define APP_MANAGER_PRESENTATION_TIMER_MS 1U

#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    #define APP_MANAGER_PRESENTATION_SNAPSHOT_PROGRESS_MAX 1000
    #define APP_MANAGER_PRESENTATION_LOG_TAG "app_presentation"
#endif

typedef struct app_manager_presentation_state
{
    bool initialized;
    bool deinitializing;
    bool running;
    bool completion_ready;
    bool diagnostics_started;
    uint32_t generation;
    uint32_t scheduled_generation;
    lv_display_t *display;
    lv_obj_t *neutral_screen;
    lv_obj_t *input_blocker;
    lv_obj_t *from_screen;
    lv_obj_t *to_screen;
    lv_event_dsc_t *loaded_event;
    lv_timer_t *completion_timer;
    app_manager_presentation_completion_cb_t completion;
    void *completion_context;
#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    bool snapshot_available;
    bool snapshot_running;
    uint32_t snapshot_width;
    uint32_t snapshot_height;
    void *snapshot_data[2];
    lv_draw_buf_t snapshot_draw_buffers[2];
    lv_obj_t *snapshot_overlay;
    lv_obj_t *snapshot_from_image;
    lv_obj_t *snapshot_to_image;
    app_manager_transition_effect_t snapshot_effect;
#endif
} app_manager_presentation_state_t;

static app_manager_presentation_state_t s_presentation;

static void _presentation_completion_timer_cb(lv_timer_t *timer);
static void _presentation_complete(uint32_t generation);
static void _presentation_schedule_completion(uint32_t generation);
static esp_err_t _presentation_recover_animation_start(lv_obj_t *from_screen,
        lv_obj_t *to_screen);

static bool _presentation_screen_is_valid(const lv_obj_t *screen)
{
    return screen != NULL && lv_obj_is_valid(screen) &&
           lv_obj_get_parent(screen) == NULL &&
           lv_obj_get_display(screen) == s_presentation.display;
}

static void _presentation_set_blocked(bool blocked)
{
    if (s_presentation.input_blocker == NULL)
    {
        return;
    }

    if (blocked)
    {
        lv_obj_move_to_index(s_presentation.input_blocker, -1);
        lv_obj_remove_flag(s_presentation.input_blocker, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_presentation.input_blocker, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _presentation_remove_loaded_event(void)
{
    if (s_presentation.loaded_event == NULL)
    {
        return;
    }

    if (_presentation_screen_is_valid(s_presentation.to_screen))
    {
        (void)lv_obj_remove_event_dsc(s_presentation.to_screen,
                                      s_presentation.loaded_event);
    }
    s_presentation.loaded_event = NULL;
}

#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
static void _presentation_snapshot_delete_overlay(void)
{
    if (s_presentation.snapshot_overlay != NULL &&
            lv_obj_is_valid(s_presentation.snapshot_overlay))
    {
        lv_obj_delete(s_presentation.snapshot_overlay);
    }
    s_presentation.snapshot_overlay = NULL;
    s_presentation.snapshot_from_image = NULL;
    s_presentation.snapshot_to_image = NULL;
}

static void _presentation_snapshot_abort(void)
{
    lv_anim_delete(&s_presentation, NULL);
    _presentation_snapshot_delete_overlay();
    s_presentation.snapshot_running = false;
    s_presentation.snapshot_effect = APP_MANAGER_TRANSITION_NONE;
}

static void _presentation_snapshot_release_buffers(void)
{
    for (size_t index = 0U;
            index < sizeof(s_presentation.snapshot_data) /
            sizeof(s_presentation.snapshot_data[0]); index++)
    {
        heap_caps_free(s_presentation.snapshot_data[index]);
        s_presentation.snapshot_data[index] = NULL;
    }
    memset(s_presentation.snapshot_draw_buffers, 0,
           sizeof(s_presentation.snapshot_draw_buffers));
    s_presentation.snapshot_available = false;
    s_presentation.snapshot_width = 0U;
    s_presentation.snapshot_height = 0U;
}

static void _presentation_snapshot_disable(const char *reason)
{
    _presentation_snapshot_release_buffers();
#ifndef HOST_TEST
    ESP_LOGW(APP_MANAGER_PRESENTATION_LOG_TAG,
             "snapshot animation disabled: %s", reason);
#else
    (void)reason;
#endif
}

static void _presentation_snapshot_init_buffers(void)
{
    const int32_t horizontal_resolution =
        lv_display_get_horizontal_resolution(s_presentation.display);
    const int32_t vertical_resolution =
        lv_display_get_vertical_resolution(s_presentation.display);
    uint32_t stride = 0U;
    uint32_t data_size = 0U;

    if (horizontal_resolution <= 0 || vertical_resolution <= 0)
    {
        _presentation_snapshot_disable("invalid display resolution");
        return;
    }

    s_presentation.snapshot_width = (uint32_t)horizontal_resolution;
    s_presentation.snapshot_height = (uint32_t)vertical_resolution;
    stride = lv_draw_buf_width_to_stride(s_presentation.snapshot_width,
                                         LV_COLOR_FORMAT_RGB565);
    if (stride == 0U ||
            s_presentation.snapshot_height > UINT32_MAX / stride)
    {
        _presentation_snapshot_disable("invalid draw buffer size");
        return;
    }
    data_size = stride * s_presentation.snapshot_height;

    for (size_t index = 0U;
            index < sizeof(s_presentation.snapshot_data) /
            sizeof(s_presentation.snapshot_data[0]); index++)
    {
        s_presentation.snapshot_data[index] = heap_caps_aligned_calloc(
                LV_DRAW_BUF_ALIGN, 1U, data_size,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_presentation.snapshot_data[index] == NULL)
        {
            _presentation_snapshot_disable("PSRAM allocation failed");
            return;
        }
        if (lv_draw_buf_init(&s_presentation.snapshot_draw_buffers[index],
                             s_presentation.snapshot_width,
                             s_presentation.snapshot_height,
                             LV_COLOR_FORMAT_RGB565, stride,
                             s_presentation.snapshot_data[index],
                             data_size) != LV_RESULT_OK)
        {
            _presentation_snapshot_disable("draw buffer initialization failed");
            return;
        }
    }

    s_presentation.snapshot_available = true;
}

static bool _presentation_snapshot_tree_has_visible_overflow(
    const lv_obj_t *object)
{
    if (object == NULL || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
    {
        return false;
    }
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_OVERFLOW_VISIBLE))
    {
        return true;
    }

    const uint32_t child_count = lv_obj_get_child_count(object);
    for (uint32_t index = 0U; index < child_count; ++index)
    {
        if (_presentation_snapshot_tree_has_visible_overflow(
                    lv_obj_get_child(object, (int32_t)index)))
        {
            return true;
        }
    }
    return false;
}

static bool _presentation_snapshot_screen_is_eligible(lv_obj_t *screen)
{
    lv_obj_update_layout(screen);
    if (_presentation_snapshot_tree_has_visible_overflow(screen))
    {
        return false;
    }

    const uint32_t child_count = lv_obj_get_child_count(screen);
    if (child_count == 0U)
    {
        return true;
    }

    for (uint32_t index = 0U; index < child_count; ++index)
    {
        const lv_obj_t *root = lv_obj_get_child(screen, (int32_t)index);
        if (root == NULL || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN) ||
                lv_obj_get_style_bg_opa(root, LV_PART_MAIN) != LV_OPA_COVER ||
                lv_obj_get_style_opa(root, LV_PART_MAIN) != LV_OPA_COVER)
        {
            continue;
        }

        const int32_t x = lv_obj_get_x(root);
        const int32_t y = lv_obj_get_y(root);
        if (x <= 0 && y <= 0 &&
                (int64_t)x + lv_obj_get_width(root) >=
                s_presentation.snapshot_width &&
                (int64_t)y + lv_obj_get_height(root) >=
                s_presentation.snapshot_height)
        {
            return true;
        }
    }
    return false;
}

static int32_t _presentation_snapshot_lerp(int32_t start, int32_t end,
        int32_t progress)
{
    return start + ((end - start) * progress) /
           APP_MANAGER_PRESENTATION_SNAPSHOT_PROGRESS_MAX;
}

static void _presentation_snapshot_progress_cb(void *variable,
        int32_t progress)
{
    app_manager_presentation_state_t *presentation = variable;
    int32_t from_x = 0;
    int32_t from_y = 0;
    int32_t to_x = 0;
    int32_t to_y = 0;
    lv_opa_t to_opa = LV_OPA_COVER;
    const int32_t width = (int32_t)s_presentation.snapshot_width;
    const int32_t height = (int32_t)s_presentation.snapshot_height;

    if (presentation != &s_presentation || !s_presentation.running ||
            !s_presentation.snapshot_running ||
            s_presentation.snapshot_from_image == NULL ||
            s_presentation.snapshot_to_image == NULL ||
            !lv_obj_is_valid(s_presentation.snapshot_from_image) ||
            !lv_obj_is_valid(s_presentation.snapshot_to_image))
    {
        return;
    }

    if (progress < 0)
    {
        progress = 0;
    }
    else if (progress > APP_MANAGER_PRESENTATION_SNAPSHOT_PROGRESS_MAX)
    {
        progress = APP_MANAGER_PRESENTATION_SNAPSHOT_PROGRESS_MAX;
    }

    switch (s_presentation.snapshot_effect)
    {
    case APP_MANAGER_TRANSITION_FADE:
        to_opa = (lv_opa_t)_presentation_snapshot_lerp(
                     LV_OPA_TRANSP, LV_OPA_COVER, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_LEFT:
        to_x = _presentation_snapshot_lerp(width, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_RIGHT:
        to_x = _presentation_snapshot_lerp(-width, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_UP:
        to_y = _presentation_snapshot_lerp(height, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_COVER_DOWN:
        to_y = _presentation_snapshot_lerp(-height, 0, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_LEFT:
        to_x = _presentation_snapshot_lerp(width, 0, progress);
        from_x = _presentation_snapshot_lerp(0, -width, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_RIGHT:
        to_x = _presentation_snapshot_lerp(-width, 0, progress);
        from_x = _presentation_snapshot_lerp(0, width, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_UP:
        to_y = _presentation_snapshot_lerp(height, 0, progress);
        from_y = _presentation_snapshot_lerp(0, -height, progress);
        break;
    case APP_MANAGER_TRANSITION_PUSH_DOWN:
        to_y = _presentation_snapshot_lerp(-height, 0, progress);
        from_y = _presentation_snapshot_lerp(0, height, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_LEFT:
        from_x = _presentation_snapshot_lerp(0, -width, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_RIGHT:
        from_x = _presentation_snapshot_lerp(0, width, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_UP:
        from_y = _presentation_snapshot_lerp(0, -height, progress);
        break;
    case APP_MANAGER_TRANSITION_REVEAL_DOWN:
        from_y = _presentation_snapshot_lerp(0, height, progress);
        break;
    case APP_MANAGER_TRANSITION_NONE:
    case APP_MANAGER_TRANSITION_DEFAULT:
    case APP_MANAGER_TRANSITION_END:
    default:
        break;
    }

    lv_obj_set_pos(s_presentation.snapshot_from_image, from_x, from_y);
    lv_obj_set_pos(s_presentation.snapshot_to_image, to_x, to_y);
    lv_obj_set_style_opa(s_presentation.snapshot_to_image, to_opa, 0);
    if (s_presentation.snapshot_overlay != NULL &&
            lv_obj_is_valid(s_presentation.snapshot_overlay))
    {
        lv_obj_move_to_index(s_presentation.snapshot_overlay, -1);
    }
}

static void _presentation_snapshot_animation_completed(lv_anim_t *animation)
{
    const uint32_t generation = s_presentation.generation;

    if (animation == NULL || animation->var != &s_presentation ||
            !s_presentation.running || !s_presentation.snapshot_running)
    {
        return;
    }

    _presentation_snapshot_progress_cb(&s_presentation,
                                       APP_MANAGER_PRESENTATION_SNAPSHOT_PROGRESS_MAX);
    _presentation_snapshot_delete_overlay();
    s_presentation.snapshot_running = false;
    s_presentation.snapshot_effect = APP_MANAGER_TRANSITION_NONE;
    if (_presentation_screen_is_valid(s_presentation.to_screen))
    {
        lv_obj_invalidate(s_presentation.to_screen);
    }
    _presentation_schedule_completion(generation);
}

static esp_err_t _presentation_start_snapshot(
    app_manager_transition_effect_t effect, uint32_t duration_ms,
    uint32_t generation)
{
    lv_obj_t *overlay = NULL;
    lv_obj_t *from_image = NULL;
    lv_obj_t *to_image = NULL;
    lv_anim_t animation;

    if (!s_presentation.snapshot_available)
    {
        app_manager_display_diagnostics_snapshot_prepare_finished(true);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!_presentation_snapshot_screen_is_eligible(
                s_presentation.from_screen) ||
            !_presentation_snapshot_screen_is_eligible(
                s_presentation.to_screen))
    {
        goto fallback;
    }
    if (lv_snapshot_take_to_draw_buf(s_presentation.from_screen,
                                     LV_COLOR_FORMAT_RGB565,
                                     &s_presentation.snapshot_draw_buffers[0]) !=
            LV_RESULT_OK ||
            lv_snapshot_take_to_draw_buf(s_presentation.to_screen,
                                         LV_COLOR_FORMAT_RGB565,
                                         &s_presentation.snapshot_draw_buffers[1]) !=
            LV_RESULT_OK)
    {
        goto fallback;
    }

    overlay = lv_obj_create(s_presentation.to_screen);
    if (overlay == NULL)
    {
        goto fallback;
    }
    s_presentation.snapshot_overlay = overlay;
    lv_obj_remove_style_all(overlay);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, (int32_t)s_presentation.snapshot_width,
                    (int32_t)s_presentation.snapshot_height);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    from_image = lv_image_create(overlay);
    s_presentation.snapshot_from_image = from_image;
    to_image = lv_image_create(overlay);
    s_presentation.snapshot_to_image = to_image;
    if (from_image == NULL || to_image == NULL)
    {
        goto fallback;
    }
    lv_image_set_src(from_image, &s_presentation.snapshot_draw_buffers[0]);
    lv_image_set_src(to_image, &s_presentation.snapshot_draw_buffers[1]);
    lv_obj_set_size(from_image, (int32_t)s_presentation.snapshot_width,
                    (int32_t)s_presentation.snapshot_height);
    lv_obj_set_size(to_image, (int32_t)s_presentation.snapshot_width,
                    (int32_t)s_presentation.snapshot_height);
    lv_obj_remove_flag(from_image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(to_image, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE |
                       LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (effect >= APP_MANAGER_TRANSITION_REVEAL_LEFT &&
            effect <= APP_MANAGER_TRANSITION_REVEAL_DOWN)
    {
        lv_obj_move_to_index(from_image, -1);
    }

    s_presentation.snapshot_effect = effect;
    s_presentation.snapshot_running = true;
    _presentation_snapshot_progress_cb(&s_presentation, 0);

    lv_anim_init(&animation);
    lv_anim_set_var(&animation, &s_presentation);
    lv_anim_set_exec_cb(&animation, _presentation_snapshot_progress_cb);
    lv_anim_set_values(&animation, 0,
                       APP_MANAGER_PRESENTATION_SNAPSHOT_PROGRESS_MAX);
    lv_anim_set_duration(&animation, duration_ms);
    lv_anim_set_completed_cb(&animation,
                             _presentation_snapshot_animation_completed);
    lv_anim_set_early_apply(&animation, true);
    if (lv_anim_start(&animation) == NULL)
    {
        goto fallback;
    }

    lv_screen_load(s_presentation.to_screen);
    if (!_presentation_screen_is_valid(s_presentation.from_screen) ||
            !_presentation_screen_is_valid(s_presentation.to_screen))
    {
        _presentation_snapshot_abort();
        app_manager_display_diagnostics_snapshot_prepare_finished(true);
        return ESP_ERR_INVALID_STATE;
    }
    if (lv_display_get_screen_active(s_presentation.display) !=
            s_presentation.to_screen)
    {
        goto fallback;
    }

    _presentation_snapshot_progress_cb(&s_presentation, 0);
    app_manager_display_diagnostics_snapshot_prepare_finished(false);
    s_presentation.diagnostics_started = true;
    app_manager_display_diagnostics_transition_started(effect);
    return ESP_OK;

fallback:
    lv_anim_delete(&s_presentation, _presentation_snapshot_progress_cb);
    _presentation_snapshot_delete_overlay();
    s_presentation.snapshot_running = false;
    s_presentation.snapshot_effect = APP_MANAGER_TRANSITION_NONE;
    app_manager_display_diagnostics_snapshot_prepare_finished(true);
    if (lv_display_get_screen_active(s_presentation.display) ==
            s_presentation.from_screen)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t result = _presentation_recover_animation_start(
                                 s_presentation.from_screen,
                                 s_presentation.to_screen);
    if (result == ESP_OK)
    {
        _presentation_complete(generation);
    }
    return result;
}

esp_err_t app_manager_presentation_capture_source(
    lv_obj_t *screen, const lv_draw_buf_t **snapshot)
{
    if (screen == NULL || snapshot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = NULL;
    if (!s_presentation.initialized || s_presentation.deinitializing ||
            !s_presentation.snapshot_available ||
            !_presentation_screen_is_valid(screen) ||
            !_presentation_snapshot_screen_is_eligible(screen))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565,
                                     &s_presentation.snapshot_draw_buffers[0]) !=
            LV_RESULT_OK)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *snapshot = &s_presentation.snapshot_draw_buffers[0];
    return ESP_OK;
}
#endif

#if !APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
esp_err_t app_manager_presentation_capture_source(
    lv_obj_t *screen, const lv_draw_buf_t **snapshot)
{
    (void)screen;
    if (snapshot != NULL)
    {
        *snapshot = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static void _presentation_schedule_completion(uint32_t generation)
{
    if (!s_presentation.running ||
            generation != s_presentation.generation)
    {
        return;
    }

    s_presentation.completion_ready = true;
    s_presentation.scheduled_generation = generation;
    if (s_presentation.completion_timer == NULL)
    {
        _presentation_complete(generation);
        return;
    }
    lv_timer_ready(s_presentation.completion_timer);
    lv_timer_resume(s_presentation.completion_timer);
}

static void _presentation_reset_transition(void)
{
    if (s_presentation.diagnostics_started)
    {
        s_presentation.diagnostics_started = false;
        app_manager_display_diagnostics_transition_cancelled();
    }
    _presentation_remove_loaded_event();
#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    _presentation_snapshot_abort();
#endif
    if (s_presentation.completion_timer != NULL)
    {
        lv_timer_pause(s_presentation.completion_timer);
    }
    _presentation_set_blocked(false);
    s_presentation.running = false;
    s_presentation.completion_ready = false;
    s_presentation.scheduled_generation = 0U;
    s_presentation.from_screen = NULL;
    s_presentation.to_screen = NULL;
    s_presentation.completion = NULL;
    s_presentation.completion_context = NULL;
}

static void _presentation_complete(uint32_t generation)
{
    app_manager_presentation_completion_cb_t completion = NULL;
    void *context = NULL;

    if (!s_presentation.running ||
            generation != s_presentation.generation)
    {
        return;
    }

    completion = s_presentation.completion;
    context = s_presentation.completion_context;
    if (s_presentation.diagnostics_started)
    {
        s_presentation.diagnostics_started = false;
        app_manager_display_diagnostics_transition_completed();
    }
    _presentation_reset_transition();
    if (completion != NULL)
    {
        completion(context);
    }
}

static void _presentation_loaded_event_cb(lv_event_t *event)
{
    const uint32_t generation =
        (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target_obj(event);

    if (!s_presentation.initialized || !s_presentation.running ||
            generation != s_presentation.generation ||
            target != s_presentation.to_screen)
    {
        return;
    }

    _presentation_remove_loaded_event();
#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    if (s_presentation.snapshot_running)
    {
        return;
    }
#endif
    _presentation_schedule_completion(generation);
}

static void _presentation_completion_timer_cb(lv_timer_t *timer)
{
    const uint32_t generation = s_presentation.scheduled_generation;

    lv_timer_pause(timer);
    if (!s_presentation.completion_ready ||
            generation != s_presentation.generation)
    {
        return;
    }
    _presentation_complete(generation);
}

static bool _presentation_map_effect(
    app_manager_transition_effect_t effect,
    lv_screen_load_anim_t *animation)
{
    bool mapped = true;

    switch (effect)
    {
    case APP_MANAGER_TRANSITION_NONE:
        *animation = LV_SCREEN_LOAD_ANIM_NONE;
        break;
    case APP_MANAGER_TRANSITION_FADE:
        *animation = LV_SCREEN_LOAD_ANIM_FADE_IN;
        break;
    case APP_MANAGER_TRANSITION_COVER_LEFT:
        *animation = LV_SCREEN_LOAD_ANIM_OVER_LEFT;
        break;
    case APP_MANAGER_TRANSITION_COVER_RIGHT:
        *animation = LV_SCREEN_LOAD_ANIM_OVER_RIGHT;
        break;
    case APP_MANAGER_TRANSITION_COVER_UP:
        *animation = LV_SCREEN_LOAD_ANIM_OVER_TOP;
        break;
    case APP_MANAGER_TRANSITION_COVER_DOWN:
        *animation = LV_SCREEN_LOAD_ANIM_OVER_BOTTOM;
        break;
    case APP_MANAGER_TRANSITION_PUSH_LEFT:
        *animation = LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
        break;
    case APP_MANAGER_TRANSITION_PUSH_RIGHT:
        *animation = LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
        break;
    case APP_MANAGER_TRANSITION_PUSH_UP:
        *animation = LV_SCREEN_LOAD_ANIM_MOVE_TOP;
        break;
    case APP_MANAGER_TRANSITION_PUSH_DOWN:
        *animation = LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM;
        break;
    case APP_MANAGER_TRANSITION_REVEAL_LEFT:
        *animation = LV_SCREEN_LOAD_ANIM_OUT_LEFT;
        break;
    case APP_MANAGER_TRANSITION_REVEAL_RIGHT:
        *animation = LV_SCREEN_LOAD_ANIM_OUT_RIGHT;
        break;
    case APP_MANAGER_TRANSITION_REVEAL_UP:
        *animation = LV_SCREEN_LOAD_ANIM_OUT_TOP;
        break;
    case APP_MANAGER_TRANSITION_REVEAL_DOWN:
        *animation = LV_SCREEN_LOAD_ANIM_OUT_BOTTOM;
        break;
    case APP_MANAGER_TRANSITION_DEFAULT:
    case APP_MANAGER_TRANSITION_END:
    default:
        mapped = false;
        break;
    }

    return mapped;
}

static esp_err_t _presentation_recover_animation_start(lv_obj_t *from_screen,
        lv_obj_t *to_screen)
{
    esp_err_t result = ESP_ERR_NO_MEM;
    lv_obj_t *scratch_screen = lv_obj_create(NULL);

    if (scratch_screen == NULL)
    {
        goto exit;
    }

    lv_screen_load(scratch_screen);
    if (lv_display_get_screen_active(s_presentation.display) != scratch_screen)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    lv_screen_load(to_screen);
    if (lv_display_get_screen_active(s_presentation.display) != to_screen)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    result = ESP_OK;

exit:
    if (result != ESP_OK && scratch_screen != NULL &&
            lv_obj_is_valid(scratch_screen) &&
            lv_display_get_screen_active(s_presentation.display) ==
            scratch_screen && _presentation_screen_is_valid(from_screen))
    {
        lv_screen_load(from_screen);
    }
    if (scratch_screen != NULL && lv_obj_is_valid(scratch_screen) &&
            lv_display_get_screen_active(s_presentation.display) !=
            scratch_screen)
    {
        lv_obj_delete(scratch_screen);
    }
    return result;
}

static esp_err_t _presentation_start_native(
    lv_screen_load_anim_t animation, app_manager_transition_effect_t effect,
    uint32_t duration_ms, uint32_t generation)
{
    s_presentation.loaded_event = lv_obj_add_event_cb(
                                      s_presentation.to_screen,
                                      _presentation_loaded_event_cb,
                                      LV_EVENT_SCREEN_LOADED,
                                      (void *)(uintptr_t)generation);
    if (s_presentation.loaded_event == NULL)
    {
        _presentation_reset_transition();
        return ESP_ERR_NO_MEM;
    }

    lv_screen_load_anim(s_presentation.to_screen, animation, duration_ms,
                        0U, false);
    if (lv_anim_get(s_presentation.to_screen, NULL) != NULL)
    {
        s_presentation.diagnostics_started = true;
        app_manager_display_diagnostics_transition_started(effect);
        return ESP_OK;
    }

    _presentation_remove_loaded_event();
    lv_anim_delete(s_presentation.from_screen, NULL);
    const esp_err_t result =
        lv_display_get_screen_active(s_presentation.display) ==
        s_presentation.to_screen ?
        ESP_OK : _presentation_recover_animation_start(
            s_presentation.from_screen, s_presentation.to_screen);
    if (result == ESP_OK)
    {
        _presentation_complete(generation);
    }
    else
    {
        _presentation_reset_transition();
    }
    return result;
}

esp_err_t app_manager_presentation_init(void)
{
    esp_err_t result = ESP_OK;
    lv_obj_t *system_layer = NULL;

    if (s_presentation.initialized)
    {
        return ESP_OK;
    }

    memset(&s_presentation, 0, sizeof(s_presentation));
    s_presentation.display = lv_display_get_default();
    if (s_presentation.display == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }

#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    _presentation_snapshot_init_buffers();
#endif

    s_presentation.neutral_screen = lv_obj_create(NULL);
    if (s_presentation.neutral_screen == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_set_style_bg_color(s_presentation.neutral_screen,
                              lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_presentation.neutral_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_presentation.neutral_screen,
                       LV_OBJ_FLAG_SCROLLABLE);

    system_layer = lv_display_get_layer_sys(s_presentation.display);
    if (system_layer == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    s_presentation.input_blocker = lv_obj_create(system_layer);
    if (s_presentation.input_blocker == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_remove_style_all(s_presentation.input_blocker);
    lv_obj_set_style_bg_opa(s_presentation.input_blocker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_presentation.input_blocker, 0, 0);
    lv_obj_set_size(s_presentation.input_blocker, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_presentation.input_blocker,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK |
                    LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_presentation.input_blocker,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE |
                       LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_presentation.completion_timer = lv_timer_create(
                                          _presentation_completion_timer_cb,
                                          APP_MANAGER_PRESENTATION_TIMER_MS,
                                          NULL);
    if (s_presentation.completion_timer == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_timer_pause(s_presentation.completion_timer);
    s_presentation.initialized = true;
    result = app_manager_presentation_load_immediate(
                 s_presentation.neutral_screen);
    if (result != ESP_OK)
    {
        goto fail;
    }
    return ESP_OK;

fail:
    if (s_presentation.completion_timer != NULL)
    {
        lv_timer_delete(s_presentation.completion_timer);
    }
    if (s_presentation.input_blocker != NULL &&
            lv_obj_is_valid(s_presentation.input_blocker))
    {
        lv_obj_delete(s_presentation.input_blocker);
    }
    if (s_presentation.neutral_screen != NULL &&
            lv_obj_is_valid(s_presentation.neutral_screen))
    {
        lv_obj_delete(s_presentation.neutral_screen);
    }
#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    _presentation_snapshot_abort();
    _presentation_snapshot_release_buffers();
#endif
    memset(&s_presentation, 0, sizeof(s_presentation));
    return result;
}

esp_err_t app_manager_presentation_deinit(void)
{
    if (!s_presentation.initialized)
    {
        return ESP_OK;
    }
    if (s_presentation.deinitializing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_presentation.deinitializing = true;
    app_manager_presentation_finish_now();
    _presentation_remove_loaded_event();
#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    _presentation_snapshot_abort();
#endif
    if (s_presentation.completion_timer != NULL)
    {
        lv_timer_pause(s_presentation.completion_timer);
        lv_timer_delete(s_presentation.completion_timer);
        s_presentation.completion_timer = NULL;
    }
    if (s_presentation.input_blocker != NULL &&
            lv_obj_is_valid(s_presentation.input_blocker))
    {
        lv_obj_delete(s_presentation.input_blocker);
    }
    if (s_presentation.neutral_screen != NULL &&
            lv_obj_is_valid(s_presentation.neutral_screen))
    {
        lv_obj_delete(s_presentation.neutral_screen);
    }
#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    _presentation_snapshot_release_buffers();
#endif
    memset(&s_presentation, 0, sizeof(s_presentation));
    return ESP_OK;
}

lv_obj_t *app_manager_presentation_create_page_screen(void)
{
    if (!s_presentation.initialized || s_presentation.deinitializing)
    {
        return NULL;
    }
    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen != NULL)
    {
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    }
    return screen;
}

esp_err_t app_manager_presentation_delete_page_screen(lv_obj_t *screen)
{
    if (!s_presentation.initialized || s_presentation.deinitializing)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_presentation_screen_is_valid(screen) ||
            screen == s_presentation.neutral_screen)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (screen == s_presentation.from_screen ||
            screen == s_presentation.to_screen ||
            screen == lv_display_get_screen_active(s_presentation.display))
    {
        return ESP_ERR_INVALID_STATE;
    }

    lv_anim_delete(screen, NULL);
    lv_obj_delete(screen);
    return ESP_OK;
}

lv_obj_t *app_manager_presentation_neutral_screen(void)
{
    return s_presentation.initialized ?
           s_presentation.neutral_screen : NULL;
}

esp_err_t app_manager_presentation_load_immediate(lv_obj_t *screen)
{
    if (!s_presentation.initialized || s_presentation.deinitializing ||
            s_presentation.running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_presentation_screen_is_valid(screen))
    {
        return ESP_ERR_INVALID_ARG;
    }

    lv_screen_load(screen);
    return lv_display_get_screen_active(s_presentation.display) == screen ?
           ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t app_manager_presentation_start(
    lv_obj_t *from_screen, lv_obj_t *to_screen,
    app_manager_transition_effect_t effect, uint32_t duration_ms,
    app_manager_presentation_completion_cb_t completion, void *context)
{
    lv_screen_load_anim_t animation;
    uint32_t generation;

    if (!s_presentation.initialized || s_presentation.deinitializing ||
            s_presentation.running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!_presentation_screen_is_valid(from_screen) ||
            !_presentation_screen_is_valid(to_screen) || completion == NULL ||
            duration_ms > APP_MANAGER_TRANSITION_MAX_DURATION_MS ||
            !_presentation_map_effect(effect, &animation))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (lv_display_get_screen_active(s_presentation.display) != from_screen)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (from_screen == to_screen || effect == APP_MANAGER_TRANSITION_NONE ||
            duration_ms == 0U)
    {
        esp_err_t result = app_manager_presentation_load_immediate(to_screen);
        if (result == ESP_OK)
        {
            completion(context);
        }
        return result;
    }

    generation = s_presentation.generation + 1U;
    if (generation == 0U)
    {
        generation = 1U;
    }
    s_presentation.generation = generation;
    s_presentation.running = true;
    s_presentation.from_screen = from_screen;
    s_presentation.to_screen = to_screen;
    s_presentation.completion = completion;
    s_presentation.completion_context = context;
    _presentation_set_blocked(true);

#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    app_manager_display_diagnostics_snapshot_prepare_started(effect);
    const esp_err_t snapshot_result = _presentation_start_snapshot(
                                          effect, duration_ms, generation);
    if (snapshot_result == ESP_OK)
    {
        return ESP_OK;
    }
    if (snapshot_result != ESP_ERR_NOT_SUPPORTED)
    {
        _presentation_reset_transition();
        return snapshot_result;
    }
#endif

    return _presentation_start_native(animation, effect, duration_ms,
                                      generation);
}

void app_manager_presentation_finish_now(void)
{
    lv_anim_t *animation = NULL;
    const uint32_t generation = s_presentation.generation;

    if (!s_presentation.initialized || !s_presentation.running)
    {
        return;
    }

#if APP_MANAGER_PRESENTATION_SNAPSHOT_ENABLED
    if (s_presentation.snapshot_running)
    {
        animation = lv_anim_get(&s_presentation,
                                _presentation_snapshot_progress_cb);
        if (animation != NULL)
        {
            animation->act_time = animation->duration;
        }
        lv_anim_refr_now();

        if (!s_presentation.running || generation != s_presentation.generation)
        {
            return;
        }
        if (lv_display_get_screen_active(s_presentation.display) !=
                s_presentation.to_screen)
        {
            (void)_presentation_recover_animation_start(
                s_presentation.from_screen, s_presentation.to_screen);
        }
        _presentation_snapshot_abort();
        _presentation_complete(generation);
        return;
    }
#endif

    animation = lv_anim_get(s_presentation.to_screen, NULL);
    if (animation != NULL)
    {
        animation->act_time = animation->duration;
    }
    animation = lv_anim_get(s_presentation.from_screen, NULL);
    if (animation != NULL)
    {
        animation->act_time = animation->duration;
    }
    lv_anim_refr_now();

    if (!s_presentation.running || generation != s_presentation.generation)
    {
        return;
    }
    if (lv_display_get_screen_active(s_presentation.display) !=
            s_presentation.to_screen)
    {
        _presentation_remove_loaded_event();
        lv_anim_delete(s_presentation.from_screen, NULL);
        lv_anim_delete(s_presentation.to_screen, NULL);
        (void)_presentation_recover_animation_start(
            s_presentation.from_screen, s_presentation.to_screen);
    }
    _presentation_complete(generation);
}

bool app_manager_presentation_is_running(void)
{
    return s_presentation.initialized && s_presentation.running;
}
