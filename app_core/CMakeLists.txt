idf_component_register(
    SRCS "src/app_manager_pm.c"
         "src/app_manager_pm_api.c"
         "src/app_manager_pm_init.c"
         "src/app_manager_pm_worker.c"
         "src/app_manager_mailbox.c"
         "src/app_manager_builtin.c"
         "src/app_manager_back_gesture.c"
         "src/app_manager_nav_model.c"
         "src/app_manager_navigation.c"
         "src/app_manager_presentation.c"
         "src/app_manager_lifecycle.c"
         "src/app_manager_lifecycle_api.c"
         "src/app_manager_recents.c"
         "src/app_manager_task_switcher.c"
         "src/app_manager_display_diagnostics.c"
         "src/app_manager_lvgl_adapter.c"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
    REQUIRES
        app_theme
        bsp
        lvgl__lvgl
        esp_common
    PRIV_REQUIRES
        espressif__esp_lvgl_adapter
        esp_lcd
        esp_driver_gpio
        mt_log
        nv_storage
        esp_timer
        freertos
        heap
        esp_mmap_assets
)
