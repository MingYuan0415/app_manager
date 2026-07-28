#include "app_manager_display_diagnostics_priv.h"

#include <assert.h>
#include <stddef.h>

int main(void)
{
    assert(app_manager_display_diagnostics_start(NULL) == ESP_OK);
    assert(app_manager_display_diagnostics_begin_benchmark(30U, 14720U) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(app_manager_display_diagnostics_wait_for_presentation(1000U) ==
           ESP_ERR_NOT_SUPPORTED);
    app_manager_display_diagnostics_record_submit(NULL, true, 7360U, 100U,
            ESP_OK);
    app_manager_display_diagnostics_transition_started(
        APP_MANAGER_TRANSITION_FADE);
    app_manager_display_diagnostics_snapshot_prepare_started(
        APP_MANAGER_TRANSITION_FADE);
    app_manager_display_diagnostics_snapshot_prepare_finished(false);
    app_manager_display_diagnostics_transition_completed();
    app_manager_display_diagnostics_transition_cancelled();
    assert(app_manager_display_diagnostics_stop() == ESP_OK);
    return 0;
}
