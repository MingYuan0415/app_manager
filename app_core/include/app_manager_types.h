/**
 * @brief  应用管理器类型定义
 * @note   不 include bsp_hal.h，apps 可安全 include。
 */

#ifndef __APP_MANAGER_TYPES_H__
#define __APP_MANAGER_TYPES_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"   /* lv_font_t, lv_obj_t */
#include "app_manager_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 生命周期消息枚举 ── */

/** @brief Lifecycle messages delivered to application page handlers. */
typedef enum
{
    APP_MANAGER_MSG_ONSTART = 0, /**< Page resources should be created. */
    APP_MANAGER_MSG_ONRESUME,   /**< Page became the active page. */
    APP_MANAGER_MSG_ONPAUSE,    /**< Page is no longer active. */
    APP_MANAGER_MSG_ONSTOP,     /**< Page resources must be released. */
    APP_MANAGER_MSG_USER_END,   /**< First application-defined message value. */
} app_manager_msg_type_t;

/**
 * @brief Receive one lifecycle message for an application page.
 * @param msg identifies the lifecycle transition.
 * @param param is the lifecycle-owned page context.
 */
typedef void (*app_page_msg_cb_t)(app_manager_msg_type_t msg, void *param);

/* ── 内置应用描述符 ── */

/** @brief Supported application registration classes. */
typedef enum
{
    APP_MANAGER_APP_BUILTIN = 0, /**< Linker-discovered static application. */
    APP_MANAGER_APP_DYNAMIC,     /**< Runtime-managed application. */
} app_manager_app_type_t;

/** @brief Linker-discoverable builtin application descriptor. */
typedef struct app_builtin_app_desc
{
    const char *name;            /**< Human-readable application name. */
    const void *icon;            /**< Optional application icon resource. */
    const char *id;              /**< Stable application identifier. */
    esp_err_t (*entry)(void);    /**< Entry point that registers the root page. */
    app_manager_app_type_t type; /**< Registration and ownership class. */
} app_builtin_app_desc_t;

/** @brief Logical keys accepted by the application manager. */
typedef enum
{
    APP_MANAGER_KEY_HOME = 0, /**< Home/navigation key. */
    APP_MANAGER_KEY_POWER,    /**< Power/screen key. */
} app_manager_key_t;

/** @brief Logical key events accepted by the application manager. */
typedef enum
{
    APP_MANAGER_KEY_EVENT_DOWN = 0, /**< Key press edge. */
    APP_MANAGER_KEY_EVENT_UP,       /**< Key release edge. */
    APP_MANAGER_KEY_EVENT_CLICK,    /**< Completed short click. */
    APP_MANAGER_KEY_EVENT_LONG_PRESS, /**< Long-press threshold reached. */
    APP_MANAGER_KEY_EVENT_DOUBLE_CLICK, /**< Completed double click. */
} app_manager_key_event_t;

/* ── Screen ctrl ops（app_manager 内部桥接 bsp_hal） ── */

/** @brief Hardware screen operations executed by app-control. */
typedef struct app_manager_screen_ctrl_ops
{
    /** Suspend the display hardware from the application-control task. */
    esp_err_t (*suspend)(void);
    /** Rebuild hardware while keeping the panel hidden and touch disabled. */
    esp_err_t (*resume_prepare)(void);
    /** Show the refreshed panel and enable the physical touch interrupt. */
    esp_err_t (*resume_commit)(void);
    /** Report whether the display is in a suspended transition state. */
    bool (*is_suspended)(void);
    /** Report whether the physical suspend transaction has committed. */
    bool (*is_suspend_committed)(void);
    /** Set and persist the normal display brightness. */
    esp_err_t (*set_brightness)(uint8_t brightness);
    /** Set temporary display brightness without persistence. */
    esp_err_t (*set_brightness_temp)(uint8_t brightness);
    /** Return the current normal display brightness. */
    uint8_t (*get_brightness)(void);
} app_manager_screen_ctrl_ops_t;

/* ── Input ops（由 main 从 bsp_input_ops_t 适配注入） ── */

/**
 * @brief Receive one typed input event from the BSP bridge.
 * @param key identifies the logical key.
 * @param event identifies the logical transition.
 * @param user_data is the registered callback context.
 */
typedef void (*app_manager_input_cb_t)(app_manager_key_t key,
                                       app_manager_key_event_t event,
                                       void *user_data);

/** @brief Registration operations for the BSP input bridge. */
typedef struct app_manager_input_ops
{
    /** Register a non-blocking producer callback during app_manager_init(). */
    esp_err_t (*register_handler)(app_manager_input_cb_t cb, void *user_data);
    /** Unregister the producer callback during application-control teardown. */
    esp_err_t (*unregister_handler)(void);
} app_manager_input_ops_t;

/* ── Standby ops（由 main 从 system_pm 适配注入） ── */

/** @brief Non-blocking operations for the system standby bridge. */
typedef struct app_manager_standby_ops
{
    /** Queue standby work without running sleep inline. */
    esp_err_t (*request_standby)(void);
    /** Queue cancellation of a pending standby transaction. */
    esp_err_t (*cancel_standby)(void);
} app_manager_standby_ops_t;

/* ── UI dispatcher 函数指针类型（mailbox 满时返回 ESP_ERR_NO_MEM） ── */

/**
 * @brief Queue a callback on the LVGL worker.
 * @param cb is the callback to enqueue.
 * @param arg is the opaque callback argument.
 * @return ESP_OK when queued, otherwise an admission error.
 */
typedef esp_err_t (*app_manager_ui_dispatch_fn)(void (*cb)(void *), void *arg);
/**
 * @brief Execute a fallible callback on the LVGL worker.
 * @param arg is the opaque callback argument.
 * @return Callback-specific result.
 */
typedef esp_err_t (*app_manager_ui_call_fn)(void *arg);

/** @brief Application-level display and standby states. */
typedef enum
{
    APP_MANAGER_PM_ACTIVE = 0, /**< Screen and touch are active. */
    APP_MANAGER_PM_DIM,        /**< Temporary dim brightness is active. */
    APP_MANAGER_PM_SCREEN_OFF, /**< Display is physically suspended. */
    APP_MANAGER_PM_STANDBY_PREPARING, /**< Sleep request has not committed. */
    APP_MANAGER_PM_STANDBY, /**< Light sleep committed. */
} app_manager_pm_state_t;

/* ── app_manager_config_t 前向声明（完整定义在 app_manager_config.h，仅 main 用） ── */

struct app_manager_config;

#ifdef __cplusplus
}
#endif

#endif /* __APP_MANAGER_TYPES_H__ */
