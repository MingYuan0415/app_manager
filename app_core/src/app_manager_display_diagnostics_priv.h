/** @brief App Core-private display diagnostic hooks. */
#ifndef __APP_MANAGER_DISPLAY_DIAGNOSTICS_PRIV_H__
#define __APP_MANAGER_DISPLAY_DIAGNOSTICS_PRIV_H__

#include "app_manager_display_diagnostics.h"

#include "lvgl.h"

/** @brief Start display diagnostics for a registered LVGL display. */
esp_err_t app_manager_display_diagnostics_start(lv_display_t *display);

/** @brief Stop the diagnostics task and release its resources. */
esp_err_t app_manager_display_diagnostics_stop(void);

/**
 * @brief Record a panel submission made while the caller owns the LVGL lock.
 */
void app_manager_display_diagnostics_record_submit(lv_display_t *display,
        bool final_flush, uint32_t pixel_count, uint32_t wait_us,
        esp_err_t result);

/** @brief Record that an LVGL animation was confirmed as started. */
#if CONFIG_APP_MANAGER_DISPLAY_DIAGNOSTICS
void app_manager_display_diagnostics_transition_started(
    app_manager_transition_effect_t effect);

/**
 * @brief Mark the active animation complete and awaiting final presentation.
 *
 * The transition remains active until its next successful final panel submit.
 */
void app_manager_display_diagnostics_transition_completed(void);

/** @brief Record cancellation of the active LVGL animation. */
void app_manager_display_diagnostics_transition_cancelled(void);
#else
static inline void app_manager_display_diagnostics_transition_started(
    app_manager_transition_effect_t effect)
{
    (void)effect;
}

static inline void app_manager_display_diagnostics_transition_completed(void)
{
}

static inline void app_manager_display_diagnostics_transition_cancelled(void)
{
}
#endif

#endif /* __APP_MANAGER_DISPLAY_DIAGNOSTICS_PRIV_H__ */
