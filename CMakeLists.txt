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
