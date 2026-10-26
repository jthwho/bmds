# bmds_ui — the desktop and its windows (devplan/windows.md).
#
# The pure parts (the compose plan, the window stack, the glyph renderer) are also built
# as a plain CMake library for the host tests (tests/host). Fonts are bmds_fonts'.
#
# WHOLE_ARCHIVE: nothing references wincommand.cpp by symbol -- its command
# is found through the linker section.

set(BMDS_UI_PURE_SRCS
        src/bevel.cpp
        src/compose.cpp
        src/glyphrenderer.cpp
        src/menulayout.cpp
        src/mnemonic.cpp
        src/servicequeue.cpp
        src/switcherlayout.cpp
        src/terminalselection.cpp
        src/terminalview.cpp
        src/windowpalette.cpp
        src/windowplacement.cpp
        src/windowstack.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_UI_PURE_SRCS}
                                    src/bevelpainter.cpp
                                    src/desktopmenu.cpp
                                    src/fontcommand.cpp
                                    src/menupainter.cpp
                                    src/painter.cpp
                                    src/programsurface.cpp
                                    src/swapchain.cpp
                                    src/png.cpp
                                    src/screenshot.cpp
                                    src/screenshotcommand.cpp
                                    src/wincommand.cpp
                                    src/windowmanager.cpp
                                    src/wmconfig.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_console bmds_core bmds_drivers bmds_fonts bmds_input bmds_kernel
                               PRIV_REQUIRES bmds_bench bmds_fs bmds_shell bmds_sysconf esp_mm esp_rom
                               WHOLE_ARCHIVE)
        # Screenshots move megabytes through the filters: speed over size here.
        set_source_files_properties(src/png.cpp PROPERTIES COMPILE_OPTIONS -O2)
        set(BMDS_UI_LIB ${COMPONENT_LIB})
else()
        add_library(bmds_ui STATIC ${BMDS_UI_PURE_SRCS})
        target_include_directories(bmds_ui PUBLIC include)
        target_link_libraries(bmds_ui PUBLIC bmds_core bmds_console bmds_fonts bmds_input)
        set(BMDS_UI_LIB bmds_ui)
endif()

bmds_target_options(${BMDS_UI_LIB})
