file(GLOB_RECURSE SRCS "*.c")
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "fonts" "images" "ui"
    REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_wifi esp_netif nvs_flash esp_http_server
             app_update esp_app_format json mbedtls esp_timer driver console vfs
    EMBED_TXTFILES "web/index.html")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-format-truncation -Wno-deprecated-declarations)
# LVGL's allocator lives here (ui/lv_mem_psram.c); LVGL links after us, so pull the object in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")
