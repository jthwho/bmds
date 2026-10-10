# bmds_loader — BMDS programs: the executable format, the loader, the system
# call table (devplan/programs.md).
#
# Portable parts (the format check and relocation, Process, Signals, the ABI lock)
# also build as a plain CMake library for the host tests and tools; the
# loader, the system call table and its slots are target only. The SDK's headers
# (sdk/include) are the userspace ABI the kernel implements.

set(BMDS_SDK_INCLUDE ${CMAKE_CURRENT_LIST_DIR}/../../sdk/include)

set(BMDS_LOADER_PURE_SRCS
        src/abilock.c
        src/futex.cpp
        src/imagecache.cpp
        src/process.cpp
        src/processtable.cpp
        src/programicons.cpp
        src/programimage.cpp
        src/signals.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_LOADER_PURE_SRCS}
                                    src/audiocalls.cpp
                                    src/loader.cpp
                                    src/midicalls.cpp
                                    src/notifycalls.cpp
                                    src/proccommand.cpp
                                    src/statscalls.cpp
                                    src/syscalls.cpp
                                    src/systable.c
                               INCLUDE_DIRS include ${BMDS_SDK_INCLUDE}
                               REQUIRES bmds_core bmds_kernel bmds_console bmds_fs bmds_sysconf
                               PRIV_REQUIRES bmds_audio bmds_midi bmds_stats bmds_synth bmds_shell esp_hw_support esp_mm freertos
                               # proccommand.cpp's commands are found through their linker
                               # section, not by symbol.
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_loader STATIC ${BMDS_LOADER_PURE_SRCS})
        target_include_directories(bmds_loader PUBLIC include ${BMDS_SDK_INCLUDE})
        target_link_libraries(bmds_loader PUBLIC bmds_core bmds_kernel bmds_console bmds_fs bmds_sysconf)
        bmds_target_options(bmds_loader)
endif()
