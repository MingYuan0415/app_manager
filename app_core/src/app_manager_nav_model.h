#ifndef __APP_MANAGER_NAV_MODEL_H__
#define __APP_MANAGER_NAV_MODEL_H__

#include "app_manager_builtin.h"
#include "app_manager_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct app_manager_app_node app_manager_app_node_t;
typedef struct app_manager_page_node app_manager_page_node_t;

struct app_manager_page_node
{
    bool in_use;
    bool cleanup_pending;
    bool unmount_pending;
    bool started;
    bool mounted;
    bool active;
    uint32_t generation;
    const app_manager_page_route_t *route;
    void *private_memory;
    app_manager_typed_blob_t arguments;
    bool has_arguments;
    lv_obj_t *screen;
    app_manager_page_node_t *previous;
    app_manager_page_node_t *next;
    app_manager_app_node_t *app;
};

struct app_manager_app_node
{
    bool in_use;
    bool cleanup_pending;
    bool active;
    const app_manager_app_desc_t *descriptor;
    app_manager_typed_blob_t arguments;
    bool has_arguments;
    app_manager_page_node_t *current_page;
    app_manager_app_node_t *previous;
    app_manager_app_node_t *next;
};

typedef struct app_manager_nav_model
{
    app_manager_app_node_t apps[APP_MANAGER_MAX_RESIDENT_APPS];
    app_manager_page_node_t pages[APP_MANAGER_MAX_PAGE_INSTANCES];
    app_manager_app_node_t *active_app;
    uint8_t *page_arena;
    size_t page_stride;
} app_manager_nav_model_t;

esp_err_t app_manager_nav_model_init(app_manager_nav_model_t *model);
void app_manager_nav_model_deinit(app_manager_nav_model_t *model);

app_manager_app_node_t *app_manager_nav_model_find_app(
    app_manager_nav_model_t *model, const char *app_id);
app_manager_page_node_t *app_manager_nav_model_find_page(
    app_manager_app_node_t *app, const char *page_id);
app_manager_app_node_t *app_manager_nav_model_alloc_app(
    app_manager_nav_model_t *model);
app_manager_page_node_t *app_manager_nav_model_alloc_page(
    app_manager_nav_model_t *model,
    const app_manager_page_route_t *route);
void app_manager_nav_model_free_app(app_manager_app_node_t *app);
void app_manager_nav_model_free_page(app_manager_nav_model_t *model,
                                     app_manager_page_node_t *page);

void app_manager_nav_model_push_app(app_manager_nav_model_t *model,
                                    app_manager_app_node_t *app);
void app_manager_nav_model_remove_app(app_manager_nav_model_t *model,
                                      app_manager_app_node_t *app);
void app_manager_nav_model_link_page(app_manager_app_node_t *app,
                                     app_manager_page_node_t *page);
void app_manager_nav_model_unlink_page(app_manager_app_node_t *app,
                                       app_manager_page_node_t *page);

uint32_t app_manager_nav_model_running_count(
    const app_manager_nav_model_t *model);
bool app_manager_nav_model_invariants(const app_manager_nav_model_t *model);

#endif /* __APP_MANAGER_NAV_MODEL_H__ */
