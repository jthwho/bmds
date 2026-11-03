# bmds_format — /bin/format (devplan/format.md §4): a card, what is on it, and Format..., which
# does what mkfs does to a whole card. An ordinary GUI program on the toolkit; opened by the blank
# card's question and by Files.
#
# WHOLE_ARCHIVE: nothing references it by symbol -- its program entry is found through the linker
# section (BMDS_PROGRAM).

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

idf_component_register(SRCS src/formatapp.cpp
                       PRIV_REQUIRES bmds_core bmds_fs bmds_gui bmds_kernel bmds_storage bmds_sysconf bmds_ui
                       WHOLE_ARCHIVE)
bmds_target_options(${COMPONENT_LIB})
