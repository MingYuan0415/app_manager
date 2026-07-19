#include "app_manager_presentation.h"

#include <stdint.h>
#include <string.h>

#define APP_MANAGER_PRESENTATION_TIMER_MS 1U

typedef struct app_manager_presentation_state
{
    bool initialized;
    bool deinitializing;
    bool running;
    bool load_event_seen;
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
} app_manager_presentation_state_t;

static app_manager_presentation_state_t s_presentation;

static void _presentation_completion_timer_cb(lv_timer_t *timer);

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

static void _presentation_reset_transition(void)
{
    _presentation_remove_loaded_event();
    if (s_presentation.completion_timer != NULL)
    {
        lv_timer_pause(s_presentation.completion_timer);
    }
    _presentation_set_blocked(false);
    s_presentation.running = false;
    s_presentation.load_event_seen = false;
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

    s_presentation.load_event_seen = true;
    s_presentation.scheduled_generation = generation;
    _presentation_remove_loaded_event();
    lv_timer_ready(s_presentation.completion_timer);
    lv_timer_resume(s_presentation.completion_timer);
}

static void _presentation_completion_timer_cb(lv_timer_t *timer)
{
    const uint32_t generation = s_presentation.scheduled_generation;

    lv_timer_pause(timer);
    if (!s_presentation.load_event_seen ||
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

esp_err_t app_manager_presentation_init(void)
{
    esp_err_t result = ESP_OK;
    lv_obj_t *top_layer = NULL;

    if (s_presentation.initialized)
    {
        goto exit;
    }

    memset(&s_presentation, 0, sizeof(s_presentation));
    s_presentation.display = lv_display_get_default();
    if (s_presentation.display == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }

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

    top_layer = lv_display_get_layer_top(s_presentation.display);
    if (top_layer == NULL)
    {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    s_presentation.input_blocker = lv_obj_create(top_layer);
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
    goto exit;

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
    memset(&s_presentation, 0, sizeof(s_presentation));

exit:
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
    memset(&s_presentation, 0, sizeof(s_presentation));
    return ESP_OK;
}

lv_obj_t *app_manager_presentation_create_page_screen(void)
{
    lv_obj_t *screen = NULL;

    if (!s_presentation.initialized || s_presentation.deinitializing)
    {
        goto exit;
    }
    screen = lv_obj_create(NULL);
    if (screen != NULL)
    {
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    }

exit:
    return screen;
}

esp_err_t app_manager_presentation_delete_page_screen(lv_obj_t *screen)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;

    if (!s_presentation.initialized || s_presentation.deinitializing)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!_presentation_screen_is_valid(screen) ||
            screen == s_presentation.neutral_screen)
    {
        goto exit;
    }
    if (screen == s_presentation.from_screen ||
            screen == s_presentation.to_screen ||
            screen == lv_display_get_screen_active(s_presentation.display))
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    lv_anim_delete(screen, NULL);
    lv_obj_delete(screen);
    result = ESP_OK;

exit:
    return result;
}

lv_obj_t *app_manager_presentation_neutral_screen(void)
{
    return s_presentation.initialized ?
           s_presentation.neutral_screen : NULL;
}

esp_err_t app_manager_presentation_load_immediate(lv_obj_t *screen)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;

    if (!s_presentation.initialized || s_presentation.deinitializing ||
            s_presentation.running)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!_presentation_screen_is_valid(screen))
    {
        goto exit;
    }

    lv_screen_load(screen);
    result = lv_display_get_screen_active(s_presentation.display) == screen ?
             ESP_OK : ESP_ERR_INVALID_STATE;

exit:
    return result;
}

esp_err_t app_manager_presentation_start(
    lv_obj_t *from_screen, lv_obj_t *to_screen,
    app_manager_transition_effect_t effect, uint32_t duration_ms,
    app_manager_presentation_completion_cb_t completion, void *context)
{
    esp_err_t result = ESP_ERR_INVALID_ARG;
    lv_screen_load_anim_t animation;
    uint32_t generation;

    if (!s_presentation.initialized || s_presentation.deinitializing ||
            s_presentation.running)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    if (!_presentation_screen_is_valid(from_screen) ||
            !_presentation_screen_is_valid(to_screen) || completion == NULL ||
            duration_ms > APP_MANAGER_TRANSITION_MAX_DURATION_MS ||
            !_presentation_map_effect(effect, &animation))
    {
        goto exit;
    }
    if (lv_display_get_screen_active(s_presentation.display) != from_screen)
    {
        result = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    if (from_screen == to_screen || effect == APP_MANAGER_TRANSITION_NONE ||
            duration_ms == 0U)
    {
        result = app_manager_presentation_load_immediate(to_screen);
        if (result == ESP_OK)
        {
            completion(context);
        }
        goto exit;
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
    s_presentation.loaded_event = lv_obj_add_event_cb(
                                      to_screen,
                                      _presentation_loaded_event_cb,
                                      LV_EVENT_SCREEN_LOADED,
                                      (void *)(uintptr_t)generation);
    if (s_presentation.loaded_event == NULL)
    {
        result = ESP_ERR_NO_MEM;
        _presentation_reset_transition();
        goto exit;
    }
    _presentation_set_blocked(true);

    lv_screen_load_anim(to_screen, animation, duration_ms, 0U, false);
    if (lv_anim_get(to_screen, NULL) != NULL)
    {
        result = ESP_OK;
        goto exit;
    }

    _presentation_remove_loaded_event();
    lv_anim_delete(from_screen, NULL);
    result = lv_display_get_screen_active(s_presentation.display) == to_screen ?
             ESP_OK : _presentation_recover_animation_start(from_screen,
                 to_screen);
    if (result == ESP_OK)
    {
        _presentation_complete(generation);
    }
    else
    {
        _presentation_reset_transition();
    }

exit:
    return result;
}

void app_manager_presentation_finish_now(void)
{
    lv_anim_t *animation = NULL;
    const uint32_t generation = s_presentation.generation;

    if (!s_presentation.initialized || !s_presentation.running)
    {
        return;
    }

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
