cmake_minimum_required(VERSION 3.16)

# A single, build-local copy of ESP-IDF FatFS changes both its implementation
# and public headers together. Never patch the user's global ESP-IDF checkout.
set(IDF_TARGET "esp32p4" CACHE STRING "M5Stack Tab5 target")
set(PROJECT_VER "1.3.21")
include(${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_player)
