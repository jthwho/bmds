# bmds_panel — the panel, /bin/panel (devplan/gui.md §4c): an ordinary GUI
# program on the toolkit (bmds_gui), built into the firmware only to have it
# at hand. It has no API of its own: nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found
# through the linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/panelapp.cpp
                       PRIV_REQUIRES bmds_core bmds_fs bmds_gui bmds_input bmds_kernel bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
