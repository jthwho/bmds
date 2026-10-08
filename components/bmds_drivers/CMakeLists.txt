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
                                    src/display.cpp
                                    src/displaycommand.cpp
                                    src/i2cbus.cpp
                                    src/lt8912b.cpp
                                    src/memtestcommand.cpp
                                    src/blitchain.cpp
                                    src/scanout.cpp
                                    src/surface.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board bmds_shell bmds_sysconf esp_driver_i2c esp_hw_support esp_hal_lcd esp_mm esp_timer
                                             spi_flash
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_drivers STATIC ${BMDS_DRIVERS_PORTABLE_SRCS})
        target_include_directories(bmds_drivers PUBLIC include)
        target_link_libraries(bmds_drivers PUBLIC bmds_core)
        bmds_target_options(bmds_drivers)
endif()
