# bmds_drivers — hardware drivers (devplan/display.md): the I2C buses, the
# LT8912B HDMI bridge, EDID, and the `display` command.
#
# EDID parsing is portable: it is also built as a plain CMake library for
# the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references displaycommand.cpp by symbol -- its
# command is found through the linker section.

set(BMDS_DRIVERS_PORTABLE_SRCS
        src/edid.cpp
        src/memtest.cpp
        src/videomodes.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_DRIVERS_PORTABLE_SRCS}
                                    src/benchdisplay.cpp
                                    src/benchmemory.cpp
                                    src/display.cpp
                                    src/displaycommand.cpp
                                    src/displayppa.cpp
                                    src/displayregs.cpp
                                    src/i2cbus.cpp
                                    src/jpegcodec.cpp
                                    src/lt8912b.cpp
                                    src/memtestcommand.cpp
                                    src/blitchain.cpp
                                    src/ppa.cpp
                                    src/scanout.cpp
                                    src/surface.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_bench bmds_board bmds_shell bmds_sysconf esp_driver_i2c esp_driver_jpeg esp_hw_support esp_hal_lcd esp_mm esp_timer
                                             spi_flash
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
        # The JPEG codec loads full-range YUV-to-RGB coefficients into the 2D-DMA (jpegcodec.cpp).
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dma2d_configure_color_space_conversion")
        # `display dsi`'s register tables, from the register headers IDF builds with (the
        # chip revision picks them): tools/gen-regtable.py.
        if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
                set(BMDS_DRIVERS_REGS ${IDF_PATH}/components/soc/esp32p4/register/hw_ver1/soc)
        else()
                set(BMDS_DRIVERS_REGS ${IDF_PATH}/components/soc/esp32p4/register/hw_ver3/soc)
        endif()
        set(BMDS_DRIVERS_REGTABLE ${CMAKE_CURRENT_LIST_DIR}/../../tools/gen-regtable.py)
        idf_build_get_property(BMDS_DRIVERS_PYTHON PYTHON)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dsihostregs.cpp
                           COMMAND ${BMDS_DRIVERS_PYTHON} ${BMDS_DRIVERS_REGTABLE} ${CMAKE_CURRENT_BINARY_DIR}/dsihostregs.cpp
                                   DsiHostRegs DSI_HOST_ ${BMDS_DRIVERS_REGS}/mipi_dsi_host_reg.h int_st0 int_st1 gen_pld_data
                           DEPENDS ${BMDS_DRIVERS_REGTABLE} ${BMDS_DRIVERS_REGS}/mipi_dsi_host_reg.h
                           VERBATIM)
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/dsibrgregs.cpp
                           COMMAND ${BMDS_DRIVERS_PYTHON} ${BMDS_DRIVERS_REGTABLE} ${CMAKE_CURRENT_BINARY_DIR}/dsibrgregs.cpp
                                   DsiBrgRegs DSI_BRG_ ${BMDS_DRIVERS_REGS}/mipi_dsi_bridge_reg.h
                           DEPENDS ${BMDS_DRIVERS_REGTABLE} ${BMDS_DRIVERS_REGS}/mipi_dsi_bridge_reg.h
                           VERBATIM)
        target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/dsihostregs.cpp
                                                ${CMAKE_CURRENT_BINARY_DIR}/dsibrgregs.cpp)
        target_include_directories(${COMPONENT_LIB} PRIVATE src)
else()
        add_library(bmds_drivers STATIC ${BMDS_DRIVERS_PORTABLE_SRCS})
        target_include_directories(bmds_drivers PUBLIC include)
        target_link_libraries(bmds_drivers PUBLIC bmds_core)
        bmds_target_options(bmds_drivers)
endif()
