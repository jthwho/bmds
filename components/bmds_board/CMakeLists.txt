# bmds_board — board definitions (all hardware truth) and MemSpace mapping.
#
# The board is selected by Kconfig (BMDS_BOARD); its directory under boards/
# provides <bmds/boarddef.h>, and may provide flashchips.c: the flash chip
# drivers IDF probes (CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST).

set(BMDS_BOARD_SRCS)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${CONFIG_BMDS_BOARD_DIR}/flashchips.c")
        list(APPEND BMDS_BOARD_SRCS "boards/${CONFIG_BMDS_BOARD_DIR}/flashchips.c")
endif()

idf_component_register(SRCS ${BMDS_BOARD_SRCS}
                       INCLUDE_DIRS include "boards/${CONFIG_BMDS_BOARD_DIR}"
                       REQUIRES bmds_core heap
                       PRIV_REQUIRES spi_flash)
if(BMDS_BOARD_SRCS)
        # Referenced only by IDF's spi_flash, which links after this component.
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-u default_registered_chips")
endif()
