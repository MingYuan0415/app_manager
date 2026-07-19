cmake_minimum_required(VERSION 3.16)
project(app_manager_host_tests C)

enable_testing()
find_package(Threads REQUIRED)

set(APP_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(APP_MANAGER_SANITIZER "none" CACHE STRING "none, address, or thread")
set_property(CACHE APP_MANAGER_SANITIZER PROPERTY STRINGS none address thread)
find_program(APP_MANAGER_SETARCH setarch)

function(configure_host_test target)
    target_sources(${target} PRIVATE host_freertos.c)
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE HOST_TEST=1)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
        "${APP_CORE_DIR}/include"
        "${APP_CORE_DIR}/src"
        "${APP_CORE_DIR}/../app_theme/include"
    )
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wno-variadic-macros
    )
    target_link_libraries(${target} PRIVATE Threads::Threads)

    if(APP_MANAGER_SANITIZER STREQUAL "address")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    elseif(APP_MANAGER_SANITIZER STREQUAL "thread")
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    elseif(NOT APP_MANAGER_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "Unknown APP_MANAGER_SANITIZER=${APP_MANAGER_SANITIZER}")
    endif()
endfunction()

function(add_host_test name target)
    if(APP_MANAGER_SANITIZER STREQUAL "thread" AND
            APP_MANAGER_SETARCH AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_test(NAME ${name} COMMAND
            ${APP_MANAGER_SETARCH} x86_64 -R $<TARGET_FILE:${target}>)
    else()
        add_test(NAME ${name} COMMAND ${target})
    endif()
endfunction()

add_executable(test_mailbox
    test_mailbox.c
    "${APP_CORE_DIR}/src/app_manager_mailbox.c"
)
configure_host_test(test_mailbox)
add_host_test(mailbox test_mailbox)

add_executable(test_navigation
    test_navigation.c
    "${APP_CORE_DIR}/src/app_manager_navigation.c"
)
configure_host_test(test_navigation)
add_host_test(navigation test_navigation)

add_executable(test_registry
    test_registry.c
    "${APP_CORE_DIR}/src/app_manager_builtin.c"
)
configure_host_test(test_registry)
add_host_test(registry test_registry)

add_executable(test_nav_model
    test_nav_model.c
    "${APP_CORE_DIR}/src/app_manager_nav_model.c"
)
configure_host_test(test_nav_model)
add_host_test(nav_model test_nav_model)

add_executable(test_lifecycle
    test_lifecycle.c
    "${APP_CORE_DIR}/src/app_manager_mailbox.c"
    "${APP_CORE_DIR}/src/app_manager_builtin.c"
    "${APP_CORE_DIR}/src/app_manager_lifecycle.c"
    "${APP_CORE_DIR}/src/app_manager_lifecycle_api.c"
    "${APP_CORE_DIR}/src/app_manager_nav_model.c"
    "${APP_CORE_DIR}/src/app_manager_navigation.c"
)
configure_host_test(test_lifecycle)
add_host_test(lifecycle test_lifecycle)

add_executable(test_control
    test_pm.c
    host_control_port.c
    "${APP_CORE_DIR}/src/app_manager_pm.c"
    "${APP_CORE_DIR}/src/app_manager_pm_api.c"
    "${APP_CORE_DIR}/src/app_manager_pm_init.c"
    "${APP_CORE_DIR}/src/app_manager_pm_worker.c"
)
configure_host_test(test_control)
add_host_test(control test_control)
set_tests_properties(control PROPERTIES TIMEOUT 30)

add_executable(test_adapter
    test_adapter.c
    "${APP_CORE_DIR}/src/app_manager_lvgl_adapter.c"
)
configure_host_test(test_adapter)
target_compile_definitions(test_adapter PRIVATE
    CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS=1
    CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE=1
)
add_host_test(adapter test_adapter)
