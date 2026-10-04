# BMDS — Bare Metal Do Stuff
# ESP-IDF project root. See docs/BUILDING.md.

cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only build the components `main` actually depends on (transitively),
# instead of every component in ESP-IDF.
idf_build_set_property(MINIMAL_BUILD ON)

project(bmds)

# Guard against linking code BMDS must never contain (see cmake/check_map.cmake).
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -DMAP=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
                           -P ${CMAKE_SOURCE_DIR}/cmake/check_map.cmake
                   COMMENT "Checking the link map")
