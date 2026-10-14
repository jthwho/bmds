# bmds_updater — /bin/updater (devplan/update.md §14): an update package's versions, checks and
# release notes, and Install. An ordinary GUI program on the toolkit and bmds/update.h.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found through the linker
# section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/updaterapp.cpp
                       PRIV_REQUIRES bmds_core bmds_files bmds_fs bmds_gui bmds_kernel bmds_sysconf bmds_ui
                                     bmds_update
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
