set(BUILTIN_TEXT_FONT font_noto_basic_16_4)
set(BUILTIN_ICON_FONT font_awesome_14_1)

idf_component_register(SRCS "display.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_lcd esp_timer lvgl esp_lvgl_port 78__xiaozhi-fonts)

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT}
            BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT})
