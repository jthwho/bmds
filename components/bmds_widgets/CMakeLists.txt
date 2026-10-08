# bmds_widgets — /bin/widgets (devplan/filebrowser.md §4e): a window showing the toolkit's
# furniture (menu bar, tool bar, splitter, scroll areas, status bar, dialogs, a progress bar),
# an ordinary GUI program on the public toolkit, to try them on the board.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found
# through the linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/widgetsapp.cpp
                       PRIV_REQUIRES bmds_core bmds_fs bmds_gui bmds_kernel bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
