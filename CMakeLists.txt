# BMDS — Bare Metal Do Stuff
# ESP-IDF project root. See docs/BUILDING.md.

cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only build the components `main` actually depends on (transitively),
# instead of every component in ESP-IDF.
idf_build_set_property(MINIMAL_BUILD ON)

project(bmds)
