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

/** @brief Immutable dependencies copied during application-manager startup. */
typedef struct app_manager_config
{
    const bsp_display_port_t *disp_port;      /**< panel / panel_io / touch / width / height */
    const uint16_t           *font_sizes;     /**< 字号表，例 {18,22,28,36,48,54} */
    size_t                    font_count;     /**< 字号表长度，例 6 */
    char                      res_fs_letter;  /**< 资源分区盘符，例 'F' */
    app_manager_screen_ctrl_ops_t screen_ops; /**< Required; called only by app-control. */
    app_manager_input_ops_t       input_ops;  /**< Required typed input bridge. */
    app_manager_standby_ops_t     standby_ops; /**< Required non-blocking system-PM bridge. */
    size_t                    page_memory_bytes; /**< 0 selects the 32 KiB default */
    size_t                    max_resident_apps; /**< 0 selects the physical maximum. */
    app_manager_resident_policy_t resident_policy; /**< Full-capacity behavior. */
    app_manager_transition_t  app_forward_transition; /**< Zero selects FADE, 220 ms. */
    app_manager_transition_t  app_back_transition; /**< Zero selects FADE, 220 ms. */
    app_manager_transition_t  page_forward_transition; /**< Zero selects PUSH_LEFT, 220 ms. */
    app_manager_transition_t  page_back_transition; /**< Zero selects PUSH_RIGHT, 220 ms. */
} app_manager_config_t;

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_CONFIG_H__ */
