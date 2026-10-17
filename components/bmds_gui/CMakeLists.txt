# bmds_gui — the GUI toolkit (devplan/gui.md): widgets, menus, an event loop,
# over the window manager's public API only, so that it can move into the SDK
# once GUI programs have an ABI.
#
# The pure parts (line layout, the icon grid, scroll bars, sliders, tab rows,
# a file dialog's names and furniture, forms' models; the menu's geometry is bmds_ui's,
# shared with the window manager's window menu) are also built as a plain
# CMake library for the host tests (tests/host).

set(BMDS_GUI_PURE_SRCS
        src/filenamelist.cpp
        src/formmodels.cpp
        src/furniturelayout.cpp
        src/graphlayout.cpp
        src/icongrid.cpp
        src/itemlayout.cpp
        src/itemmodel.cpp
        src/itemselection.cpp
        src/lineeditmodel.cpp
        src/linelayout.cpp
        src/scrollmodel.cpp
        src/slidermodel.cpp
        src/tabrow.cpp
        src/trayrow.cpp
        src/treerows.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_GUI_PURE_SRCS}
                                    src/colordialog.cpp
                                    src/combobox.cpp
                                    src/detailsview.cpp
                                    src/dialog.cpp
                                    src/filedialog.cpp
                                    src/fileicons.cpp
                                    src/folderitemmodel.cpp
                                    src/formlayout.cpp
                                    src/furniture.cpp
                                    src/graph.cpp
                                    src/guiapplication.cpp
                                    src/guiclipboard.cpp
                                    src/guiwindow.cpp
                                    src/iconview.cpp
                                    src/icontheme.cpp
                                    src/itemview.cpp
                                    src/lineedit.cpp
                                    src/listeditor.cpp
                                    src/menu.cpp
                                    src/menubar.cpp
                                    src/messagebox.cpp
                                    src/notifier.cpp
                                    src/numberfield.cpp
                                    src/popupedit.cpp
                                    src/scrollarea.cpp
                                    src/scrollview.cpp
                                    src/slider.cpp
                                    src/tabwidget.cpp
                                    src/tooltip.cpp
                                    src/trayicon.cpp
                                    src/treeview.cpp
                                    src/widget.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_clipboard bmds_core bmds_files bmds_fs bmds_input bmds_kernel bmds_loader bmds_sysconf bmds_ui)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_gui STATIC ${BMDS_GUI_PURE_SRCS})
        target_include_directories(bmds_gui PUBLIC include)
        target_link_libraries(bmds_gui PUBLIC bmds_core bmds_sysconf bmds_ui)
        bmds_target_options(bmds_gui)
endif()
