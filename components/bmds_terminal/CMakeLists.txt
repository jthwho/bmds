# bmds_terminal — the terminal application, /bin/terminal (devplan/windows.md §8
# step 9): an ordinary GUI program on the window manager's API, a pseudo-terminal
# and a shell, built into the firmware only to have it at hand. It has no API of
# its own: nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found
# through the linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/terminalapp.cpp
                       PRIV_REQUIRES bmds_audio bmds_clipboard bmds_console bmds_core bmds_fs bmds_gui bmds_input bmds_kernel bmds_sysconf bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})

# Its icons, built in (devplan/gui.md §4j): the flat-color theme's command_line.
set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
bmds_embed_icons(${COMPONENT_LIB} terminalicons.h TerminalIcons ${BMDS_ICONS}/16x16/command_line.png ${BMDS_ICONS}/32x32/command_line.png)
