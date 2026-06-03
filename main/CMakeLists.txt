idf_component_register(SRCS "app_main.c"
                            "app_version.c"
                            "generic_task.c"
                            "gnss.c"
                            "http_stream.c"
                            "ota_update.c"
                            "wifi_ap.c"
                    PRIV_REQUIRES app_update esp_app_format esp_driver_gpio esp_driver_uart esp_http_server esp_netif esp_system esp_wifi lwip mbedtls nvs_flash
                    INCLUDE_DIRS ".")

set(STREAM_APP_VERSION_FLAVOR "main" CACHE STRING "Firmware identity suffix printed in boot/status logs")

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE STREAM_APP_VERSION_GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

if(NOT STREAM_APP_VERSION_GIT_TAG)
    set(STREAM_APP_VERSION_GIT_TAG "unknown")
endif()

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE
        APP_VERSION_GIT_TAG="${STREAM_APP_VERSION_GIT_TAG}"
        APP_VERSION_FLAVOR="${STREAM_APP_VERSION_FLAVOR}")
