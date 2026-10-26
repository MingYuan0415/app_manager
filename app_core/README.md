**English** | [简体中文](README_ZH.md)

# app_core

`app_core` is the core component of the App Manager: static App registry,
fixed-capacity navigation gateway, Page lifecycle and residence policy,
standalone Screen transition engine, Sys Layer edge back, unified UI
mailbox/dispatcher, and display and PM cooperation.

See the parent [../README.md](../README.md) for the full design, invariants and
acceptance boundary; this file only summarizes this module's own public API.

## Public API

Public header `include/app_manager.h` (which also aggregates
`app_manager_types.h`, `app_manager_ids.h` and `app_theme.h`):

- Lifecycle: `app_manager_init()` (full configuration lives in
  `app_manager_config.h`, used only by the assembly layer),
  `app_manager_deinit()`, `app_manager_is_initialized()`.
- Navigation: `app_manager_navigate()` (non-UI worker, waits for a stable
  foreground), `app_manager_navigate_async()` (any context, the completion
  executes exactly once on the UI worker).
- Queries: `app_manager_is_actived()`, `app_manager_get_active_app_id()`,
  `app_page_is_actived()`, `app_manager_is_page_present()`,
  `app_manager_is_all_closed()`, `app_manager_get_running_apps()`,
  `app_manager_get_recent_tasks()`, `app_manager_is_transitioning()`.
- System surfaces: `app_manager_control_center_show()` /
  `app_manager_control_center_hide()` /
  `app_manager_control_center_is_visible()` /
  `app_manager_control_center_set_enabled()` /
  `app_manager_control_center_is_enabled()`. The task switcher is driven by
  navigation system commands; its `show()` / `is_visible()` exist only in
  `src/app_manager_task_switcher.h`.
- Edge back: `app_manager_back_gesture_set_enabled()` /
  `app_manager_back_gesture_is_enabled()`.
- Page context accessors: `app_manager_this_page_memory()` /
  `app_manager_this_page_userdata()` / `app_manager_this_page_screen()` /
  `app_manager_this_page_arguments()` /
  `app_manager_this_page_report_cleanup_error()`.
- Screen control: `app_manager_screen_suspend()` /
  `app_manager_screen_resume()` / `app_manager_screen_is_suspended()`,
  `app_manager_display_commit_initial()`, `app_manager_startup_commit()`,
  `app_manager_screen_set_brightness()` /
  `app_manager_screen_set_brightness_temp()` /
  `app_manager_screen_get_brightness()`,
  `app_manager_factory_reset_persisted_state()`.
- Input and PM: `app_manager_input_inject()`, plus the `app_manager_pm_*`
  requests, notifications, state, timeout/standby delays and the standby
  commit hook.
- Registry: `app_manager_builtin_discover()` /
  `app_manager_builtin_list_open()` / `app_manager_builtin_list_get_next()`;
  export macros `APP_MANAGER_APP_EXPORT` / `APP_MANAGER_APP_EXPORT_META`.
- UI and resources: `app_manager_ui_post()` / `app_manager_ui_call()` /
  `app_manager_get_ui_dispatch_fn()`, `app_manager_get_font()`,
  `app_manager_get_image()`.

`include/app_manager_types.h` defines the App/Page descriptor, route,
definition, Typed Blob, lifecycle messages and transition types;
`include/app_manager_ids.h` provides the stable App IDs;
`include/app_manager_display_diagnostics.h` is a development-only display
benchmark API.

## Configuration and Dependencies

- `idf_component.yml`: `idf: ">=5.0"`, `espressif/esp_lvgl_adapter: "0.7.3"`.
- CMake: `REQUIRES app_theme bsp lvgl__lvgl esp_common`; `PRIV_REQUIRES espressif__esp_lvgl_adapter esp_lcd esp_driver_gpio mt_log nvs_flash nv_storage esp_timer freertos heap esp_mmap_assets`.
- Product capacity and behavior are determined by the `CONFIG_APP_MANAGER_*`
  symbols in `Kconfig`; the assembly is provided by `main` through
  `app_manager_config_t`.
- `linker.lf` retains the single `.app_manager_apps` App descriptor section.

## Test Boundary

- `tests/host` (see its README) covers the registry, nav model, navigation
  gateway, lifecycle, mailbox, PM and adapter; presentation transitions are
  covered by `tests/integration`.
- On-device display, touch, DMA, power consumption and performance are covered
  by `tests/integration` and board acceptance; host tests do not prove these.
