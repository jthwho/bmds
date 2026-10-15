# bmds_controls — /bin/controls, the Controls window and its text form (devplan/controls.md §7d):
# applications' actions and their keys and pad controls, changed and saved in the user's files. An
# ordinary GUI program on the public toolkit and the kernel's API, built into the firmware only to
# have it at hand. Nothing in the OS refers to it.
#
# WHOLE_ARCHIVE: nothing references the program by symbol -- its entry is found through the
# linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS src/controlsapp.cpp
                                    src/controlsbook.cpp
                                    src/controlswindow.cpp
                               PRIV_REQUIRES bmds_core bmds_files bmds_fs bmds_gui bmds_input bmds_kernel bmds_ui
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
endif()
