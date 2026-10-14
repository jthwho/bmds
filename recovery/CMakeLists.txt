# BMDS's recovery firmware (devplan/update.md §4): the same components, built for ota_1, that
# apply updates and repair the main firmware. Build it in build-recovery/ (docs/BUILDING.md):
#   idf.py -B build-recovery -C recovery set-target esp32p4; cd build-recovery && build
# The main project's flash writes it to ota_1 when it is there.

cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# Size-checked against its own slot (components/bmds_storage/project_include.cmake).
set(BMDS_APP_PARTITION ota_1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

# PROJECT_VER: the same version as the main firmware's (cmake/version.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/version.cmake)

project(bmds_recovery)
