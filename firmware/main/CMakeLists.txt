set(BUILTIN_TEXT_FONT font_puhui_14_1)
set(BUILTIN_ICON_FONT font_awesome_14_1)

idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_http_client json driver lvgl esp_lvgl_port auth_client display auth_button)

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT}
            BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT})
