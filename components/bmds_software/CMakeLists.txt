# bmds_software — packages for the user (devplan/packages.md §24, §18): /bin/welcome, the first
# run's window, and later the store. Ordinary GUI programs on the toolkit, driving `pkg --json`
# (src/pkgjob.h) as any program could.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entries are found through the
# linker section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/pkgjob.cpp
                            src/welcomeapp.cpp
                       PRIV_REQUIRES bmds_core bmds_fs bmds_gui bmds_kernel bmds_pkg bmds_sysconf bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
