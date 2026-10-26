[English](README.md) | **简体中文**

# app_theme

`app_theme` 提供七档语义字体角色与 Emissive AMOLED 颜色常量，是 App 与 Page 共享的字体和颜色单一来源。

完整设计见上级 [../README_ZH.md](../README_ZH.md)；本文件只概述本模块自身的公共 API。

## 对外 API

`include/app_theme.h`：

- `app_theme_init()` / `app_theme_deinit()`：复位或释放字体表。
- `app_theme_set_font()` / `app_theme_get_font()`：按 `app_theme_font_id_t` 角色（BODY / SMALL / HEAD / BIGL / HUGE / TITLE / DISPLAY）设置或获取已加载字体。
- `APP_THEME_FONT_SIZES`：默认字号表 `{24, 20, 30, 36, 56, 64, 96}`：BODY 24（行标题、按钮）、SMALL 20（说明、单位）、HEAD 30（卡片数值、面板标题）、BIGL 36（页标题）、HUGE 56（计时/结果数字）、TITLE 64（天气温度、时钟）、DISPLAY 96（主页时钟）。

`include/app_theme_colors.h`：AMOLED 颜色宏，包括画布 `APP_THEME_COLOR_VOID`；按亮度递增的表面 `PLUME`（卡片）/ `SHEET`（底部面板）/ `PLUME_HI`（按下、次要填充）/ `TRACK`（环、滑条、开关轨道）及 `SUNKEN` / `HAIRLINE`；文字 `INK` / `INK_SOFT` / `INK_FAINT` / `ON_ACCENT`；强调色 `AZURE`（按下态 `AZURE_HI`）/ `AMBER` / `MINT` / `CORAL`，以及约 16% 预混到 VOID 的 `*_TINT`，用于选中与状态底色，无需 alpha 混合。

## 配置与依赖

- `idf_component.yml`：`idf: ">=5.0"`、`espressif/esp_lvgl_adapter: "0.7.3"`。
- CMake：`REQUIRES lvgl__lvgl`。
- `resource_manifest.cmake` 将 `assets/font.ttf` 注册为构建资源记录，由装配层加载并绑定到字体角色。

## 测试边界

- 本组件无 `tests/host`；字体加载与颜色呈现由 App Manager 宿主/跨层测试和上板显示验收覆盖。
