# bmds_storage — the SD card driver and the storage service (card detect,
# hot-plug), the `sd` command, the RAM disk, and /boot on the flash. Target-only.
#
# /boot's default files are the repository's boot/ directory, embedded by
# tools/embedfiles.py (devplan/gui.md G14). /rom's image is built from rom/ by
# tools/mkrom.py and written to the partition "rom" by idf.py flash (devplan/rom.md).
#
# WHOLE_ARCHIVE: nothing references sdcommand.cpp by symbol -- its command is
# found through the linker section -- so without it the linker would never
# pull that object out of the archive.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/bootdisk.cpp
                            src/flashpartition.cpp
                            src/fsbench.cpp
                            src/ramdisk.cpp
                            src/romcommand.cpp
                            src/romdisk.cpp
                            src/sdcard.cpp
                            src/sdcommand.cpp
                            src/storage.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_kernel bmds_fs
                       PRIV_REQUIRES app_update bmds_board bmds_shell esp_driver_gpio esp_driver_sdmmc esp_hw_support esp_system
                                     esp_mm esp_partition sdmmc
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

# The ROM (devplan/rom.md §3c): the bmds icon theme made from rom/icons.list, then the image
# (and rom/files/, when there is such a tree), refused if it outgrows its partition; idf.py flash
# writes it. Its content hash becomes romhash.h, which RomDisk compares with what it finds.
if(NOT BOOTLOADER_BUILD)
        set(BMDS_ROM_DIR ${CMAKE_CURRENT_LIST_DIR}/../../rom)
        set(BMDS_MKROM ${CMAKE_CURRENT_LIST_DIR}/../../tools/mkrom.py)
        set(BMDS_ROM_OUT ${CMAKE_BINARY_DIR}/rom)
        set(BMDS_ROM_BIN ${BMDS_ROM_OUT}/rom.bin)
        set(BMDS_ROM_HASH ${CMAKE_CURRENT_BINARY_DIR}/romhash.h)
        partition_table_get_partition_info(BMDS_ROM_PARTITION_BYTES "--partition-name rom" "size")
        set(BMDS_ROM_TREE)
        set(BMDS_ROM_TREE_FILES)
        if(EXISTS ${BMDS_ROM_DIR}/files)
                set(BMDS_ROM_TREE --dir =${BMDS_ROM_DIR}/files)
                file(GLOB_RECURSE BMDS_ROM_TREE_FILES CONFIGURE_DEPENDS ${BMDS_ROM_DIR}/files/*)
        endif()
        add_custom_command(OUTPUT ${BMDS_ROM_OUT}/bmds.icons ${BMDS_ROM_OUT}/bmds.LICENSES
                           COMMAND ${BMDS_STORAGE_PYTHON} ${BMDS_MKROM} icons ${BMDS_ROM_DIR}/icons.list
                                   ${CMAKE_CURRENT_LIST_DIR}/../.. ${BMDS_ROM_OUT}/bmds.icons
                                   ${BMDS_ROM_OUT}/bmds.LICENSES
                           DEPENDS ${BMDS_MKROM} ${BMDS_ROM_DIR}/icons.list
                           VERBATIM)
        add_custom_command(OUTPUT ${BMDS_ROM_BIN} ${BMDS_ROM_HASH}
                           COMMAND ${BMDS_STORAGE_PYTHON} ${BMDS_MKROM} image ${BMDS_ROM_BIN}
                                   --max-bytes ${BMDS_ROM_PARTITION_BYTES} ${BMDS_ROM_TREE}
                                   --file share/icons/bmds.icons=${BMDS_ROM_OUT}/bmds.icons
                                   --file share/icons/bmds.LICENSES=${BMDS_ROM_OUT}/bmds.LICENSES
                                   --hash-header ${BMDS_ROM_HASH}
                           DEPENDS ${BMDS_MKROM} ${BMDS_ROM_OUT}/bmds.icons ${BMDS_ROM_OUT}/bmds.LICENSES
                                   ${BMDS_ROM_TREE_FILES}
                           VERBATIM)
        target_sources(${COMPONENT_LIB} PRIVATE ${BMDS_ROM_HASH})
        target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        add_custom_target(bmds_rom_image ALL DEPENDS ${BMDS_ROM_BIN})
        add_dependencies(flash bmds_rom_image)
        esptool_py_flash_to_partition(flash rom ${BMDS_ROM_BIN})
endif()
