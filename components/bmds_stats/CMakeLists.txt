# bmds_stats — how the system is behaving (devplan/sysmon.md): the tasks' CPU time, switched per
# boot by sysmon/enable; the system-wide counters (cores, memory, clock) in the kernel's registry
# (Stats); the monitor's settings; the `stats` command. Target-only.
#
# WHOLE_ARCHIVE: the parameters and the command are found through their linker sections.

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/cputime.cpp
                            src/statscommand.cpp
                            src/sysmon.cpp
                       INCLUDE_DIRS include
                       REQUIRES bmds_core bmds_kernel
                       PRIV_REQUIRES bmds_shell bmds_sysconf esp_timer freertos
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})

# FreeRTOS reads its run-time clock through xPortGetRunTimeCounterValue() (portable/riscv/port.c)
# at every context switch; cputime.cpp's __wrap_ answers instead, the clock or 0 by the setting.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xPortGetRunTimeCounterValue")
