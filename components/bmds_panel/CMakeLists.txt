# bmds_panel — the panel, /bin/panel (devplan/gui.md §4c): an ordinary GUI
# program on the toolkit (bmds_gui), built into the firmware only to have it
# at hand: the desktop, the taskbar and the folder windows (devplan/filebrowser.md
# §4h), with `files`, the command that asks it for folder windows. It has no API
# of its own: nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found
# through the linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/desktopwindow.cpp src/fileicons.cpp src/filescommand.cpp src/folderactions.cpp
                            src/folderitems.cpp src/folderwindow.cpp src/panelapp.cpp src/panelentries.cpp
                       PRIV_REQUIRES bmds_clipboard bmds_core bmds_files bmds_fs bmds_gui bmds_input bmds_kernel bmds_shell
                                     bmds_sysconf bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})

# Its icons, built in (devplan/gui.md §4j): the flat-color theme's services.
set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
bmds_embed_icons(${COMPONENT_LIB} panelicons.h PanelIcons ${BMDS_ICONS}/16x16/services.png ${BMDS_ICONS}/32x32/services.png)
