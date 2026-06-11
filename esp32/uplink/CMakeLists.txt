cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "main")
# Read version from shared/version.h (single source of truth). FoF Badge
# and production are on separate version tracks; pick the right macro
# based on the PlatformIO env name so ESP-IDF's PROJECT_VER metadata
# matches the runtime FOF_VERSION string.
set(_fof_version_header "${CMAKE_CURRENT_SOURCE_DIR}/../shared/version.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fof_version_header}")

if("$ENV{PIOENV}" MATCHES "fof_badge")
    set(_ver_macro "FOF_VERSION_BADGE")
else()
    set(_ver_macro "FOF_VERSION_PROD")
endif()
file(STRINGS "${_fof_version_header}" _ver_line REGEX "^#define ${_ver_macro} ")
string(REGEX MATCH "\"([^\"]+)\"" _ver_match "${_ver_line}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Failed to parse ${_ver_macro} from shared/version.h (PIOENV=$ENV{PIOENV})")
endif()
set(PROJECT_VER "${CMAKE_MATCH_1}")
message(STATUS "FoF: using ${_ver_macro} = ${PROJECT_VER} (PIOENV=$ENV{PIOENV})")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fof_uplink)
