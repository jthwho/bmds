# bmds_storage — the SD card driver and the storage service (card detect,
# hot-plug), the `sd` command, /tmp, /flash on the flash, and /cfg (devplan/cfg.md). Target-only.
#
# The default files are the repository's flash/ directory (cfg/ for /cfg, the rest for
# /flash), embedded by tools/embedfiles.py (devplan/gui.md G14). /rom's image is built from
# rom/ by tools/mkrom.py and written to the partition "rom" by idf.py flash (devplan/rom.md).
#
# WHOLE_ARCHIVE: nothing references sdcommand.cpp by symbol -- its command is
# found through the linker section -- so without it the linker would never
# pull that object out of the archive.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/benchsd.cpp
                            src/cfg.cpp
                            src/flashdisk.cpp
                            src/flashpartition.cpp
                            src/fsbench.cpp
                            src/romcommand.cpp
                            src/romdisk.cpp
                            src/sdcard.cpp
                            src/sdcommand.cpp
                            src/storage.cpp
                            src/tmpfs.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_kernel bmds_fs
                       PRIV_REQUIRES app_update bmds_bench bmds_board bmds_shell bmds_sysconf esp_driver_gpio esp_driver_sdmmc esp_hw_support esp_system
                                     esp_mm esp_partition sdmmc
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})

set(BMDS_FLASH_DIR ${CMAKE_CURRENT_LIST_DIR}/../../flash)
set(BMDS_FLASH_SRC ${CMAKE_CURRENT_BINARY_DIR}/flashdefaults.cpp)
set(BMDS_EMBED ${CMAKE_CURRENT_LIST_DIR}/../../tools/embedfiles.py)
file(GLOB_RECURSE BMDS_FLASH_FILES CONFIGURE_DEPENDS ${BMDS_FLASH_DIR}/*)
idf_build_get_property(BMDS_STORAGE_PYTHON PYTHON)
add_custom_command(OUTPUT ${BMDS_FLASH_SRC}
                   COMMAND ${BMDS_STORAGE_PYTHON} ${BMDS_EMBED} ${BMDS_FLASH_DIR} ${BMDS_FLASH_SRC}
                           flashdefaults.h FlashDefaults
                   DEPENDS ${BMDS_EMBED} ${BMDS_FLASH_FILES}
                   VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${BMDS_FLASH_SRC})
target_include_directories(${COMPONENT_LIB} PRIVATE src)

# The ROM (devplan/rom.md §3c): the bmds icon theme made from rom/icons.list, the time zone
# database from thirdparty/tzdata (with the host's zic: without it, the ROM has none, and says so
# here), then the image (and rom/files/, when there is such a tree), refused if it outgrows its
# partition; idf.py flash writes it. Its content hash becomes romhash.h, which RomDisk compares
# with what it finds. The recovery firmware (devplan/update.md §4) builds no ROM: it may be what
# is repaired, so its RomContentHash matches none.
if(CONFIG_BMDS_RECOVERY AND NOT BOOTLOADER_BUILD)
        set(BMDS_ROM_HASH ${CMAKE_CURRENT_BINARY_DIR}/romhash.h)
        file(WRITE ${BMDS_ROM_HASH}.new "// Recovery: no ROM built with it (components/bmds_storage).\n"
                                        "#pragma once\n#include <cstdint>\n\nnamespace {\n\n"
                                        "        constexpr uint8_t RomContentHash[32] = {};\n\n} // namespace\n")
        configure_file(${BMDS_ROM_HASH}.new ${BMDS_ROM_HASH} COPYONLY)
        target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
elseif(NOT BOOTLOADER_BUILD)
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
        find_program(BMDS_ZIC zic PATHS /usr/sbin /sbin)
        set(BMDS_ROM_ZONES)
        set(BMDS_ROM_ZONES_FILE)
        if(BMDS_ZIC)
                set(BMDS_ROM_ZONES_FILE ${BMDS_ROM_OUT}/zoneinfo.bin)
                set(BMDS_TZBUNDLE ${CMAKE_CURRENT_LIST_DIR}/../../tools/tzbundle)
                file(GLOB BMDS_TZDATA ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/tzdata/*)
                add_custom_command(OUTPUT ${BMDS_ROM_ZONES_FILE}
                                   COMMAND ${CMAKE_COMMAND} -E env ZIC=${BMDS_ZIC} ${BMDS_STORAGE_PYTHON}
                                           ${BMDS_TZBUNDLE} ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/tzdata
                                           ${BMDS_ROM_ZONES_FILE}
                                   DEPENDS ${BMDS_TZBUNDLE} ${BMDS_TZDATA}
                                   VERBATIM)
                set(BMDS_ROM_ZONES --file share/zoneinfo.bin=${BMDS_ROM_ZONES_FILE})
        else()
                message(STATUS "zic not found: the ROM has no time zone database (devplan/rom.md §3g)")
        endif()
        # The SSH programs (devplan/network.md §24): Dropbear and our sftp-server, built by
        # tools/bmds-dist into this build directory (nested: no build wrapper, one job at a time;
        # Dropbear's release is fetched once into ~/.cache/bmds-dist) and put in /rom/bin.
        set(BMDS_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
        set(BMDS_ROM_DIST ${BMDS_ROM_OUT}/dist)
        set(BMDS_ROM_DROPBEAR ${BMDS_ROM_DIST}/stage/dropbear)
        set(BMDS_ROM_SFTP ${BMDS_ROM_DIST}/stage/sftp-server)
        file(GLOB_RECURSE BMDS_ROM_DIST_SOURCES CONFIGURE_DEPENDS ${BMDS_ROOT_DIR}/dist/dropbear/*
             ${BMDS_ROOT_DIR}/dist/sftp-server/* ${BMDS_ROOT_DIR}/programs/sftp-server/*
             ${BMDS_ROOT_DIR}/sdk/crt/* ${BMDS_ROOT_DIR}/sdk/include/* ${BMDS_ROOT_DIR}/sdk/cmake/*
             ${BMDS_ROOT_DIR}/sdk/ld/*)
        add_custom_command(OUTPUT ${BMDS_ROM_DROPBEAR}/bin/dropbearmulti ${BMDS_ROM_SFTP}/bin/sftp-server
                           COMMAND ${BMDS_STORAGE_PYTHON} ${BMDS_ROOT_DIR}/tools/bmds-dist --out ${BMDS_ROM_DIST}
                                   --nested build dropbear sftp-server
                           DEPENDS ${BMDS_ROOT_DIR}/tools/bmds-dist ${BMDS_ROM_DIST_SOURCES}
                           VERBATIM)
        set(BMDS_ROM_SSH
            --file bin/dropbearmulti=${BMDS_ROM_DROPBEAR}/bin/dropbearmulti
            --link bin/dropbear=dropbearmulti --link bin/dbclient=dropbearmulti --link bin/ssh=dropbearmulti
            --link bin/dropbearkey=dropbearmulti --link bin/dropbearconvert=dropbearmulti
            --link bin/scp=dropbearmulti
            --file bin/sftp-server=${BMDS_ROM_SFTP}/bin/sftp-server
            --file share/licenses/dropbear/LICENSE=${BMDS_ROM_DROPBEAR}/share/licenses/dropbear/LICENSE)
        # IANA's leap-second list (devplan/network.md §31e): CLOCK_TAI and PTP's UTC offset; a newer one
        # on the card (clock/leapseconds) wins.
        set(BMDS_ROM_LEAP_FILE ${BMDS_ROOT_DIR}/thirdparty/tzdata/leap-seconds.list)
        set(BMDS_ROM_LEAP --file share/leap-seconds.list=${BMDS_ROM_LEAP_FILE})
        # Text to speech (devplan/speech.md): the en-US voice, read in place; Pico's notices.
        set(BMDS_ROM_PICO ${BMDS_ROOT_DIR}/thirdparty/svox-pico)
        set(BMDS_ROM_SPEECH_FILES ${BMDS_ROM_PICO}/lang/en-US_ta.bin ${BMDS_ROM_PICO}/lang/en-US_lh0_sg.bin
                                  ${BMDS_ROM_PICO}/NOTICE ${BMDS_ROM_PICO}/lang/NOTICE)
        set(BMDS_ROM_SPEECH
            --file share/speech/en-US_ta.bin=${BMDS_ROM_PICO}/lang/en-US_ta.bin
            --file share/speech/en-US_lh0_sg.bin=${BMDS_ROM_PICO}/lang/en-US_lh0_sg.bin
            --file share/licenses/svox-pico/NOTICE=${BMDS_ROM_PICO}/NOTICE
            --file share/licenses/svox-pico/voices.NOTICE=${BMDS_ROM_PICO}/lang/NOTICE)
        # The gamepads' mappings (devplan/controls.md §7a): SDL's line format, our own entries.
        set(BMDS_ROM_PADS_FILE ${BMDS_ROOT_DIR}/rom/gamepads.txt)
        set(BMDS_ROM_PADS --file share/gamepads.txt=${BMDS_ROM_PADS_FILE})
        add_custom_command(OUTPUT ${BMDS_ROM_BIN} ${BMDS_ROM_HASH}
                           COMMAND ${BMDS_STORAGE_PYTHON} ${BMDS_MKROM} image ${BMDS_ROM_BIN}
                                   --max-bytes ${BMDS_ROM_PARTITION_BYTES} ${BMDS_ROM_TREE}
                                   --file share/icons/bmds.icons=${BMDS_ROM_OUT}/bmds.icons
                                   --file share/icons/bmds.LICENSES=${BMDS_ROM_OUT}/bmds.LICENSES
                                   ${BMDS_ROM_ZONES} ${BMDS_ROM_LEAP} ${BMDS_ROM_SSH} ${BMDS_ROM_SPEECH}
                                   ${BMDS_ROM_PADS} --hash-header ${BMDS_ROM_HASH}
                           DEPENDS ${BMDS_MKROM} ${BMDS_ROM_OUT}/bmds.icons ${BMDS_ROM_OUT}/bmds.LICENSES
                                   ${BMDS_ROM_DROPBEAR}/bin/dropbearmulti ${BMDS_ROM_SFTP}/bin/sftp-server
                                   ${BMDS_ROM_TREE_FILES} ${BMDS_ROM_ZONES_FILE} ${BMDS_ROM_LEAP_FILE}
                                   ${BMDS_ROM_SPEECH_FILES} ${BMDS_ROM_PADS_FILE}
                           VERBATIM)
        target_sources(${COMPONENT_LIB} PRIVATE ${BMDS_ROM_HASH})
        target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        add_custom_target(bmds_rom_image ALL DEPENDS ${BMDS_ROM_BIN})
        add_dependencies(flash bmds_rom_image)
        esptool_py_flash_to_partition(flash rom ${BMDS_ROM_BIN})
endif()
