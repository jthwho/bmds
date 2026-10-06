# bmds_terminal — the terminal application, /bin/terminal (devplan/windows.md §8
# step 9): an ordinary GUI program on the window manager's API, a pseudo-terminal
# and a shell, built into the firmware only to have it at hand. It has no API of
# its own: nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found
# through the linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/terminalapp.cpp
                       PRIV_REQUIRES bmds_console bmds_core bmds_input bmds_kernel bmds_shell bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
