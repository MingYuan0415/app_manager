/**
 * @brief  Emissive AMOLED colour language — the single source of raw colour
 *         values for the panel UI shell and every application page.
 *
 * The SH8601A is self-emissive: #000000 turns pixels fully off, so the canvas
 * is VOID and hierarchy is expressed with sparse light (hairlines, tinted
 * selection, brighter press states) instead of grey elevation tiles. Keep
 * average picture level low; spend luminance only on information. Static
 * chrome must not hold large near-white areas (differential ageing).
 *
 * Ratios are WCAG contrast on the noted background; body ink stays below
 * pure white to limit bloom halos around glyph edges.
 */

#ifndef __APP_THEME_COLORS_H__
#define __APP_THEME_COLORS_H__

/* Canvas and sparse dark layers (two elevation steps maximum). */
#define APP_THEME_COLOR_VOID        0x000000 /**< Screen; pixels off. */
#define APP_THEME_COLOR_PLUME       0x14191E /**< Card / row tile. */
#define APP_THEME_COLOR_PLUME_HI    0x1E262D /**< Pressed; lightens, never darkens. */
#define APP_THEME_COLOR_SUNKEN      0x0A0E11 /**< Icon-chip well below PLUME. */
#define APP_THEME_COLOR_HAIRLINE    0x232C33 /**< 1 px dividers, inert glyph. */

/* Ink ramp on VOID / PLUME. */
#define APP_THEME_COLOR_INK         0xEAF0F2 /**< Primary text; 18.2:1 / 15.4:1. */
#define APP_THEME_COLOR_INK_SOFT    0xA9B6BC /**< Secondary text; 10.1:1 / 8.5:1. */
#define APP_THEME_COLOR_ON_ACCENT   0x04141A /**< Text/glyph drawn on an accent fill. */

/* Accents — saturated, narrow set; hue alone must never carry meaning. */
#define APP_THEME_COLOR_AZURE       0x43C6DB /**< Primary action / active; 10.3:1. */
#define APP_THEME_COLOR_AMBER       0xFFB454 /**< Time, pending, solar; 11.9:1. */
#define APP_THEME_COLOR_MINT        0x4FDD8B /**< Success, charging, level OK; 12.0:1. */
#define APP_THEME_COLOR_CORAL       0xFF6B6B /**< Destructive, fault; 7.6:1. */

#endif /* __APP_THEME_COLORS_H__ */
