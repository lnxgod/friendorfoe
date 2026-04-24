cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "main"
)
# Read version from shared/version.h (single source of truth)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../shared/version.h" _ver_line REGEX "FOF_VERSION")
string(REGEX MATCH "\"([^\"]+)\"" _ver_match "${_ver_line}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Failed to parse FOF_VERSION from shared/version.h")
endif()
set(PROJECT_VER "${CMAKE_MATCH_1}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fof_scanner)
