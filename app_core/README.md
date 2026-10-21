# app_core

`app_core` 是 App Manager 的核心组件：静态 App registry、固定容量导航 gateway、页面生命周期与驻留策略、独立 Screen 转换引擎、Sys Layer 边缘返回、统一 UI mailbox/dispatcher，以及显示与 PM 协作。

完整设计、不变量与验收边界见上级 [../README.md](../README.md)；本文件只概述本模块自身的公共 API。

## 对外 API

公共头 `include/app_manager.h`（同时聚合 `app_manager_types.h`、`app_manager_ids.h` 和 `app_theme.h`）：

- 生命周期：`app_manager_init()`（完整配置在 `app_manager_config.h`，仅装配层使用）、`app_manager_deinit()`、`app_manager_is_initialized()`。
- 导航：`app_manager_navigate()`（非 UI worker，等待稳定前台）、`app_manager_navigate_async()`（任意上下文，completion 在 UI worker 上恰好执行一次）。
- 查询：`app_manager_is_actived()`、`app_manager_get_active_app_id()`、`app_page_is_actived()`、`app_manager_is_page_present()`、`app_manager_is_all_closed()`、`app_manager_get_running_apps()`、`app_manager_get_recent_tasks()`、`app_manager_is_transitioning()`。
- 系统表面：`app_manager_control_center_show()` / `app_manager_control_center_hide()` / `app_manager_control_center_is_visible()` / `app_manager_control_center_set_enabled()` / `app_manager_control_center_is_enabled()`。任务切换器由导航系统命令驱动，其 `show()` / `is_visible()` 只存在于 `src/app_manager_task_switcher.h`。
- 边缘返回：`app_manager_back_gesture_set_enabled()` / `app_manager_back_gesture_is_enabled()`。
- 页面上下文访问器：`app_manager_this_page_memory()` / `app_manager_this_page_userdata()` / `app_manager_this_page_screen()` / `app_manager_this_page_arguments()` / `app_manager_this_page_report_cleanup_error()`。
- Screen 控制：`app_manager_screen_suspend()` / `app_manager_screen_resume()` / `app_manager_screen_is_suspended()`、`app_manager_display_commit_initial()`、`app_manager_startup_commit()`、`app_manager_screen_set_brightness()` / `app_manager_screen_set_brightness_temp()` / `app_manager_screen_get_brightness()`、`app_manager_factory_reset_persisted_state()`。
- 输入与 PM：`app_manager_input_inject()`，以及 `app_manager_pm_*` 请求、通知、状态、超时/待机延迟与 standby commit 钩子。
- Registry：`app_manager_builtin_discover()` / `app_manager_builtin_list_open()` / `app_manager_builtin_list_get_next()`；导出宏 `APP_MANAGER_APP_EXPORT` / `APP_MANAGER_APP_EXPORT_META`。
- UI 与资源：`app_manager_ui_post()` / `app_manager_ui_call()` / `app_manager_get_ui_dispatch_fn()`、`app_manager_get_font()`、`app_manager_get_image()`。

`include/app_manager_types.h` 定义 App/Page descriptor、route、definition、Typed Blob、生命周期消息与转场类型；`include/app_manager_ids.h` 提供稳定 App ID；`include/app_manager_display_diagnostics.h` 为仅开发用的显示基准 API。

## 配置与依赖

- `idf_component.yml`：`idf: ">=5.0"`、`espressif/esp_lvgl_adapter: "0.7.3"`。
- CMake：`REQUIRES app_theme bsp lvgl__lvgl esp_common`；`PRIV_REQUIRES espressif__esp_lvgl_adapter esp_lcd esp_driver_gpio mt_log nvs_flash nv_storage esp_timer freertos heap esp_mmap_assets`。
- 产品容量与行为由 `Kconfig` 中的 `CONFIG_APP_MANAGER_*` 符号决定；装配由 `main` 通过 `app_manager_config_t` 提供。
- `linker.lf` 保留唯一的 `.app_manager_apps` App descriptor 段。

## 测试边界

- `tests/host`（见其 README）覆盖 registry、nav model、navigation gateway、lifecycle、mailbox、PM 与 adapter；presentation 转场由 `tests/integration` 覆盖。
- 真机显示、触摸、DMA、功耗与性能由 `tests/integration` 和上板验收覆盖，宿主测试不证明这些。