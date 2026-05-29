idf_component_register(SRCS "auth_client.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_client esp_wifi json)
