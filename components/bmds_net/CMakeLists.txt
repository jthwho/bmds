# bmds_net — the network stack (devplan/network.md): the Ethernet driver, and the `net` command.
#
# Ethernet framing and ARP (ethernet.cpp) are portable: also built as a plain CMake library for
# the host tests (tests/host).
#
# WHOLE_ARCHIVE: nothing references netcommand.cpp by symbol -- its command is found through the
# linker section.

set(BMDS_NET_PORTABLE_SRCS
        src/ethernet.cpp)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_NET_PORTABLE_SRCS}
                                    src/ethernetdevice.cpp
                                    src/netcommand.cpp
                               INCLUDE_DIRS include
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board bmds_shell esp_driver_gpio esp_hw_support esp_mm hal soc
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_net STATIC ${BMDS_NET_PORTABLE_SRCS})
        target_include_directories(bmds_net PUBLIC include)
        target_link_libraries(bmds_net PUBLIC bmds_core)
        bmds_target_options(bmds_net)
endif()
