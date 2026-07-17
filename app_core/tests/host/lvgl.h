/** @file Minimal LVGL declarations used by App Manager host tests. */
#ifndef __HOST_LVGL_H__
#define __HOST_LVGL_H__

#include <stdbool.h>

/** @brief Opaque fake LVGL object. */
typedef struct _lv_obj_t lv_obj_t;
/** @brief Opaque fake LVGL font. */
typedef struct _lv_font_t lv_font_t;
/** @brief Opaque fake LVGL display. */
typedef struct _lv_display_t lv_display_t;
/** @brief Opaque fake LVGL input device. */
typedef struct _lv_indev_t lv_indev_t;

/** @brief Return the active screen of a fake display. */
lv_obj_t *lv_display_get_screen_active(lv_display_t *display);
/** @brief Mark a fake object for redraw. */
void lv_obj_invalidate(lv_obj_t *obj);
/** @brief Run one synchronous fake refresh. */
void lv_refr_now(lv_display_t *display);
/** @brief Enable or disable a fake input device. */
void lv_indev_enable(lv_indev_t *indev, bool enable);
/** @brief Reset the fake input device state. */
void lv_indev_reset(lv_indev_t *indev, lv_obj_t *obj);

#endif /* __HOST_LVGL_H__ */
