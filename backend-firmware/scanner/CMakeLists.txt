cmake_minimum_required(VERSION 3.16)

file(
    STRINGS "../shared/backend_version.h" PROJECT_VER
    REGEX "^#define FOF_VERSION_BACKEND \"")
list(TRANSFORM PROJECT_VER REPLACE "^#define FOF_VERSION_BACKEND \"" "")
list(TRANSFORM PROJECT_VER REPLACE "\"" "")
if(NOT PROJECT_VER)
    message(FATAL_ERROR "Failed to parse FOF_VERSION_BACKEND")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fof_backend_scanner)
