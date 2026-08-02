cmake_minimum_required(VERSION 3.16)

file(
    STRINGS "../shared/backend_version.h" PROJECT_VER
    REGEX "^#define FOF_VERSION_BACKEND \"")
list(TRANSFORM PROJECT_VER REPLACE "^#define FOF_VERSION_BACKEND \"" "")
list(TRANSFORM PROJECT_VER REPLACE "\"" "")
if(NOT PROJECT_VER)
    message(FATAL_ERROR "Failed to parse FOF_VERSION_BACKEND")
endif()

if(NOT DEFINED FOF_BACKEND_PROJECT_NAME OR
   NOT DEFINED FOF_BACKEND_PROFILE_NAME)
    message(FATAL_ERROR "Backend scanner project/profile identity is required")
endif()
if("${FOF_BACKEND_PROFILE_NAME}" STREQUAL "badge_lite")
    set(FOF_BACKEND_EXPECTED_PROJECT_NAME "fof_backend_scanner")
elseif("${FOF_BACKEND_PROFILE_NAME}" STREQUAL "s3_fullsize")
    set(FOF_BACKEND_EXPECTED_PROJECT_NAME "fof_backend_scanner_fullsize")
else()
    message(FATAL_ERROR "Unknown backend scanner profile: ${FOF_BACKEND_PROFILE_NAME}")
endif()
if(NOT "${FOF_BACKEND_PROJECT_NAME}" STREQUAL
       "${FOF_BACKEND_EXPECTED_PROJECT_NAME}")
    message(FATAL_ERROR
        "Backend scanner project/profile identity mismatch")
endif()

set(COMPONENTS main __pio_env)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${FOF_BACKEND_PROJECT_NAME})
