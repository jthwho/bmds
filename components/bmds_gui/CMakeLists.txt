# bmds_gui — the GUI toolkit (devplan/gui.md): widgets, menus, an event loop,
# over the window manager's public API only, so that it can move into the SDK
# once GUI programs have an ABI.
#
# The pure parts (line layout, the icon grid, scroll bars, sliders and
# furniture; the menu's geometry is bmds_ui's, shared with the window manager's
# window menu) are also built as a plain CMake library for the host tests
# (tests/host).

set(BMDS_GUI_PURE_SRCS
        src/furniturelayout.cpp
        src/icongrid.cpp
        src/itemlayout.cpp
        src/itemmodel.cpp
        src/itemselection.cpp
        src/lineeditmodel.cpp
        src/linelayout.cpp
        src/scrollmodel.cpp
        src/slidermodel.cpp
        src/treerows.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_GUI_PURE_SRCS}
                                    src/detailsview.cpp
                                    src/dialog.cpp
                                    src/furniture.cpp
                                    src/guiapplication.cpp
                                    src/guiclipboard.cpp
                                    src/guiwindow.cpp
                                    src/iconview.cpp
                                    src/icontheme.cpp
                                    src/itemview.cpp
                                    src/lineedit.cpp
                                    src/menu.cpp
                                    src/menubar.cpp
                                    src/messagebox.cpp
                                    src/popupedit.cpp
                                    src/scrollarea.cpp
                                    src/slider.cpp
                                    src/tooltip.cpp
                                    src/treeview.cpp
                                    src/widget.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_clipboard bmds_core bmds_fs bmds_input bmds_kernel bmds_loader bmds_sysconf bmds_ui)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_gui STATIC ${BMDS_GUI_PURE_SRCS})
        target_include_directories(bmds_gui PUBLIC include)
        target_link_libraries(bmds_gui PUBLIC bmds_core bmds_sysconf bmds_ui)
        bmds_target_options(bmds_gui)
endif()
