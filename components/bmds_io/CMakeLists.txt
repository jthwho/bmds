# bmds_io — the system's I/O (devplan/io.md): every pad and peripheral instance of the chip, who
# holds it (drivers claim what they use through Io::claim()), the board's wiring, the pads' state
# read from the hardware, the `io` command, and `/dev/gpiochip0` (gpio_ctl's, in bmds_loader).
#
# The chip's table is generated from ESP-IDF's soc headers by tools/gen-iochip.py into
# chips/<target>/iochipdef.cpp (checked in); chips/<target>/iochipops.cpp reads the chip's pad
# registers. Porting to another ESP32 is those two files.
#
# Its pure part (the chip table's lookups, the registry, the pad state's text) is also built as a
# plain CMake library for the host tests (tests/host), with the ESP32-P4's table.
#
# WHOLE_ARCHIVE: nothing references the `io` command by symbol -- it is found through the linker
# section (BMDS_SHELL_COMMAND).

set(BMDS_IO_PURE_SRCS
        src/iochip.cpp
        src/iopad.cpp
        src/ioline.cpp
        src/iopin.cpp
        src/ioport.cpp
        src/pwmtiming.cpp
        src/iosetup.cpp
        src/ioregistry.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        set(BMDS_IO_CHIP chips/${IDF_TARGET})
        if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/${BMDS_IO_CHIP}/iochipdef.cpp)
                message(FATAL_ERROR "bmds_io: no I/O table for ${IDF_TARGET}: run tools/gen-iochip.py ${IDF_TARGET} "
                                    "and write ${BMDS_IO_CHIP}/iochipops.cpp (devplan/io.md §3.1)")
        endif()
        idf_component_register(SRCS ${BMDS_IO_PURE_SRCS}
                                    ${BMDS_IO_CHIP}/iochipdef.cpp
                                    ${BMDS_IO_CHIP}/iochipops.cpp
                                    src/gpiochip.cpp
                                    src/gpiolines.cpp
                                    src/io.cpp
                                    src/iogpio.cpp
                                    src/ioports.cpp
                                    src/iosetups.cpp
                                    src/spiports.cpp
                                    src/rmtports.cpp
                                    src/pwmports.cpp
                                    src/uartconsole.cpp
                                    src/iocommand.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS src
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board bmds_console bmds_fs bmds_shell bmds_sysconf esp_driver_gpio esp_driver_gptimer esp_driver_rmt esp_driver_spi esp_hw_support esp_mm hal soc
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_io STATIC ${BMDS_IO_PURE_SRCS} chips/esp32p4/iochipdef.cpp)
        target_include_directories(bmds_io PUBLIC include)
        target_link_libraries(bmds_io PUBLIC bmds_core bmds_kernel)
        bmds_target_options(bmds_io)
endif()
