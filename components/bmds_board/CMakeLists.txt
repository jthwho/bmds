# bmds_board — board definitions (all hardware truth) and MemSpace mapping.
#
# The board is selected by Kconfig (BMDS_BOARD); its directory under boards/
# provides <bmds/boarddef.h>, and may provide flashchips.c: the flash chip
# drivers IDF probes (CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST). Its definition
# names how its serial number is read; src/ has the readers boards share
# (chipserial.cpp: the chip's factory MAC). regionprotect.cpp sets the CPU's PMA and PMP for
# every app that links this component (devplan/protection.md §4.1).

set(BMDS_BOARD_SRCS src/chipserial.cpp src/regionprotect.cpp)
set(BMDS_BOARD_FLASHCHIPS OFF)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${CONFIG_BMDS_BOARD_DIR}/flashchips.c")
        list(APPEND BMDS_BOARD_SRCS "boards/${CONFIG_BMDS_BOARD_DIR}/flashchips.c")
        set(BMDS_BOARD_FLASHCHIPS ON)
endif()

idf_component_register(SRCS ${BMDS_BOARD_SRCS}
                       INCLUDE_DIRS include "boards/${CONFIG_BMDS_BOARD_DIR}"
                       REQUIRES bmds_core heap
                       PRIV_REQUIRES esp_hw_support esp_system riscv soc spi_flash)
if(BMDS_BOARD_FLASHCHIPS)
        # Referenced only by IDF's spi_flash, which links after this component.
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-u default_registered_chips")
endif()
# The region setup is a start-up hook nothing calls: kept by its anchor in every app.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bmds_region_protect_anchor")
