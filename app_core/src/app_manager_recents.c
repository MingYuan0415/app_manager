#define DBG_TAG "app_recents"
#define DBG_LVL DBG_INFO
#include "mt_log.h"

#include "app_manager_recents.h"

#ifndef HOST_TEST
    #include "esp_heap_caps.h"
#else
    #include <stdlib.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APP_MANAGER_RECENTS_PREVIEW_BYTES \
    (APP_MANAGER_RECENT_PREVIEW_WIDTH * 2U * \
     APP_MANAGER_RECENT_PREVIEW_HEIGHT)

typedef struct app_manager_recents_entry
{
    bool in_use;
    bool valid;
    uint32_t order;
    char app_id[APP_MANAGER_ID_BYTES];
    void *data;
    lv_draw_buf_t draw_buf;
} app_manager_recents_entry_t;

static app_manager_recents_entry_t
s_entries[APP_MANAGER_MAX_RECENT_TASKS];
static bool s_initialized;
static uint32_t s_order;

static void *_recents_alloc(void)
{
#ifndef HOST_TEST
    return heap_caps_aligned_calloc(
               LV_DRAW_BUF_ALIGN, 1U, APP_MANAGER_RECENTS_PREVIEW_BYTES,
               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    (void)LV_DRAW_BUF_ALIGN;
    return calloc(1U, APP_MANAGER_RECENTS_PREVIEW_BYTES);
#endif
}

static void _recents_free(void *data)
{
#ifndef HOST_TEST
    heap_caps_free(data);
#else
    free(data);
#endif
}

static app_manager_recents_entry_t *_recents_find(const char *app_id)
{
    for (size_t index = 0U; index < APP_MANAGER_MAX_RECENT_TASKS; index++)
    {
        if (s_entries[index].in_use &&
                strcmp(s_entries[index].app_id, app_id) == 0)
        {
            return &s_entries[index];
        }
    }
    return NULL;
}

static app_manager_recents_entry_t *_recents_allocate(void)
{
    size_t empty = SIZE_MAX;
    size_t oldest = 0U;
    for (size_t index = 0U; index < APP_MANAGER_MAX_RECENT_TASKS; index++)
    {
        if (!s_entries[index].in_use)
        {
            if (empty == SIZE_MAX)
            {
                empty = index;
            }
            continue;
        }
        if (s_entries[index].order < s_entries[oldest].order)
        {
            oldest = index;
        }
    }
    if (empty != SIZE_MAX)
    {
        return &s_entries[empty];
    }
    return &s_entries[oldest];
}

esp_err_t app_manager_recents_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }
    memset(s_entries, 0, sizeof(s_entries));
    s_order = 0U;
    for (size_t index = 0U; index < APP_MANAGER_MAX_RECENT_TASKS; index++)
    {
        app_manager_recents_entry_t *entry = &s_entries[index];
        entry->data = _recents_alloc();
        if (entry->data == NULL)
        {
            LOG_W("preview slot %u unavailable", (unsigned)index);
            app_manager_recents_deinit();
            return ESP_ERR_NO_MEM;
        }
        const uint32_t stride = lv_draw_buf_width_to_stride(
                                    APP_MANAGER_RECENT_PREVIEW_WIDTH,
                                    LV_COLOR_FORMAT_RGB565);
        if (lv_draw_buf_init(&entry->draw_buf,
                             APP_MANAGER_RECENT_PREVIEW_WIDTH,
                             APP_MANAGER_RECENT_PREVIEW_HEIGHT,
                             LV_COLOR_FORMAT_RGB565, stride,
                             entry->data,
                             APP_MANAGER_RECENTS_PREVIEW_BYTES) != LV_RESULT_OK)
        {
            LOG_W("preview slot %u init failed", (unsigned)index);
            app_manager_recents_deinit();
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_initialized = true;
    return ESP_OK;
}

void app_manager_recents_deinit(void)
{
    for (size_t index = 0U; index < APP_MANAGER_MAX_RECENT_TASKS; index++)
    {
        _recents_free(s_entries[index].data);
        memset(&s_entries[index], 0, sizeof(s_entries[index]));
    }
    s_initialized = false;
    s_order = 0U;
}

void app_manager_recents_capture(const char *app_id,
                                 const lv_draw_buf_t *source)
{
    if (!s_initialized || app_id == NULL || source == NULL ||
            source->data == NULL || source->header.w == 0U ||
            source->header.h == 0U || source->header.stride == 0U)
    {
        return;
    }

    app_manager_recents_entry_t *entry = _recents_find(app_id);
    if (entry == NULL)
    {
        entry = _recents_allocate();
        entry->valid = false;
        entry->order = 0U;
        entry->app_id[0] = '\0';
        (void)snprintf(entry->app_id, sizeof(entry->app_id), "%s", app_id);
        entry->in_use = true;
    }

    const uint8_t *source_pixels = source->data;
    const uint32_t source_stride = source->header.stride;
    const uint32_t source_width = source->header.w;
    const uint32_t source_height = source->header.h;
    uint8_t *preview_pixels = entry->draw_buf.data;
    const uint32_t preview_stride = entry->draw_buf.header.stride;

    for (uint32_t y = 0U; y < APP_MANAGER_RECENT_PREVIEW_HEIGHT; y++)
    {
        const uint32_t source_y =
            (y * source_height) / APP_MANAGER_RECENT_PREVIEW_HEIGHT;
        for (uint32_t x = 0U; x < APP_MANAGER_RECENT_PREVIEW_WIDTH; x++)
        {
            const uint32_t source_x =
                (x * source_width) / APP_MANAGER_RECENT_PREVIEW_WIDTH;
            memcpy(preview_pixels + y * preview_stride + x * 2U,
                   source_pixels + source_y * source_stride + source_x * 2U,
                   2U);
        }
    }
    entry->valid = true;
    entry->order = ++s_order;
    lv_draw_buf_flush_cache(&entry->draw_buf, NULL);
}

void app_manager_recents_invalidate(const char *app_id)
{
    if (!s_initialized || app_id == NULL)
    {
        return;
    }
    app_manager_recents_entry_t *entry = _recents_find(app_id);
    if (entry == NULL)
    {
        return;
    }
    entry->in_use = false;
    entry->valid = false;
    entry->order = 0U;
    entry->app_id[0] = '\0';
}

bool app_manager_recents_lookup(const char *app_id,
                                const lv_draw_buf_t **preview)
{
    if (preview != NULL)
    {
        *preview = NULL;
    }
    if (!s_initialized || app_id == NULL)
    {
        return false;
    }
    const app_manager_recents_entry_t *entry = _recents_find(app_id);
    if (entry == NULL || !entry->valid)
    {
        return false;
    }
    if (preview != NULL)
    {
        *preview = &entry->draw_buf;
    }
    return true;
}
