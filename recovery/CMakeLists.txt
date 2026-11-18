# BMDS's recovery firmware (devplan/update.md §4): the same components, built for ota_1, that
# apply updates and repair the main firmware. Build it in build-recovery/ (docs/BUILDING.md):
#   idf.py -B build-recovery -C recovery set-target esp32p4; cd build-recovery && build
# The main project's flash writes it to ota_1 when it is there.

cmake_minimum_required(VERSION 3.22)

# Normalized: IDF's reproducible build maps each component's folder by its text, and ".." in it
# matches none, leaving this machine's paths in the debug information (devplan/debuginfo.md §13b).
get_filename_component(BMDS_COMPONENTS "${CMAKE_CURRENT_LIST_DIR}/../components" ABSOLUTE)
set(EXTRA_COMPONENT_DIRS ${BMDS_COMPONENTS})
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# Size-checked against its own slot (components/bmds_storage/project_include.cmake).
set(BMDS_APP_PARTITION ota_1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The repository's files outside this project (thirdparty/..., and __FILE__ in its components) as
# /BMDS/..., not this machine's path: IDF's reproducible build maps this project's folder, IDF and
# each component's folder only (the main project's folder is the repository). Given before
# project(), IDF's own maps come after it and win where they match (devplan/debuginfo.md §13b).
get_filename_component(BMDS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
idf_build_set_property(COMPILE_OPTIONS "-ffile-prefix-map=${BMDS_ROOT}=/BMDS" APPEND)

idf_build_set_property(MINIMAL_BUILD ON)
# For the components' requirements, which are expanded before Kconfig's values are known (as
# CONFIG_BMDS_RECOVERY is): what this firmware leaves out (bmds_input: MIDI, and with it the
# network).
idf_build_set_property(BMDS_RECOVERY ON)

# PROJECT_VER: the same version as the main firmware's (cmake/version.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/version.cmake)

project(bmds_recovery)
bmds_version_every_build()
