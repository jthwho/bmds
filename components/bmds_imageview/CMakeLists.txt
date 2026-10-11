# bmds_imageview — /bin/imageview, the image viewer (devplan/images.md §4): a picture fitted to its
# window or zoomed and panned, the folder's other pictures a key away; an ordinary GUI program on
# the public toolkit and the system's Images, built into the firmware only to have it at hand.
# Nothing in the OS refers to it.
#
# Its pure part (ViewLayout: fit, zoom, pan) is also built as a plain CMake library for the host
# tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references the program by symbol -- its entry is found through the
# linker section (BMDS_PROGRAM).

set(BMDS_IMAGEVIEW_PURE_SRCS
        src/viewlayout.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_IMAGEVIEW_PURE_SRCS}
                                    src/imageviewapp.cpp
                                    src/imagewindow.cpp
                               PRIV_REQUIRES bmds_core bmds_files bmds_fs bmds_gui bmds_images bmds_input bmds_kernel
                                             bmds_ui
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})

        # Its icons, built in (devplan/gui.md §4j): the flat-color theme's picture, as the bmds
        # theme's image-viewer.
        set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
        bmds_embed_icons(${COMPONENT_LIB} imageviewicons.h ImageviewIcons ${BMDS_ICONS}/16x16/picture.png
                         ${BMDS_ICONS}/32x32/picture.png)
else()
        add_library(bmds_imageview STATIC ${BMDS_IMAGEVIEW_PURE_SRCS})
        target_include_directories(bmds_imageview PUBLIC src)
        target_link_libraries(bmds_imageview PUBLIC bmds_core)
        bmds_target_options(bmds_imageview)
endif()
