# bmds_edit — /bin/edit, the text editor (devplan/editor.md §8): documents in tabs on the toolkit's
# TextEdit, find and replace, files saved safely; an ordinary GUI program on the public toolkit,
# built into the firmware only to have it at hand. Nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references the program by symbol -- its entry is found through the
# linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS src/angelsupport.cpp
                                    src/debugsession.cpp
                                    src/editapp.cpp
                                    src/editdebug.cpp
                                    src/editrun.cpp
                                    src/editwindow.cpp
                               PRIV_REQUIRES bmds_core bmds_fs bmds_gui bmds_input bmds_kernel bmds_shell bmds_sysconf
                                             bmds_ui
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})

        # Its icons, built in (devplan/gui.md §4j): the flat-color theme's document, as the bmds
        # theme's accessories-text-editor.
        set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
        bmds_embed_icons(${COMPONENT_LIB} editicons.h EditIcons ${BMDS_ICONS}/16x16/document.png
                         ${BMDS_ICONS}/32x32/document.png)
endif()
