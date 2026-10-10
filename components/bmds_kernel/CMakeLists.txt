# bmds_kernel — boot sequence, logging, memory spaces, task/IPC wrappers,
# streams and pipes (and later: interrupt dispatch, device model, VFS).
#
# The synchronization classes, Interrupt and the stream layer have
# FreeRTOS-free headers. They are also built as a plain CMake library for
# the host tests (tests/host), with src/host/ standing in for the platform
# layer (src/platform.cpp, src/mutex.cpp, src/semaphore.cpp) on top of the
# C++ thread library.

set(BMDS_KERNEL_PORTABLE_SRCS
        src/builtinprograms.cpp
        src/clock.cpp
        src/fdtable.cpp
        src/filehandle.cpp
        src/interrupt.cpp
        src/memorystream.cpp
        src/pipe.cpp
        src/random.cpp
        src/stats.cpp
        src/taskowner.cpp
        src/waitlist.cpp
        src/workerpool.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_KERNEL_PORTABLE_SRCS}
                                    src/assert.cpp
                                    src/boot.cpp
                                    src/clockinit.cpp
                                    src/esperr_check.cpp
                                    src/log.cpp
                                    src/memory.cpp
                                    src/mutex.cpp
                                    src/platform.cpp
                                    src/semaphore.cpp
                                    src/system.cpp
                                    src/task.cpp
                                    src/tasklist.cpp
                                    src/thread.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_board freertos
                               PRIV_REQUIRES bootloader_support esp_app_format esp_hw_support esp_psram esp_system esp_timer heap log
                               LDFRAGMENTS linker.lf)
        bmds_target_options(${COMPONENT_LIB})
else()
        find_package(Threads REQUIRED)
        # No clockinit.cpp: on the host nothing survives a "reset".
        add_library(bmds_kernel STATIC ${BMDS_KERNEL_PORTABLE_SRCS} src/host/platform.cpp)
        target_include_directories(bmds_kernel PUBLIC include)
        target_link_libraries(bmds_kernel PUBLIC bmds_core Threads::Threads)
        bmds_target_options(bmds_kernel)
endif()
