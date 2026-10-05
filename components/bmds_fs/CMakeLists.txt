# bmds_fs — block devices, the filesystem (FatFs, vendored in thirdparty/fatfs)
# and the VFS.
#
# Portable: no ESP-IDF headers. Also built as a plain CMake library for the
# host tests (tests/host), where RamBlockDevice stands in for the SD card.
# FatFs's headers (and our ffconf.h) stay private to this component.

set(BMDS_FATFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/fatfs)

set(BMDS_FS_SRCS
        src/automount.cpp
        src/blockdevice.cpp
        src/blockdevices.cpp
        src/fatfile.cpp
        src/fatfsport.cpp
        src/fatvolume.cpp
        src/partitions.cpp
        src/ramblockdevice.cpp
        src/sectorcache.cpp
        src/vfs.cpp
        ${BMDS_FATFS_DIR}/ff.c
        ${BMDS_FATFS_DIR}/ffunicode.c)

set(BMDS_FS_PRIV_INCLUDES src src/fatfs ${BMDS_FATFS_DIR})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_FS_SRCS}
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_FS_PRIV_INCLUDES}
                               REQUIRES bmds_core bmds_kernel)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_fs STATIC ${BMDS_FS_SRCS})
        target_include_directories(bmds_fs PUBLIC include PRIVATE ${BMDS_FS_PRIV_INCLUDES})
        target_link_libraries(bmds_fs PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_fs)
endif()
