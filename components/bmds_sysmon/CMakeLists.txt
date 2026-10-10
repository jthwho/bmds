# bmds_sysmon — /bin/sysmon, the System Monitor (devplan/sysmon.md §3.5): how the system is
# behaving, down to every subsystem's counters, with whatever is out of normal in amber or red; an
# ordinary GUI program on the public toolkit and the kernel's public classes (through its Source),
# built into the firmware only to have it at hand. Nothing in the OS refers to it. The shell's
# `top` and `tasks` read through the same Source.
#
# Its pure part (Monitor: the counters' catalogue, rates, health, text) is also built as a plain
# CMake library for the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references the program by symbol -- its entry is found through the
# linker section (BMDS_PROGRAM).

set(BMDS_SYSMON_PURE_SRCS
        src/monitor.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_SYSMON_PURE_SRCS}
                                    src/kernelsource.cpp
                                    src/sysmonapp.cpp
                                    src/sysmonwindow.cpp
                                    src/tablemodel.cpp
                                    src/taskscommand.cpp
                                    src/topcommand.cpp
                               PRIV_REQUIRES bmds_console bmds_core bmds_fs bmds_gui bmds_kernel bmds_loader bmds_shell
                                             bmds_stats bmds_ui
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})

        # Its icons, built in (devplan/gui.md §4j): the flat-color theme's line chart, as the bmds
        # theme's utilities-system-monitor.
        set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
        bmds_embed_icons(${COMPONENT_LIB} sysmonicons.h SysmonIcons ${BMDS_ICONS}/16x16/line_chart.png
                         ${BMDS_ICONS}/32x32/line_chart.png)
else()
        add_library(bmds_sysmon STATIC ${BMDS_SYSMON_PURE_SRCS})
        target_include_directories(bmds_sysmon PUBLIC src)
        target_link_libraries(bmds_sysmon PUBLIC bmds_core)
        bmds_target_options(bmds_sysmon)
endif()
