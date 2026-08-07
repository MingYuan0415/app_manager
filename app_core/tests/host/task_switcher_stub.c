/** @brief Host stub of the system task switcher for UI-free test targets. */
#include "app_manager_task_switcher.h"

#include <stdbool.h>
#include <string.h>

unsigned host_task_switcher_show_count;
unsigned host_task_switcher_navigation_begin_count;
unsigned host_task_switcher_clear_all_finished_count;
esp_err_t host_task_switcher_show_result;

esp_err_t app_manager_task_switcher_init(void *display)
{
    (void)display;
    return ESP_OK;
}

esp_err_t app_manager_task_switcher_deinit(void)
{
    return ESP_OK;
}

esp_err_t app_manager_task_switcher_show(void)
{
    host_task_switcher_show_count++;
    return host_task_switcher_show_result;
}

void app_manager_task_switcher_navigation_begin(void)
{
    host_task_switcher_navigation_begin_count++;
}

bool app_manager_task_switcher_is_visible(void)
{
    return false;
}

void app_manager_task_switcher_screen_suspend(void)
{
}

void app_manager_task_switcher_clear_all_finished(esp_err_t result)
{
    (void)result;
    host_task_switcher_clear_all_finished_count++;
}
