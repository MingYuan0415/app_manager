#ifndef __APP_MANAGER_PM_INTERNAL_H__
#define __APP_MANAGER_PM_INTERNAL_H__

#include "app_manager.h"
#include "app_manager_config.h"

#ifndef HOST_TEST
    #include "esp_attr.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/queue.h"
    #include "freertos/semphr.h"
    #include "freertos/task.h"
#else
    #include "host_control_port.h"
#endif

#include <stdatomic.h>

/** @brief Maximum number of commands held by app-control. */
#define APP_CONTROL_QUEUE_DEPTH          CONFIG_APP_MANAGER_CONTROL_QUEUE_DEPTH
/** @brief Stack size of the app-control task in bytes. */
#define APP_CONTROL_TASK_STACK           CONFIG_APP_MANAGER_CONTROL_TASK_STACK
/** @brief Poll and retry period in milliseconds. */
#define APP_CONTROL_POLL_MS              100
/** @brief Maximum startup wait for the app-control task. */
#define APP_CONTROL_TASK_READY_MS        1000
/** @brief Default active-to-dim timeout in milliseconds. */
#define APP_CONTROL_DEFAULT_DIM_MS       15000
/** @brief Default dim-to-off timeout in milliseconds. */
#define APP_CONTROL_DEFAULT_OFF_MS       30000
/** @brief Default screen-off-to-standby delay in milliseconds. */
#define APP_CONTROL_DEFAULT_STANDBY_MS   5000
/** @brief Temporary brightness used while dimmed. */
#define APP_CONTROL_DIM_BRIGHTNESS       76

/** @brief Encode one physical power-key event for the fallback bit mask. */
#define POWER_PENDING_EVENT(event)       (1U << (unsigned)(event))
/** @brief Wake-classification bit stored with the current HOME generation. */
#define HOME_SEQUENCE_WAKE_MASK          (UINT32_C(1) << 31)
/** @brief Generation bits stored in the HOME sequence state. */
#define HOME_SEQUENCE_GENERATION_MASK    (HOME_SEQUENCE_WAKE_MASK - 1U)

/** @brief NVS key storing the normal display brightness. */
#define NVS_KEY_BRIGHTNESS               "brightness"
/** @brief NVS key storing the active-to-dim timeout. */
#define NVS_KEY_DIM_TIMEOUT              "pm_dim_to"
/** @brief NVS key storing the dim-to-off timeout. */
#define NVS_KEY_OFF_TIMEOUT              "pm_timeout"
/** @brief NVS key storing the standby delay. */
#define NVS_KEY_STANDBY_TIMEOUT          "pm_stby_dly"

/** @brief Commands serialized by the application-control task. */
typedef enum
{
    CONTROL_WAKE_SIGNAL = 0,       /**< Generic user-activity notification. */
    CONTROL_TOUCH_ACTIVITY,        /**< Touch activity with a generation. */
    CONTROL_INPUT,                 /**< Typed key event. */
    CONTROL_SUSPEND,               /**< Request screen suspension. */
    CONTROL_RESUME,                /**< Request active-screen restoration. */
    CONTROL_SET_BRIGHTNESS,        /**< Persist and apply normal brightness. */
    CONTROL_SET_BRIGHTNESS_TEMP,   /**< Apply temporary brightness. */
    CONTROL_SET_OFF_TIMEOUT,       /**< Persist inactivity timeout. */
    CONTROL_SET_STANDBY_TIMEOUT,   /**< Persist standby delay. */
    CONTROL_STOP,                  /**< Stop the worker after restoration. */
} control_command_type_t;

/** @brief Initialization state of the application-control subsystem. */
typedef enum
{
    CONTROL_UNINITIALIZED = 0, /**< No task or queue is owned. */
    CONTROL_INITIALIZING,      /**< Startup is in progress. */
    CONTROL_READY,             /**< Producers may submit commands. */
    CONTROL_DEINITIALIZING,    /**< Teardown rejects new producers. */
} control_lifecycle_state_t;

/** @brief One fixed-size command passed through the control queue. */
typedef struct control_command
{
    control_command_type_t type; /**< Operation to execute. */
    bool synchronous;            /**< Whether a caller waits for completion. */
    union control_command_data
    {
        struct control_input_data
        {
            app_manager_key_t key;         /**< Logical input key. */
            app_manager_key_event_t event; /**< Logical input event. */
            uint32_t generation;           /**< HOME input sequence or zero. */
        } input;
        uint32_t touch_generation; /**< Touch-admission generation. */
        uint8_t brightness;        /**< Requested display brightness. */
        int32_t timeout_ms;        /**< Requested timeout in milliseconds. */
    } data;
} control_command_t;

extern app_manager_screen_ctrl_ops_t s_screen_ops;
extern app_manager_standby_ops_t s_standby_ops;
extern QueueHandle_t s_queue;
extern SemaphoreHandle_t s_call_lock;
extern SemaphoreHandle_t s_call_done;
extern SemaphoreHandle_t s_state_lock;
extern SemaphoreHandle_t s_task_ready;
extern SemaphoreHandle_t s_task_stopped;
extern TaskHandle_t s_task;
extern esp_err_t s_call_result;
extern app_manager_pm_state_t s_state;
extern int64_t s_last_activity_us;
extern int64_t s_screen_off_us;
extern int32_t s_dim_timeout_ms;
extern int32_t s_off_timeout_ms;
extern int32_t s_standby_timeout_ms;
extern uint8_t s_brightness;
extern atomic_int s_lifecycle_state;
extern atomic_uint s_active_accesses;
extern atomic_bool s_wake_pending;
extern atomic_uint s_power_pending_events;
extern atomic_bool s_power_pending_wake;
extern atomic_uint s_standby_generation;
extern atomic_uint s_standby_bound_epoch;
extern atomic_uint s_standby_system_generation;
extern atomic_uint s_standby_commit_epoch;
extern atomic_uint s_standby_commit_generation;
extern atomic_bool s_standby_commit_pending;
extern atomic_bool s_standby_abort_pending;
extern atomic_uint s_power_input_pending;
extern atomic_bool s_touch_pending;
extern atomic_uint s_touch_pending_generation;
extern atomic_uint s_touch_generation;
extern atomic_bool s_touch_activity_enabled;
extern atomic_bool s_touch_restore_pending;
extern atomic_bool s_input_wake_required;
extern atomic_uint s_home_sequence_state;
extern atomic_uint s_home_pending_generation;
extern atomic_bool s_home_inflight;
extern atomic_uint s_brightness_temp_value;
extern atomic_uint s_brightness_temp_generation;
extern atomic_bool s_brightness_temp_pending;
extern atomic_bool s_control_admitted;
extern bool s_power_wake_click_pending;
extern bool s_power_long_press_seen;
/** @brief Worker-owned wake classification for the queued HOME sequence. */
extern uint32_t s_home_worker_wake_generation;

/** @brief Start a new HOME input sequence and return its nonzero generation. */
static inline uint32_t _app_manager_control_home_sequence_begin(void)
{
    uint32_t state = atomic_load_explicit(&s_home_sequence_state,
                                          memory_order_acquire);
    for (;;)
    {
        uint32_t generation = (state & HOME_SEQUENCE_GENERATION_MASK) + 1U;
        if (generation > HOME_SEQUENCE_GENERATION_MASK)
        {
            generation = 1U;
        }
        if (atomic_compare_exchange_weak_explicit(
                    &s_home_sequence_state, &state, generation,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return generation;
        }
    }
}

/** @brief Close a HOME sequence unless a newer sequence already replaced it. */
static inline void _app_manager_control_home_sequence_close(
    uint32_t generation)
{
    if (generation == 0U)
    {
        return;
    }
    uint32_t state = atomic_load_explicit(&s_home_sequence_state,
                                          memory_order_acquire);
    while ((state & HOME_SEQUENCE_GENERATION_MASK) == generation)
    {
        uint32_t next_generation = generation + 1U;
        if (next_generation > HOME_SEQUENCE_GENERATION_MASK)
        {
            next_generation = 1U;
        }
        if (atomic_compare_exchange_weak_explicit(
                    &s_home_sequence_state, &state, next_generation,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return;
        }
    }
}

/** @brief Mark a still-current HOME sequence as display wake input. */
static inline void _app_manager_control_home_sequence_mark_wake(
    uint32_t generation)
{
    if (generation == 0U)
    {
        return;
    }
    uint32_t state = atomic_load_explicit(&s_home_sequence_state,
                                          memory_order_acquire);
    while ((state & HOME_SEQUENCE_GENERATION_MASK) == generation &&
            (state & HOME_SEQUENCE_WAKE_MASK) == 0U)
    {
        const uint32_t wake_state = state | HOME_SEQUENCE_WAKE_MASK;
        if (atomic_compare_exchange_weak_explicit(
                    &s_home_sequence_state, &state, wake_state,
                    memory_order_acq_rel, memory_order_acquire))
        {
            return;
        }
    }
}

/** @brief Report whether one HOME generation was classified as wake input. */
static inline bool _app_manager_control_home_sequence_is_wake(
    uint32_t generation)
{
    const uint32_t state = atomic_load_explicit(&s_home_sequence_state,
                           memory_order_acquire);
    return generation != 0U &&
           (state & HOME_SEQUENCE_GENERATION_MASK) == generation &&
           (state & HOME_SEQUENCE_WAKE_MASK) != 0U;
}

/** @brief Report whether the current HOME sequence still represents wake. */
static inline bool _app_manager_control_home_wake_is_pending(void)
{
    return (atomic_load_explicit(&s_home_sequence_state,
                                 memory_order_acquire) &
            HOME_SEQUENCE_WAKE_MASK) != 0U;
}

/** @brief Invalidate a temporary brightness preview before a final command. */
static inline void _app_manager_control_clear_brightness_preview(void)
{
    atomic_fetch_add_explicit(&s_brightness_temp_generation, 1U,
                              memory_order_acq_rel);
    atomic_store_explicit(&s_brightness_temp_pending, false,
                          memory_order_release);
}

/**
 * @brief Admit one producer while application control is ready.
 *
 * @return true when access was acquired; false during teardown or startup.
 */
bool IRAM_ATTR app_manager_control_access_begin(void);

/** @brief Release one producer admission acquired by access_begin(). */
void IRAM_ATTR app_manager_control_access_end(void);

/**
 * @brief Apply a brightness value on the LVGL worker.
 *
 * @param brightness is the hardware brightness value.
 * @param persistent selects the persistent or temporary BSP operation.
 *
 * @return ESP_OK when applied, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_control_apply_brightness(uint8_t brightness,
        bool persistent);

/**
 * @brief Check and bind one system-standby commit generation.
 *
 * @param generation identifies the system-PM request.
 *
 * @return true when the current control state may commit to standby.
 */
bool app_manager_control_standby_commit_ready(uint32_t generation);

/**
 * @brief Run the application-control worker task.
 * @param arg is unused and must be NULL.
 */
void app_manager_control_task(void *arg);

/**
 * @brief Queue one asynchronous command.
 *
 * @param command contains the operation and its payload.
 *
 * @return ESP_OK when accepted, otherwise an ESP-IDF error.
 */
esp_err_t app_manager_control_post(control_command_t command);

/**
 * @brief Execute one command and wait for its result.
 *
 * @param command contains the operation and its payload.
 *
 * @return Command result or an admission/queue error.
 */
esp_err_t app_manager_control_call(control_command_t command);

#endif /* __APP_MANAGER_PM_INTERNAL_H__ */
