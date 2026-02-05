# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
        COMMAND git describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(PROJECT_VER "${GIT_TAG}-${GIT_HASH}")
add_compile_definitions(VERSION_HASH="${GIT_HASH}")
add_compile_definitions(VERSION_TAG="${GIT_TAG}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(odroid-power-mate)
