# bmds_fs — block devices, the filesystems (FatFs and littlefs, vendored in
# thirdparty/fatfs and thirdparty/littlefs) and the VFS.
#
# Portable: no ESP-IDF headers. Also built as a plain CMake library for the
# host tests (tests/host), where RamBlockDevice stands in for the SD card.
# FatFs's headers (and our ffconf.h) stay private to this component.

set(BMDS_FATFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/fatfs)
set(BMDS_LFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/littlefs)

set(BMDS_FS_SRCS
        src/automount.cpp
        src/blockdevice.cpp
        src/blockdevices.cpp
        src/fatfile.cpp
        src/fatfsport.cpp
        src/fatvolume.cpp
        src/flashqueue.cpp
        src/fslayer.cpp
        src/identify.cpp
        src/lfsvolume.cpp
        src/partitions.cpp
        src/ramblockdevice.cpp
        src/ramflashdevice.cpp
        src/sectorcache.cpp
        src/trash.cpp
        src/vfs.cpp
        src/zoneinfo.cpp
        ${BMDS_FATFS_DIR}/ff.c
        ${BMDS_FATFS_DIR}/ffunicode.c
        ${BMDS_LFS_DIR}/lfs.c
        ${BMDS_LFS_DIR}/lfs_util.c)

set(BMDS_FS_PRIV_INCLUDES src src/fatfs ${BMDS_FATFS_DIR} ${BMDS_LFS_DIR})

# littlefs without its heap (every buffer supplied) or messages (errors are returned); its own
# code shadows variables, so -Wshadow is off for its files alone (thirdparty/littlefs/README.md),
# set after the component is registered (IDF first reads this file as a script).
set(BMDS_LFS_DEFINITIONS LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_FS_SRCS}
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_FS_PRIV_INCLUDES}
                               REQUIRES bmds_core bmds_kernel)
        bmds_target_options(${COMPONENT_LIB})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${BMDS_LFS_DEFINITIONS})
        set_source_files_properties(${BMDS_LFS_DIR}/lfs.c ${BMDS_LFS_DIR}/lfs_util.c PROPERTIES COMPILE_OPTIONS
                                    -Wno-shadow)
else()
        add_library(bmds_fs STATIC ${BMDS_FS_SRCS})
        target_include_directories(bmds_fs PUBLIC include PRIVATE ${BMDS_FS_PRIV_INCLUDES})
        target_link_libraries(bmds_fs PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_fs)
        target_compile_definitions(bmds_fs PRIVATE ${BMDS_LFS_DEFINITIONS})
        set_source_files_properties(${BMDS_LFS_DIR}/lfs.c ${BMDS_LFS_DIR}/lfs_util.c PROPERTIES COMPILE_OPTIONS
                                    -Wno-shadow)
endif()
