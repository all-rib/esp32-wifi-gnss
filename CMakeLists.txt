# The following lines of boilerplate have to be in an ESP-IDF project's
# CMakeLists.txt in this exact order for CMake to work correctly.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp32_wifi_stream)

#
# ESP-IDF v6.0.1 generates a "monitor" Ninja target through a CMake
# execute_process() wrapper. In this local setup that wrapper returns
# immediately instead of keeping the interactive terminal attached.
#
# Keep the generated target untouched and expose a project-local replacement
# that launches idf.py directly. ESPPORT and ESPBAUD remain runtime environment
# variables, for example:
#
#   ESPPORT=/dev/ttyUSB0 ninja -C build serial-monitor
#
add_custom_target(serial-monitor
    COMMAND $ENV{IDF_PYTHON_ENV_PATH}/bin/python $ENV{IDF_PATH}/tools/idf.py monitor
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM)

if(DEFINED ENV{IDF_PYTHON_ENV_PATH} AND EXISTS "$ENV{IDF_PYTHON_ENV_PATH}/bin/python")
    set(HARDWARE_TEST_PYTHON "$ENV{IDF_PYTHON_ENV_PATH}/bin/python")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HARDWARE_TEST_PYTHON "${Python3_EXECUTABLE}")
endif()

set(HARDWARE_TEST_ENV
    "ESP_IDF_VERSION=$ENV{ESP_IDF_VERSION}"
    "IDF_TOOLS_PATH=$ENV{IDF_TOOLS_PATH}"
    "IDF_COMPONENT_LOCAL_STORAGE_URL=$ENV{IDF_COMPONENT_LOCAL_STORAGE_URL}"
    "IDF_PATH=$ENV{IDF_PATH}"
    "ESP_ROM_ELF_DIR=$ENV{ESP_ROM_ELF_DIR}"
    "OPENOCD_SCRIPTS=$ENV{OPENOCD_SCRIPTS}"
    "IDF_PYTHON_ENV_PATH=$ENV{IDF_PYTHON_ENV_PATH}"
    "PATH=$ENV{PATH}")

add_custom_target(hardware-test-builds
    COMMAND ${CMAKE_COMMAND} -E env ${HARDWARE_TEST_ENV}
            "${HARDWARE_TEST_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/hardware_ota_test.py"
            --build-root "${CMAKE_BINARY_DIR}/hardware-tests"
            --build-only
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM)

add_custom_target(hardware-test-boot
    COMMAND ${CMAKE_COMMAND} -E env ${HARDWARE_TEST_ENV}
            "${HARDWARE_TEST_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/hardware_ota_test.py"
            --build-root "${CMAKE_BINARY_DIR}/hardware-tests"
            --skip-ota
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM)

add_custom_target(hardware-test-manual-wifi
    COMMAND ${CMAKE_COMMAND} -E env ${HARDWARE_TEST_ENV}
            "${HARDWARE_TEST_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/hardware_ota_test.py"
            --build-root "${CMAKE_BINARY_DIR}/hardware-tests"
            --no-manage-wifi
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM)

add_custom_target(hardware-test
    COMMAND ${CMAKE_COMMAND} -E env ${HARDWARE_TEST_ENV}
            "${HARDWARE_TEST_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/hardware_ota_test.py"
            --build-root "${CMAKE_BINARY_DIR}/hardware-tests"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM)
