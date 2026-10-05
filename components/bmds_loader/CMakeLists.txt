# bmds_loader — BMDS programs: the executable format, the loader, the system
# call table (devplan/programs.md).
#
# Portable parts (the format check and relocation, Process, the ABI lock)
# also build as a plain CMake library for the host tests and tools; the
# loader, the system call table and its slots are target only. The SDK's headers
# (sdk/include) are the userspace ABI the kernel implements.

set(BMDS_SDK_INCLUDE ${CMAKE_CURRENT_LIST_DIR}/../../sdk/include)

set(BMDS_LOADER_PURE_SRCS
        src/abilock.c
        src/process.cpp
        src/processtable.cpp
        src/programimage.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_LOADER_PURE_SRCS}
                                    src/loader.cpp
                                    src/syscalls.cpp
                                    src/systable.c
                               INCLUDE_DIRS include ${BMDS_SDK_INCLUDE}
                               REQUIRES bmds_core bmds_kernel bmds_console bmds_fs
                               PRIV_REQUIRES esp_hw_support esp_mm freertos)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_loader STATIC ${BMDS_LOADER_PURE_SRCS})
        target_include_directories(bmds_loader PUBLIC include ${BMDS_SDK_INCLUDE})
        target_link_libraries(bmds_loader PUBLIC bmds_core bmds_kernel bmds_console bmds_fs)
        bmds_target_options(bmds_loader)
endif()
