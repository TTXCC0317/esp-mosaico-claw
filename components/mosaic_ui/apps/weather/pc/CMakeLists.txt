# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(weather_backend LANGUAGES C)

get_filename_component(WEATHER_APP_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(MOSAIC_UI_DIR "${WEATHER_APP_DIR}/../.." ABSOLUTE)
get_filename_component(REPO_ROOT "${MOSAIC_UI_DIR}/../.." ABSOLUTE)

# Honor the runner's explicit component selection, then the environment, then
# the managed component checked out inside this repository.
if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    set(ESP_GSP_COMPONENT_DIR "${REPO_ROOT}/managed_components/espressif__esp-gsp")
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Cannot find the esp-gsp sim_bridge; set ESP_GSP_COMPONENT_DIR to the component directory")
endif()

add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)

# gsp_add_backend packs the deployable bundle and the scene C API, and builds
# both the socket executable and the loadable backend library.
gsp_add_backend(weather_backend
    SOURCES "${WEATHER_APP_DIR}/weather_app.c" platform_pc.c
    INCLUDE_DIRECTORIES "${WEATHER_APP_DIR}"
    SCENES "${WEATHER_APP_DIR}/scene/weather_480.json")

# weather_app.c includes the split gspc headers and the Mosaico framework
# headers. The split headers live under the generated scene0/ subdirectory.
set(_weather_gen "${CMAKE_CURRENT_BINARY_DIR}/weather_backend_assets/scene0")
# mosaic_hub_actions.h is produced by the firmware build (gspc for the hub
# scene); reuse that generated header rather than restating action ids.
set(_hub_actions "${REPO_ROOT}/build/esp-idf/mosaic_ui/mosaic_gen/hub")
if(NOT EXISTS "${_hub_actions}/mosaic_hub_actions.h")
    message(FATAL_ERROR
        "mosaic_hub_actions.h not found in ${_hub_actions}; run 'idf.py build' first to generate the scene headers")
endif()
foreach(_target IN ITEMS weather_backend weather_backend_module)
    target_include_directories(${_target} PRIVATE
        "${_weather_gen}"
        "${MOSAIC_UI_DIR}/common"
        "${_hub_actions}")
endforeach()
