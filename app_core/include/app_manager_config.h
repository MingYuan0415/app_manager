/**
 * @brief  应用管理器配置类型
 * @note   含 BSP 类型，仅 main 与 app_manager 内部 adapter 用；apps 不 include。
 */

#ifndef __APP_MANAGER_CONFIG_H__
#define __APP_MANAGER_CONFIG_H__

#include "bsp_hal.h"
#include "app_manager_types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Product mapping from a semantic image ID to one mmap asset. */
typedef struct app_manager_image_resource_config
{
    uint32_t semantic_id; /**< Nonzero product-owned stable image identity. */
    uint16_t asset_index; /**< Generated mmap asset index. */
    uint16_t width;       /**< Expected decoded width. */
    uint16_t height;      /**< Expected decoded height. */
} app_manager_image_resource_config_t;

/** @brief Immutable dependencies copied during application-manager startup. */
typedef struct app_manager_config
{
    const bsp_display_port_t *disp_port;      /**< panel / panel_io / touch / width / height */
    const uint16_t           *font_sizes;     /**< Exactly APP_THEME_FONT_MAX entries in role order when non-NULL; NULL selects APP_THEME_FONT_SIZES. */
    size_t                    font_count;     /**< Must equal APP_THEME_FONT_MAX (7) when font_sizes is non-NULL. */
    char                      res_fs_letter;  /**< 资源分区盘符，例 'F' */
    uint16_t                  resource_file_count; /**< Generated mmap file count. */
    uint16_t                  resource_checksum; /**< Generated mmap checksum. */
    const app_manager_image_resource_config_t *image_resources; /**< Semantic image map. */
    size_t                    image_resource_count; /**< Semantic image map length. */
    app_manager_screen_ctrl_ops_t screen_ops; /**< Required; suspend/resume run on app-control, brightness and resume_commit on the UI worker. */
    app_manager_input_ops_t       input_ops;  /**< Required typed input bridge. */
    app_manager_standby_ops_t     standby_ops; /**< Required non-blocking system-PM bridge. */
    app_manager_control_center_ops_t control_center_ops; /**< Optional; NULL hides tiles. */
    size_t                    max_resident_apps; /**< 0 selects APP_MANAGER_MAX_RESIDENT_APPS; larger values are rejected. */
    app_manager_resident_policy_t resident_policy; /**< Full-capacity behavior. */
    app_manager_transition_t  app_forward_transition; /**< Zero selects FADE, 220 ms. */
    app_manager_transition_t  app_back_transition; /**< Zero selects FADE, 220 ms. */
    app_manager_transition_t  page_forward_transition; /**< Zero selects PUSH_LEFT, 220 ms. */
    app_manager_transition_t  page_back_transition; /**< Zero selects PUSH_RIGHT, 220 ms. */
    uint32_t                  control_task_priority; /**< App-control worker priority; must be in [1, configMAX_PRIORITIES). */
} app_manager_config_t;

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_CONFIG_H__ */
