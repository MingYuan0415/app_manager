/** @file Minimal LVGL declarations used by App Manager host tests. */
#ifndef __HOST_LVGL_H__
#define __HOST_LVGL_H__

#include <stdbool.h>
#include <stdint.h>

/** @brief Opaque fake LVGL object. */
typedef struct _lv_obj_t lv_obj_t;
/** @brief Opaque fake LVGL font. */
typedef struct _lv_font_t lv_font_t;
/** @brief Opaque fake LVGL display. */
typedef struct _lv_display_t lv_display_t;
/** @brief Opaque fake LVGL input device. */
typedef struct _lv_indev_t lv_indev_t;
/** @brief Opaque fake LVGL event. */
typedef struct _lv_event_t lv_event_t;
/** @brief Fake LVGL event code. */
typedef int32_t lv_event_code_t;
/** @brief Fake LVGL event callback. */
typedef void (*lv_event_cb_t)(lv_event_t *event);

/** @brief Fake inclusive LVGL rectangle. */
typedef struct lv_area_t
{
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
} lv_area_t;

enum
{
    LV_EVENT_ALL = 0,
    LV_EVENT_RENDER_START,
    LV_EVENT_RENDER_READY,
    LV_EVENT_FLUSH_START,
    LV_EVENT_FLUSH_FINISH,
    LV_EVENT_FLUSH_WAIT_START,
    LV_EVENT_FLUSH_WAIT_FINISH,
};

/** @brief Add one fake display event callback. */
void lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback,
                             lv_event_code_t filter, void *user_data);
/** @brief Return the fake display event callback count. */
uint32_t lv_display_get_event_count(lv_display_t *display);
/** @brief Remove matching fake display event callbacks. */
uint32_t lv_display_remove_event_cb_with_user_data(lv_display_t *display,
        lv_event_cb_t callback, void *user_data);
/** @brief Return a fake event's code. */
lv_event_code_t lv_event_get_code(lv_event_t *event);
/** @brief Return a fake event's parameter. */
void *lv_event_get_param(lv_event_t *event);

/** @brief Color formats exercised by the App Manager display adapter. */
typedef enum
{
    LV_COLOR_FORMAT_UNKNOWN = 0,
    LV_COLOR_FORMAT_RGB565 = 0x12,
    LV_COLOR_FORMAT_RGB565A8 = 0x14,
    LV_COLOR_FORMAT_RGB565_SWAPPED = 0x1B,
} lv_color_format_t;

/** @brief Minimal LVGL result values used by draw-buffer APIs. */
typedef enum
{
    LV_RESULT_OK = 0,
    LV_RESULT_INVALID,
} lv_result_t;

#define LV_IMAGE_HEADER_MAGIC 0x19U
#define LV_IMAGE_FLAGS_COMPRESSED 0x0008U

/** @brief Minimal LVGL image header used by mmap resource tests. */
typedef struct lv_image_header
{
    uint32_t magic : 8;
    uint32_t cf : 8;
    uint32_t flags : 16;
    uint32_t w : 16;
    uint32_t h : 16;
    uint32_t stride : 16;
    uint32_t reserved_2 : 16;
} lv_image_header_t;

/** @brief Minimal persistent LVGL image descriptor. */
typedef struct lv_image_dsc
{
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
    const void *reserved_2;
} lv_image_dsc_t;

/** @brief Byte alignment required for draw-buffer data. */
#define LV_DRAW_BUF_ALIGN 16U

/** @brief Minimal fake LVGL draw buffer used by recent-tasks previews. */
typedef struct lv_draw_buf
{
    lv_image_header_t header;
    uint32_t data_size;
    void *data;
} lv_draw_buf_t;

/** @brief Initialize a fake draw buffer. */
lv_result_t lv_draw_buf_init(lv_draw_buf_t *draw_buf, uint32_t width,
                             uint32_t height, lv_color_format_t color_format,
                             uint32_t stride, void *data, uint32_t data_size);
/** @brief Return the byte stride for one scanline of a color format. */
uint32_t lv_draw_buf_width_to_stride(uint32_t width,
                                     lv_color_format_t color_format);
/** @brief Flush the data cache of one fake draw buffer. */
void lv_draw_buf_flush_cache(const lv_draw_buf_t *draw_buf,
                             const lv_area_t *area);

/** @brief Return the active screen of a fake display. */
lv_obj_t *lv_display_get_screen_active(lv_display_t *display);
/** @brief Set the pixel format produced for a fake display. */
void lv_display_set_color_format(lv_display_t *display,
                                 lv_color_format_t color_format);
/** @brief Return whether the active flush is the frame's final area. */
bool lv_display_flush_is_last(lv_display_t *display);
/** @brief Mark a fake object for redraw. */
void lv_obj_invalidate(lv_obj_t *obj);
/** @brief Run one synchronous fake refresh. */
void lv_refr_now(lv_display_t *display);
/** @brief Enable or disable a fake input device. */
void lv_indev_enable(lv_indev_t *indev, bool enable);
/** @brief Reset the fake input device state. */
void lv_indev_reset(lv_indev_t *indev, lv_obj_t *obj);

#endif /* __HOST_LVGL_H__ */
