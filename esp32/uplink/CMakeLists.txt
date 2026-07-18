cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "main")
# Select descriptor project and version from the exact release target.
set(_fof_version_header "${CMAKE_CURRENT_SOURCE_DIR}/../shared/version.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fof_version_header}")

if("$ENV{PIOENV}" STREQUAL "uplink-s3")
    set(_fof_project "fof_uplink")
    set(_ver_macro "FOF_VERSION_PROD")
elseif("$ENV{PIOENV}" STREQUAL "uplink-s3-fof_badge")
    set(_fof_project "fof_badge_uplink")
    set(_ver_macro "FOF_VERSION_BADGE")
else()
    message(FATAL_ERROR "Unknown FoF uplink PIOENV=$ENV{PIOENV}")
endif()
file(STRINGS "${_fof_version_header}" _ver_line REGEX "^#define ${_ver_macro} ")
string(REGEX MATCH "\"([^\"]+)\"" _ver_match "${_ver_line}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Failed to parse ${_ver_macro} from shared/version.h (PIOENV=$ENV{PIOENV})")
endif()
set(PROJECT_VER "${CMAKE_MATCH_1}")
message(STATUS "FoF: target=$ENV{PIOENV} project=${_fof_project} version=${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${_fof_project})
