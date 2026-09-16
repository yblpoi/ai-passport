idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "love_app.c"
         "love_date.c"
         "love_lunar.c"
         "love_store.c"
         "love_time.c"
         "love_net.c"
         "love_ble.c"
         "love_httpd.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_http_server esp_netif esp_timer esp_wifi json nvs_flash
)

# 生成的素材:像素字库(assets/fonts/)与像素图标、爱心底纹、调色板(assets/images/)。
# 重新生成方式见 assets/README.zh_CN.md;不要手工编辑这些文件。
# 字号只有 12 / 24 / 36 —— 像素字体只在整数倍下才不变形,别在这里加第四种。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/love_font_12.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/love_font_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/love_font_36.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/images/love_pixel_art.c"
)
