# bmds_storage — the SD card driver and the storage service (card detect,
# hot-plug), the `sd` command, the RAM disk, and /boot on the flash. Target-only.
#
# /boot's default files are the repository's boot/ directory, embedded by
# tools/embedfiles.py (devplan/gui.md G14).
#
# WHOLE_ARCHIVE: nothing references sdcommand.cpp by symbol -- its command is
# found through the linker section -- so without it the linker would never
# pull that object out of the archive.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/bootdisk.cpp
                            src/ramdisk.cpp
                            src/sdcard.cpp
                            src/sdcommand.cpp
                            src/storage.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_kernel bmds_fs
                       PRIV_REQUIRES bmds_board bmds_shell esp_driver_gpio esp_driver_sdmmc esp_hw_support esp_system
                                     esp_mm esp_partition sdmmc wear_levelling
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})

set(BMDS_BOOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../../boot)
set(BMDS_BOOT_SRC ${CMAKE_CURRENT_BINARY_DIR}/bootdefaults.cpp)
set(BMDS_EMBED ${CMAKE_CURRENT_LIST_DIR}/../../tools/embedfiles.py)
file(GLOB_RECURSE BMDS_BOOT_FILES CONFIGURE_DEPENDS ${BMDS_BOOT_DIR}/*)
idf_build_get_property(BMDS_STORAGE_PYTHON PYTHON)
add_custom_command(OUTPUT ${BMDS_BOOT_SRC}
                   COMMAND ${BMDS_STORAGE_PYTHON} ${BMDS_EMBED} ${BMDS_BOOT_DIR} ${BMDS_BOOT_SRC}
                           bootdefaults.h BootDefaults
                   DEPENDS ${BMDS_EMBED} ${BMDS_BOOT_FILES}
                   VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${BMDS_BOOT_SRC})
target_include_directories(${COMPONENT_LIB} PRIVATE src)
