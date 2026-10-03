# bmds_board — board definitions (all hardware truth) and MemSpace mapping.
#
# The board is selected by Kconfig (BMDS_BOARD); its directory under boards/
# provides <bmds/boarddef.h>.

idf_component_register(INCLUDE_DIRS include "boards/${CONFIG_BMDS_BOARD_DIR}"
                       REQUIRES bmds_core heap)
