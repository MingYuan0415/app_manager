# MicroTech App Manager

本仓库提供 MicroTech 的应用运行时，由 `app_core` 与 `app_theme` 两个 ESP-IDF 组件组成。它负责在 LVGL 工作线程上管理应用页面，并协调显示、输入和系统待机；具体应用界面不属于本仓库。

## 目录结构

- `app_core/include/`：应用生命周期、页面导航、UI 调度、电源管理和初始化配置等公共 API。
- `app_core/src/`：页面栈、内置应用发现、UI mailbox、LVGL 适配及应用级电源状态机。
- `app_core/linker.lf`：保留并界定 `.app_builtin_apps` 内置应用描述符段。
- `app_core/tests/host/`：使用 FreeRTOS、LVGL 和硬件 shim 编译生产源文件的宿主测试。
- `app_theme/`：六档主题字体角色及其注册、查询接口。

## 主要能力

- 通过 `BUILTIN_APP_EXPORT` 发现静态应用，管理根页面、子页面、私有内存及 `ONSTART` 到 `ONSTOP` 生命周期。
- 通过 `app_manager_ui_post()` 和 `app_manager_ui_call()` 将 UI 操作串行化到 LVGL 工作线程。
- 初始化显示、触摸、资源文件系统和字体，并处理刷新栅栏、熄屏及恢复顺序。
- 通过上层注入的 screen、input、standby 操作协调亮度、空闲熄屏、按键/触摸唤醒和待机提交。

页面生命周期按资源所有权分工：`ONSTART` 只初始化长期保留的非可视状态，不创建 LVGL 对象、主动刷新或启动前台定时器/订阅；`ONRESUME` 创建可见 UI 并挂接前台资源；`ONPAUSE` 销毁 UI、停止定时器、退订事件并关闭活动会话；`ONSTOP` 释放页面实例剩余资源。未完成的清理必须保留未释放句柄，并通过 `app_manager_this_page_report_cleanup_error()` 上报，以便下次转换重试。

熄屏只暂停当前前台页面，唤醒后恢复同一页面，应用栈和页面私有内存保持不变。`PAUSING/PAUSED` 期间 mailbox 不提供页面级拓扑查询：页面 userdata 返回 `NULL`，页面存在性返回 `false`，这些默认值不表示实例已销毁。成功进入 `PAUSED` 时会保存运行应用数，`app_manager_get_running_apps()` 与 `app_manager_is_all_closed()` 使用该快照；关闭过程中则返回保守默认值，实际页面拓扑需在唤醒后查询。

公共应用代码通常只包含 `app_manager.h`；创建运行时的装配层还需包含 `app_manager_config.h`，填充有效的 panel、panel IO、touch 以及三组操作接口后调用 `app_manager_init()`。

## ESP-IDF 集成

在项目根 `CMakeLists.txt` 的 `project()` 之前加入仓库目录：

```cmake
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/app_manager"
)
```

使用方组件声明 `PRIV_REQUIRES app_core app_theme`。`app_core` 要求 ESP-IDF 5.0 或更高版本，并由组件管理器解析 `espressif/esp_lvgl_adapter`；上层工程还必须提供其 `CMakeLists.txt` 中列出的 `bsp`、`mt_log`、`nv_storage` 和 LVGL 等组件。启用 adapter 文件系统和 FreeType 时，需提供标签为 `res` 的资源分区、配置盘符，并在分区中打包 `font.ttf`。

## 宿主测试

```sh
cmake -S app_core/tests/host -B /tmp/mt-app-manager-host -G Ninja
cmake --build /tmp/mt-app-manager-host
ctest --test-dir /tmp/mt-app-manager-host --output-on-failure
```

设置 `-DAPP_MANAGER_SANITIZER=address` 可运行 ASan/UBSan，设置为 `thread` 可运行 TSan。测试覆盖 mailbox、生命周期、应用控制和 adapter 的失败回滚/重试；显示、电源与触摸改动仍须在 ESP32-S3 上验证。

## 设计与修改边界

LVGL 对象只能在 UI 工作线程中访问，硬件回调不得绕过注入接口直接耦合具体 BSP。资源释放失败应保留所有权并允许重试，页面 handler 可用 `app_manager_this_page_report_cleanup_error()` 上报。遵循主工程 `doc/code-style.md`，保持单一出口和逆序清理；不要修改 ESP-IDF 或 `managed_components/`，依赖问题优先通过版本升级或本仓库适配解决。

## 许可证

本项目采用 [MIT License](LICENSE)。
