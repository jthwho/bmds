# bmds_patchbay — /bin/patchbay, the patchbay (devplan/audio.md §24): MIDI ports and their
# connections in a window, made and ended by dragging or by keys; an ordinary GUI program on the
# public toolkit and the kernel's public classes (through its PatchSource), built into the
# firmware only to have it at hand. Nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references the program by symbol -- its entry is found through the
# linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/kernelpatchsource.cpp
                            src/patchbayapp.cpp
                            src/patchview.cpp
                       PRIV_REQUIRES bmds_core bmds_gui bmds_input bmds_kernel bmds_midi bmds_sysconf bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})

# Its icons, built in (devplan/gui.md §4j): the flat-color theme's flow chart.
set(BMDS_ICONS ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/flat-color-icons/png)
bmds_embed_icons(${COMPONENT_LIB} patchbayicons.h PatchbayIcons ${BMDS_ICONS}/16x16/flow_chart.png
                 ${BMDS_ICONS}/32x32/flow_chart.png)
