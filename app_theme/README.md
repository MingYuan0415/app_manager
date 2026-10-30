**English** | [简体中文](README_ZH.md)

# app_theme

`app_theme` provides seven semantic font roles and the Emissive AMOLED color
constants; it is the single source of fonts and colors shared by Apps and
Pages.

See the parent [../README.md](../README.md) for the full design; this file only
summarizes this module's own public API.

## Public API

`include/app_theme.h`:

- `app_theme_init()` / `app_theme_deinit()`: reset or release the font table.
- `app_theme_lvgl_attach()` / `app_theme_lvgl_detach()`: attach or release the
  single project LVGL theme after the display exists and fonts are loaded. The
  display must outlive the detach call; a second display is rejected while the
  first one is attached.
- `app_theme_set_font()` / `app_theme_get_font()`: set or get the loaded font by
  `app_theme_font_id_t` role (BODY / SMALL / HEAD / BIGL / HUGE / TITLE /
  DISPLAY).
- `APP_THEME_FONT_SIZES`: the default size table `{24, 20, 30, 36, 56, 64, 96}`:
  BODY 24 (row titles, buttons), SMALL 20 (captions, units), HEAD 30 (card
  values, panel titles), BIGL 36 (page titles), HUGE 56 (timer/result digits),
  TITLE 64 (weather temperature, clock), DISPLAY 96 (home clock).

`include/app_theme_colors.h`: AMOLED color macros, including the canvas
`APP_THEME_COLOR_VOID`; surfaces ordered by increasing brightness `PLUME`
(cards) / `SHEET` (bottom sheets) / `PLUME_HI` (pressed, secondary fill) /
`TRACK` (rings, sliders, switch tracks) plus `SUNKEN` / `HAIRLINE`; text `INK` /
`INK_SOFT` / `INK_FAINT` / `ON_ACCENT`; accents `AZURE` (pressed state
`AZURE_HI`) / `AMBER` / `MINT` / `CORAL`, plus the `*_TINT` variants premixed
about 16% into VOID for selected and status backgrounds, requiring no alpha
blending.

## Configuration and Dependencies

- `idf_component.yml`: `idf: ">=5.0"`, `espressif/esp_lvgl_adapter: "0.7.3"`;
  the root project pins `lvgl/lvgl` 9.6 in `dependencies.lock`, and the local
  theme copy checks the version at compile time.
- CMake: `REQUIRES lvgl__lvgl`.
- `resource_manifest.cmake` registers `assets/font.ttf` as a build resource
  record, which the assembly layer loads and binds to the font roles.
- Built-in LVGL themes are disabled in `sdkconfig.defaults`. `src/app_theme_lvgl.c`
  compiles the project-owned `src/lv_theme_default_local.inc` copy of the pinned
  LVGL 9.6 default theme with a theme-only widget mask, so widget Kconfig options
  remain enabled while unused widget classes are dropped by `--gc-sections`.
  The local copy is pinned to LVGL 9.6 and is checked at compile time.
- `src/lv_theme_default_local.inc` is derived from LVGL's MIT-licensed default
  theme; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the upstream
  notice.

## Test Boundary

- This component has no `tests/host`; font loading and color presentation are
  covered by the App Manager host/cross-layer tests and board display
  acceptance. The App Manager adapter host test also covers LVGL theme
  attach/detach ordering and rollback.
