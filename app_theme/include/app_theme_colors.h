/**
 * @brief  Emissive AMOLED colour language — the single source of raw colour
 *         values for the panel UI shell and every application page.
 *
 * The SH8601A is self-emissive: #000000 turns pixels fully off, so the canvas
 * is VOID and hierarchy is expressed with sparse light (tonal surface steps,
 * tinted selection, brighter press states) instead of shadows or blur. Keep
 * average picture level low; spend luminance only on information. Static
 * chrome must not hold large near-white areas (differential ageing).
 *
 * Ratios are WCAG contrast on the noted background; body ink stays below
 * pure white to limit bloom halos around glyph edges. Tints are accents
 * pre-mixed at ~16 % over VOID so selection fills never need alpha blending.
 */

#ifndef __APP_THEME_COLORS_H__
#define __APP_THEME_COLORS_H__

/* Canvas and tonal surface ramp (lightness ascends with elevation). */
#define APP_THEME_COLOR_VOID        0x000000 /**< Screen; pixels off. */
#define APP_THEME_COLOR_PLUME       0x161B20 /**< L1 card / grouped list. */
#define APP_THEME_COLOR_SHEET       0x1A2026 /**< Modal sheet; above PLUME. */
#define APP_THEME_COLOR_PLUME_HI    0x222A31 /**< L2 pressed / secondary fill. */
#define APP_THEME_COLOR_TRACK       0x2C353D /**< L3 ring/slider/switch track. */
#define APP_THEME_COLOR_SUNKEN      0x0B0F12 /**< Well below PLUME. */
#define APP_THEME_COLOR_HAIRLINE    0x242C33 /**< 1 px dividers, inert glyph. */

/* Scrim behind modal surfaces; relies on opacity at the use site. */
#define APP_THEME_COLOR_SCRIM       0x000000 /**< Dimming layer = VOID with alpha. */

/* Ink ramp on VOID / PLUME. */
#define APP_THEME_COLOR_INK         0xEAF0F2 /**< Primary text; 18.2:1 / 15.1:1. */
#define APP_THEME_COLOR_INK_SOFT    0xA9B6BC /**< Secondary text; 10.1:1 / 8.3:1. */
#define APP_THEME_COLOR_INK_FAINT   0x76838A /**< Tertiary labels, units; 5.4:1 / 4.4:1. */
#define APP_THEME_COLOR_ON_ACCENT   0x04141A /**< Text/glyph drawn on an accent fill. */

/* Accents — saturated, narrow set; hue alone must never carry meaning. */
#define APP_THEME_COLOR_AZURE       0x43C6DB /**< Primary action / active; 10.3:1. */
#define APP_THEME_COLOR_AMBER       0xFFB454 /**< Time, pending, solar; 11.9:1. */
#define APP_THEME_COLOR_MINT        0x4FDD8B /**< Success, charging, level OK; 12.0:1. */
#define APP_THEME_COLOR_CORAL       0xFF6B6B /**< Destructive, fault; 7.6:1. */
#define APP_THEME_COLOR_AZURE_HI    0x7DD8E7 /**< Pressed primary fill; lightens. */

/* Accent tints (accent at ~16 % over VOID) for selected / badge fills. */
#define APP_THEME_COLOR_AZURE_TINT  0x0B2024 /**< Selected chip, today cell. */
#define APP_THEME_COLOR_AMBER_TINT  0x291D0E /**< Pending / advisory pill. */
#define APP_THEME_COLOR_MINT_TINT   0x0D2416 /**< Success pill. */
#define APP_THEME_COLOR_CORAL_TINT  0x291111 /**< Fault / destructive pill. */

#endif /* __APP_THEME_COLORS_H__ */
