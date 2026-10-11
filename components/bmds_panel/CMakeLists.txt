# bmds_panel — the panel, /bin/panel (devplan/gui.md §4c): an ordinary GUI
# program on the toolkit (bmds_gui), built into the firmware only to have it
# at hand: the desktop, the taskbar and the folder windows (devplan/filebrowser.md
# §4h), with `files`, the command that asks it for folder windows. It has no API
# of its own: nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found
# through the linker section (BMDS_PROGRAM).

# Its pure parts (the CPU meter's picture, the network icon's picture and text, panel/items'
# edits) are also built as a plain CMake library for the host tests (tests/host).

set(BMDS_PANEL_PURE_SRCS
        src/cpumeterpicture.cpp
        src/networkview.cpp
        src/panelitems.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_PANEL_PURE_SRCS}
                                    src/cpumeter.cpp src/desktopwindow.cpp src/filejobs.cpp src/filescommand.cpp
                                    src/folderactions.cpp src/folderwindow.cpp src/networkstatus.cpp src/panelapp.cpp
                                    src/panelentries.cpp src/notifyarea.cpp src/notificationserver.cpp src/screenshottaker.cpp
                                    src/volumecontrol.cpp
                               PRIV_REQUIRES bmds_audio bmds_clipboard bmds_core bmds_files bmds_fs bmds_gui bmds_input bmds_kernel
                                             bmds_net bmds_shell bmds_sysconf bmds_ui
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})

        # Its icons, built in (devplan/gui.md §4j): the flat-color theme's services.
        set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
        bmds_embed_icons(${COMPONENT_LIB} panelicons.h PanelIcons ${BMDS_ICONS}/16x16/services.png
                         ${BMDS_ICONS}/32x32/services.png)
else()
        add_library(bmds_panel STATIC ${BMDS_PANEL_PURE_SRCS})
        target_include_directories(bmds_panel PUBLIC src)
        target_link_libraries(bmds_panel PUBLIC bmds_core bmds_gui bmds_net)
        bmds_target_options(bmds_panel)
endif()
