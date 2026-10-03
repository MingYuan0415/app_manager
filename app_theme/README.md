# app_theme

`app_theme` 提供六档语义字体角色与 Emissive AMOLED 颜色常量，是 App 与 Page 共享的字体和颜色单一来源。

完整设计见上级 [../README.md](../README.md)；本文件只概述本模块自身的公共 API。

## 对外 API

`include/app_theme.h`：

- `app_theme_init()` / `app_theme_deinit()`：复位或释放字体表。
- `app_theme_set_font()` / `app_theme_get_font()`：按 `app_theme_font_id_t` 角色（BODY / SMALL / HEAD / BIGL / HUGE / TITLE）设置或获取已加载字体。
- `APP_THEME_FONT_SIZES`：默认字号表 `{18, 22, 28, 36, 48, 54}`。

`include/app_theme_colors.h`：AMOLED 颜色宏，包括 `APP_THEME_COLOR_VOID`、`PLUME` / `PLUME_HI` / `SUNKEN` / `HAIRLINE`、`INK` / `INK_SOFT` / `ON_ACCENT`，以及强调色 `AZURE` / `AMBER` / `MINT` / `CORAL`。

## 配置与依赖

- `idf_component.yml`：`idf: ">=5.0"`、`espressif/esp_lvgl_adapter: "0.7.2"`。
- CMake：`REQUIRES lvgl__lvgl`。
- `resource_manifest.cmake` 将 `assets/font.ttf` 注册为构建资源记录，由装配层加载并绑定到字体角色。

## 测试边界

- 本组件无 `tests/host`；字体加载与颜色呈现由 App Manager 宿主/跨层测试和上板显示验收覆盖。