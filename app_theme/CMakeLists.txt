idf_component_register(
    SRCS
        "src/app_theme.c"
        "src/app_theme_lvgl.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
)
