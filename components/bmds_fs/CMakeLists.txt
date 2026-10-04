# bmds_fs — block devices, the filesystem and the VFS.
#
# Portable: no ESP-IDF headers. Also built as a plain CMake library for the
# host tests (tests/host), where RamBlockDevice stands in for the SD card.

set(BMDS_FS_SRCS
        src/blockdevice.cpp
        src/ramblockdevice.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_FS_SRCS}
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_fs STATIC ${BMDS_FS_SRCS})
        target_include_directories(bmds_fs PUBLIC include)
        target_link_libraries(bmds_fs PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_fs)
endif()
