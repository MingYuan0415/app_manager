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

/** Maximum bytes, including the terminator, in an application or page ID. */
#define APP_MANAGER_ID_BYTES                         32U
/** Total size of one copied navigation argument blob. */
#define APP_MANAGER_TYPED_BLOB_BYTES                 96U
/** Size of the versioned typed-blob header. */
#define APP_MANAGER_TYPED_BLOB_HEADER_BYTES          8U
/** Maximum typed-blob payload size. */
#define APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES         \
    (APP_MANAGER_TYPED_BLOB_BYTES - APP_MANAGER_TYPED_BLOB_HEADER_BYTES)
/** Current typed-blob envelope version. */
#define APP_MANAGER_TYPED_BLOB_VERSION               1U
/** Fixed navigation-command pool capacity. */
#define APP_MANAGER_NAV_COMMAND_CAPACITY              24U
/** Maximum supported transition duration. */
#define APP_MANAGER_TRANSITION_MAX_DURATION_MS        2000U
/** Built-in duration used when a configured duration is zero. */
#define APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS    220U
/** Physical running-application capacity. */
#define APP_MANAGER_MAX_RESIDENT_APPS                 8U
/** Physical retained-page capacity. */
#define APP_MANAGER_MAX_PAGE_INSTANCES                12U

/** @brief Versioned, value-owned navigation arguments. */
typedef struct app_manager_typed_blob
{
    uint16_t version; /**< Envelope version; use APP_MANAGER_TYPED_BLOB_VERSION. */
    uint16_t type;    /**< Application-defined payload type. */
    uint16_t size;    /**< Valid bytes in payload. */
    uint16_t reserved; /**< Must be zero. */
    uint8_t payload[APP_MANAGER_TYPED_BLOB_PAYLOAD_BYTES]; /**< Copied payload. */
} app_manager_typed_blob_t;

#ifdef __cplusplus
static_assert(sizeof(app_manager_typed_blob_t) == APP_MANAGER_TYPED_BLOB_BYTES,
              "app_manager_typed_blob_t must remain 96 bytes");
#else
_Static_assert(sizeof(app_manager_typed_blob_t) == APP_MANAGER_TYPED_BLOB_BYTES,
               "app_manager_typed_blob_t must remain 96 bytes");
#endif

/* ── Lifecycle and registry contracts ── */

/** @brief Lifecycle messages delivered to application page handlers. */
typedef enum
{
    APP_MANAGER_MSG_ONSTART = 0, /**< Initialize retained nonvisual state. */
    APP_MANAGER_MSG_ONMOUNT,    /**< Create UI under the page-owned screen. */
    APP_MANAGER_MSG_ONRESUME,   /**< Start foreground sessions and input. */
    APP_MANAGER_MSG_ONPAUSE,    /**< Stop foreground sessions and input. */
    APP_MANAGER_MSG_ONUNMOUNT,  /**< Destroy UI owned by the page screen. */
    APP_MANAGER_MSG_ONSTOP,     /**< Release the remaining retained page state. */
    APP_MANAGER_MSG_ONNEWINTENT, /**< Receive updated navigation arguments. */
    APP_MANAGER_MSG_USER_END,   /**< First application-defined message value. */
} app_manager_msg_type_t;

/**
 * @brief Receive one lifecycle message for an application page.
 * @param msg identifies the lifecycle transition.
 * @param param is descriptor-owned page user data.
 *
 * @note Navigation arguments are exposed through the current-page accessors;
 *       param retains the same meaning for every lifecycle message.
 */
typedef void (*app_page_msg_cb_t)(app_manager_msg_type_t msg, void *param);

/** @brief Supported application registration classes. */
typedef enum
{
    APP_MANAGER_APP_BUILTIN = 0, /**< Linker-discovered static application. */
    APP_MANAGER_APP_DYNAMIC,     /**< Runtime-managed application. */
} app_manager_app_type_t;

/** @brief Flags attached to an application descriptor. */
typedef enum
{
    APP_MANAGER_APP_FLAG_NONE = 0,       /**< No special policy. */
    APP_MANAGER_APP_FLAG_PINNED = 1U << 0, /**< Never evict automatically. */
    APP_MANAGER_APP_FLAG_HIDDEN = 1U << 1, /**< Omit from normal launchers. */
} app_manager_app_flag_t;

/** @brief Linker-discoverable application descriptor. */
typedef struct app_manager_app_desc
{
    const char *name;             /**< Human-readable application name. */
    const void *icon;             /**< Optional application icon resource. */
    const char *id;               /**< Stable application identifier. */
    const char *root_page_id;     /**< ID of the statically described root page. */
    uint32_t flags;               /**< Bitwise app_manager_app_flag_t values. */
    app_manager_app_type_t type;  /**< Provider and ownership class. */
} app_manager_app_desc_t;

/** @brief Linker-discoverable static page descriptor. */
typedef struct app_manager_page_desc
{
    const char *app_id;           /**< Owning application identifier. */
    const char *page_id;          /**< Stable page identifier within the app. */
    app_page_msg_cb_t handler;    /**< Lifecycle handler. */
    void *user_data;              /**< Stable handler user data, or NULL. */
    uint32_t memory_size;         /**< Zeroed private bytes required by the page. */
} app_manager_page_desc_t;

/** @brief Native screen-transition effects. */
typedef enum
{
    APP_MANAGER_TRANSITION_DEFAULT = 0, /**< Use the configured effect. */
    APP_MANAGER_TRANSITION_NONE,        /**< Switch immediately. */
    APP_MANAGER_TRANSITION_FADE,        /**< Cross-fade screens. */
    APP_MANAGER_TRANSITION_COVER_LEFT,  /**< New screen covers toward the left. */
    APP_MANAGER_TRANSITION_COVER_RIGHT, /**< New screen covers toward the right. */
    APP_MANAGER_TRANSITION_COVER_UP,    /**< New screen covers upward. */
    APP_MANAGER_TRANSITION_COVER_DOWN,  /**< New screen covers downward. */
    APP_MANAGER_TRANSITION_PUSH_LEFT,   /**< Push both screens toward the left. */
    APP_MANAGER_TRANSITION_PUSH_RIGHT,  /**< Push both screens toward the right. */
    APP_MANAGER_TRANSITION_PUSH_UP,     /**< Push both screens upward. */
    APP_MANAGER_TRANSITION_PUSH_DOWN,   /**< Push both screens downward. */
    APP_MANAGER_TRANSITION_REVEAL_LEFT, /**< Old screen reveals toward the left. */
    APP_MANAGER_TRANSITION_REVEAL_RIGHT, /**< Old screen reveals toward the right. */
    APP_MANAGER_TRANSITION_REVEAL_UP,   /**< Old screen reveals upward. */
    APP_MANAGER_TRANSITION_REVEAL_DOWN, /**< Old screen reveals downward. */
    APP_MANAGER_TRANSITION_END,         /**< Internal range sentinel. */
} app_manager_transition_effect_t;

/** @brief Effect and duration used by one navigation transition. */
typedef struct app_manager_transition
{
    app_manager_transition_effect_t effect; /**< Effect or DEFAULT. */
    uint16_t duration_ms; /**< Zero selects the configured duration. */
    uint16_t reserved;    /**< Must be zero. */
} app_manager_transition_t;

/** @brief Unified navigation operations. */
typedef enum
{
    APP_MANAGER_NAV_OP_RUN = 0,        /**< Start or foreground an application. */
    APP_MANAGER_NAV_OP_EXIT,           /**< Stop a named application. */
    APP_MANAGER_NAV_OP_EXIT_SELF,      /**< Stop the lifecycle-callback owner. */
    APP_MANAGER_NAV_OP_OPEN_PAGE,      /**< Start, foreground, or open a static page. */
    APP_MANAGER_NAV_OP_BACK,           /**< Navigate back one page or application. */
    APP_MANAGER_NAV_OP_BACK_TO,        /**< Navigate back to a retained page. */
    APP_MANAGER_NAV_OP_REMOVE_PAGE,    /**< Remove a retained page. */
    APP_MANAGER_NAV_OP_CLEANUP_BACKGROUND, /**< Stop eligible background apps. */
    APP_MANAGER_NAV_OP_END,            /**< Internal range sentinel. */
} app_manager_nav_operation_t;

/** @brief Value copied into one synchronous or asynchronous navigation command. */
typedef struct app_manager_nav_request
{
    app_manager_nav_operation_t operation; /**< Requested navigation operation. */
    const char *app_id;                    /**< Target app ID; NULL for EXIT_SELF input. */
    const char *page_id;                   /**< Optional target static page ID. */
    bool has_arguments;                    /**< arguments contains a typed value. */
    app_manager_typed_blob_t arguments;    /**< Fully copied launch arguments. */
    app_manager_transition_t transition;   /**< Per-request transition override. */
} app_manager_nav_request_t;

/**
 * @brief Receive the final result of an asynchronous navigation request.
 * @param result is the completed lifecycle and presentation result.
 * @param context is the caller-provided completion context.
 */
typedef void (*app_manager_nav_completion_cb_t)(esp_err_t result, void *context);

/** @brief Policy applied when the resident-application limit is reached. */
typedef enum
{
    APP_MANAGER_RESIDENT_REJECT = 0, /**< Reject the new application. */
    APP_MANAGER_RESIDENT_EVICT_OLDEST_BACKGROUND, /**< Evict oldest eligible app. */
} app_manager_resident_policy_t;

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
    /**
     * Return whether a new standby transaction may be started.
     *
     * This callback is optional; a NULL callback permits standby. It is
     * evaluated before the application-control generation is advanced, so a
     * temporary development/debug policy can keep the screen off without
     * creating a system-PM request.
     */
    bool (*is_standby_allowed)(void);
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
