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
        src/flashwrites.cpp
        src/interrupt.cpp
        src/memorystream.cpp
        src/notifications.cpp
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
                                    src/logstream.cpp
                                    src/memory.cpp
                                    src/mutex.cpp
                                    src/platform.cpp
                                    src/semaphore.cpp
                                    src/system.cpp
                                    src/simdsetup.cpp
                                    src/task.cpp
                                    src/tasklist.cpp
                                    src/thread.cpp
                                    src/updatekeys.cpp
                                    src/waketimer.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_board freertos
                               PRIV_REQUIRES app_update bootloader_support esp_app_format esp_driver_gptimer esp_hw_support esp_psram esp_system esp_timer heap log
                               LDFRAGMENTS linker.lf)
        bmds_target_options(${COMPONENT_LIB})
        # esp_timer_get_time() is Detail::monotonicUs() (platform.cpp): the same counter without the
        # ROM's 64-bit division (devplan/bench.md §15).
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_timer_get_time")
        bmds_update_keys(${COMPONENT_LIB} updatekeys.inc "${CONFIG_BMDS_UPDATE_KEYS_RELEASE}")
else()
        find_package(Threads REQUIRED)
        # No clockinit.cpp: on the host nothing survives a "reset".
        add_library(bmds_kernel STATIC ${BMDS_KERNEL_PORTABLE_SRCS} src/host/platform.cpp src/host/waketimer.cpp)
        target_include_directories(bmds_kernel PUBLIC include)
        target_link_libraries(bmds_kernel PUBLIC bmds_core Threads::Threads)
        bmds_target_options(bmds_kernel)
endif()
