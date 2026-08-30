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
#define APP_MANAGER_NAV_COMMAND_CAPACITY \
    CONFIG_APP_MANAGER_NAV_COMMAND_CAPACITY
/** Maximum supported transition duration. */
#define APP_MANAGER_TRANSITION_MAX_DURATION_MS        2000U
/** Built-in duration used when a configured duration is zero. */
#define APP_MANAGER_TRANSITION_DEFAULT_DURATION_MS    220U
/** Physical running-application capacity. */
#define APP_MANAGER_MAX_RESIDENT_APPS CONFIG_APP_MANAGER_MAX_RESIDENT_APPS
/** Physical retained-page capacity. */
#define APP_MANAGER_MAX_PAGE_INSTANCES CONFIG_APP_MANAGER_MAX_PAGE_INSTANCES
/** Maximum private-state bytes owned by one retained Page instance. */
#define APP_MANAGER_PAGE_STATE_BYTES CONFIG_APP_MANAGER_PAGE_STATE_BYTES
/** Maximum tasks reported by the recent-tasks snapshot. */
#define APP_MANAGER_MAX_RECENT_TASKS  3U
/** Preview width in pixels of one retained task thumbnail. */
#define APP_MANAGER_RECENT_PREVIEW_WIDTH 230U
/** Preview height in pixels of one retained task thumbnail. */
#define APP_MANAGER_RECENT_PREVIEW_HEIGHT 280U

#ifdef __cplusplus
static_assert(APP_MANAGER_PAGE_STATE_BYTES % 8U == 0U,
              "Page state capacity must be 8-byte aligned");
#else
_Static_assert(APP_MANAGER_PAGE_STATE_BYTES % 8U == 0U,
               "Page state capacity must be 8-byte aligned");
#endif

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

/** @brief Callback-scoped state and immutable inputs for Page lifecycle ops. */
typedef struct app_manager_page_context
{
    /** Page-private state valid until the Page instance is released. */
    void *state;
    /** Descriptor-owned route data valid for the firmware lifetime. */
    void *route_user_data;
    /** Mounted Page Screen, or NULL when no Screen is owned. */
    lv_obj_t *screen;
    /** Current immutable navigation arguments, or NULL when absent. */
    const app_manager_typed_blob_t *arguments;
} app_manager_page_context_t;

/**
 * @brief Infallible Page lifecycle operation.
 * @param context is valid only for the duration of this callback.
 */
typedef void (*app_manager_page_stage_cb_t)(
    const app_manager_page_context_t *context);
/**
 * @brief Retryable PAUSE or STOP cleanup operation.
 * @param context is valid only for the duration of this callback.
 * @return ESP_OK on success, otherwise a cleanup error retained for retry.
 */
typedef esp_err_t (*app_manager_page_cleanup_cb_t)(
    const app_manager_page_context_t *context);

/** @brief Optional typed lifecycle operations for one Page definition. */
typedef struct app_manager_page_ops
{
    app_manager_page_stage_cb_t start; /**< Initialize nonvisual state. */
    app_manager_page_stage_cb_t mount; /**< Create UI under context->screen. */
    app_manager_page_stage_cb_t resume; /**< Start foreground resources. */
    app_manager_page_cleanup_cb_t pause; /**< Stop foreground resources. */
    app_manager_page_stage_cb_t unmount; /**< Destroy Page UI. */
    app_manager_page_cleanup_cb_t stop; /**< Release retained resources. */
    app_manager_page_stage_cb_t new_intent; /**< Consume updated arguments. */
} app_manager_page_ops_t;

/** @brief Flags attached to an application descriptor. */
typedef enum
{
    APP_MANAGER_APP_FLAG_NONE = 0,       /**< No special policy. */
    APP_MANAGER_APP_FLAG_PINNED = 1U << 0, /**< Never evict automatically. */
    APP_MANAGER_APP_FLAG_HIDDEN = 1U << 1, /**< Omit from normal launchers. */
} app_manager_app_flag_t;

/** @brief Immutable implementation shared by one or more application routes. */
typedef struct app_manager_page_definition
{
    /** Raw lifecycle handler; mutually exclusive with ops. */
    app_page_msg_cb_t handler;
    /** Typed lifecycle operations; mutually exclusive with handler. */
    const app_manager_page_ops_t *ops;
    uint32_t memory_size;      /**< Zeroed private bytes required per instance. */
} app_manager_page_definition_t;

/** @brief Application-owned route to an immutable page definition. */
typedef struct app_manager_page_route
{
    const char *page_id; /**< Stable page identifier within the owning app. */
    const app_manager_page_definition_t *definition; /**< Shared implementation. */
    void *user_data; /**< Stable route-specific handler user data, or NULL. */
} app_manager_page_route_t;

/** @brief Linker-discoverable builtin application descriptor. */
typedef struct app_manager_app_desc
{
    const char *name;             /**< Human-readable application name. */
    uint32_t icon_id;             /**< Semantic application icon ID, or zero. */
    const char *id;               /**< Stable application identifier. */
    const char *root_page_id;     /**< ID of the statically described root page. */
    const app_manager_page_route_t *page_routes; /**< Owned static route array. */
    size_t page_route_count;      /**< Number of entries in page_routes. */
    uint32_t flags;               /**< Bitwise app_manager_app_flag_t values. */
    const char *display_name;     /**< Localized display name, or NULL to use name. */
} app_manager_app_desc_t;

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

/**
 * @brief One task reported by the recent-tasks snapshot.
 *
 * The preview points to a fixed App Manager-owned PSRAM draw buffer that is
 * valid only until the owning application is destroyed or the manager stops.
 * Callers must never modify the preview data.
 */
typedef struct app_manager_recent_task
{
    const char *app_id;           /**< Stable application identifier. */
    const char *display_name;     /**< Localized name, or descriptor name. */
    uint32_t icon_id;             /**< Semantic application icon ID, or zero. */
    const char *page_id;          /**< Retained current page identifier. */
    bool preview_valid;           /**< preview contains a captured thumbnail. */
    const lv_draw_buf_t *preview; /**< Fixed preview draw buffer, or NULL. */
    bool active;                  /**< This task is the current foreground task. */
    bool cleanup_pending;         /**< Cleanup failed; the instance retains resources. */
} app_manager_recent_task_t;

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
