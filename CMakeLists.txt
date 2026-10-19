# BMDS — Bare Metal Do Stuff
# ESP-IDF project root. See docs/BUILDING.md.

cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only build the components `main` actually depends on (transitively),
# instead of every component in ESP-IDF.
idf_build_set_property(MINIMAL_BUILD ON)

# PROJECT_VER: bmds/version.h's version and the commit (cmake/version.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

project(bmds)
bmds_version_every_build()

# Guard against linking code BMDS must never contain (see cmake/check_map.cmake).
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -DMAP=${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map
                           -P ${CMAKE_SOURCE_DIR}/cmake/check_map.cmake
                   COMMENT "Checking the link map")

# The recovery firmware (recovery/, devplan/update.md §4), built in build-recovery/: idf.py flash
# writes it to ota_1 and `build update` carries it when it is there (looked for at configure time:
# reconfigure after building it the first time).
set(BMDS_RECOVERY_BIN ${CMAKE_SOURCE_DIR}/build-recovery/bmds_recovery.bin CACHE FILEPATH
    "The recovery firmware's image, for ota_1")
if(EXISTS ${BMDS_RECOVERY_BIN})
        esptool_py_flash_to_partition(flash ota_1 ${BMDS_RECOVERY_BIN})
else()
        message(STATUS "No recovery firmware at ${BMDS_RECOVERY_BIN}: flash leaves ota_1 alone")
endif()

# The update package (devplan/update.md §2): `build update` makes bmds.bmup of this build's
# firmware and ROM (and the recovery firmware, above) for its board, signed with BMDS_UPDATE_KEY (default: keys/dev.key) and refused
# unless the acceptance mode (CONFIG_BMDS_UPDATE_KEYS_*) takes that key. BMDS_UPDATE_NOTES names
# release notes to carry.
set(BMDS_UPDATE_KEY "" CACHE FILEPATH
    "The key update packages are signed with (default: keys/dev.key)")
set(BMDS_UPDATE_NOTES "" CACHE FILEPATH "Release notes for the update package (optional)")
set(bmds_update_args --board ${CONFIG_BMDS_BOARD_DIR} --firmware ${CMAKE_BINARY_DIR}/bmds.bin
                     --rom ${CMAKE_BINARY_DIR}/rom/rom.bin
                     --partitions ${CMAKE_SOURCE_DIR}/partitions.csv)
if(EXISTS ${CMAKE_SOURCE_DIR}/keys/release.pub)
        list(APPEND bmds_update_args --accept ${CMAKE_SOURCE_DIR}/keys/release.pub)
endif()
if(EXISTS ${BMDS_RECOVERY_BIN})
        list(APPEND bmds_update_args --recovery ${BMDS_RECOVERY_BIN})
endif()
if(CONFIG_BMDS_UPDATE_KEYS_DEV)
        list(APPEND bmds_update_args --accept ${CMAKE_SOURCE_DIR}/keys/dev.pub)
endif()
if(BMDS_UPDATE_KEY)
        list(APPEND bmds_update_args --key ${BMDS_UPDATE_KEY})
endif()
if(BMDS_UPDATE_NOTES)
        list(APPEND bmds_update_args --notes ${BMDS_UPDATE_NOTES})
endif()
idf_build_get_property(bmds_python PYTHON)
add_custom_target(update
                  COMMAND ${bmds_python} ${CMAKE_SOURCE_DIR}/tools/mkupdate make
                          ${CMAKE_BINARY_DIR}/bmds.bmup ${bmds_update_args}
                  COMMENT "Making the update package"
                  VERBATIM)
add_dependencies(update app bmds_rom_image)
