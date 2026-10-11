# bmds_net — the network stack (devplan/network.md): the Ethernet driver, lwIP (vendored in
# thirdparty/lwip) with its port and interfaces, and the `net` and `ping` commands.
#
# Ethernet framing, ARP, IPv4 text, the multicast filter and the sockets are portable: also built
# as a plain CMake library for the host tests (tests/host), with lwIP itself as bmds_lwip (the
# same lwipopts.h; the host tests define the port's functions and NetStack's).
#
# WHOLE_ARCHIVE: nothing references the commands by symbol -- they are found through the linker
# section.

set(BMDS_LWIP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/lwip/src)

set(BMDS_NET_PORTABLE_SRCS
        src/ethernet.cpp
        src/multicastfilter.cpp)

# lwIP: the core, IPv4 and DNS (IPv6 and mDNS are vendored for later steps, not built yet).
set(BMDS_LWIP_SRCS
        ${BMDS_LWIP_DIR}/core/def.c
        ${BMDS_LWIP_DIR}/core/dns.c
        ${BMDS_LWIP_DIR}/core/inet_chksum.c
        ${BMDS_LWIP_DIR}/core/init.c
        ${BMDS_LWIP_DIR}/core/ip.c
        ${BMDS_LWIP_DIR}/core/mem.c
        ${BMDS_LWIP_DIR}/core/memp.c
        ${BMDS_LWIP_DIR}/core/netif.c
        ${BMDS_LWIP_DIR}/core/pbuf.c
        ${BMDS_LWIP_DIR}/core/raw.c
        ${BMDS_LWIP_DIR}/core/stats.c
        ${BMDS_LWIP_DIR}/core/tcp.c
        ${BMDS_LWIP_DIR}/core/tcp_in.c
        ${BMDS_LWIP_DIR}/core/tcp_out.c
        ${BMDS_LWIP_DIR}/core/timeouts.c
        ${BMDS_LWIP_DIR}/core/udp.c
        ${BMDS_LWIP_DIR}/core/ipv4/dhcp.c
        ${BMDS_LWIP_DIR}/core/ipv4/etharp.c
        ${BMDS_LWIP_DIR}/core/ipv4/icmp.c
        ${BMDS_LWIP_DIR}/core/ipv4/igmp.c
        ${BMDS_LWIP_DIR}/core/ipv4/ip4.c
        ${BMDS_LWIP_DIR}/core/ipv4/ip4_addr.c
        ${BMDS_LWIP_DIR}/core/ipv4/ip4_frag.c
        ${BMDS_LWIP_DIR}/netif/ethernet.c)

set(BMDS_LWIP_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/lwip ${BMDS_LWIP_DIR}/include)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_NET_PORTABLE_SRCS}
                                    ${BMDS_LWIP_SRCS}
                                    src/ethernetdevice.cpp
                                    src/netcommand.cpp
                                    src/netinterface.cpp
                                    src/netnames.cpp
                                    src/netstack.cpp
                                    src/netstatus.cpp
                                    src/pingcommand.cpp
                                    src/socket.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_LWIP_INCLUDES}
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board bmds_fs bmds_shell bmds_sysconf esp_driver_gpio esp_hw_support esp_mm
                                             hal soc
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_lwip STATIC ${BMDS_LWIP_SRCS})
        target_include_directories(bmds_lwip PUBLIC ${BMDS_LWIP_INCLUDES})
        bmds_target_options(bmds_lwip)

        add_library(bmds_net STATIC ${BMDS_NET_PORTABLE_SRCS} src/socket.cpp)
        target_include_directories(bmds_net PUBLIC include PRIVATE ${BMDS_LWIP_INCLUDES})
        target_link_libraries(bmds_net PUBLIC bmds_core bmds_kernel bmds_lwip)
        bmds_target_options(bmds_net)
endif()
