/** @brief Host stub of the system control center for UI-free test targets. */
#include "app_manager_control_center.h"

#include <stdbool.h>

unsigned host_control_center_show_count;
unsigned host_control_center_navigation_begin_count;
esp_err_t host_control_center_show_result;

esp_err_t app_manager_control_center_init(void *display, void *touch)
{
    (void)display;
    (void)touch;
    return ESP_OK;
}

void app_manager_control_center_set_ops(
    const app_manager_control_center_ops_t *ops)
{
    (void)ops;
}

esp_err_t app_manager_control_center_deinit(void)
{
    return ESP_OK;
}

esp_err_t app_manager_control_center_show(void)
{
    host_control_center_show_count++;
    return host_control_center_show_result;
}

esp_err_t app_manager_control_center_hide(void)
{
    return ESP_OK;
}

void app_manager_control_center_navigation_begin(void)
{
    host_control_center_navigation_begin_count++;
}

bool app_manager_control_center_is_visible(void)
{
    return false;
}

void app_manager_control_center_screen_suspend(void)
{
}

void app_manager_control_center_screen_resume(void)
{
}

esp_err_t app_manager_control_center_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

bool app_manager_control_center_is_enabled(void)
{
    return false;
}