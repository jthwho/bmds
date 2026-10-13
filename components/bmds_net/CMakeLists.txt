# bmds_net — the network stack (devplan/network.md): the Ethernet driver, lwIP (vendored in
# thirdparty/lwip) with its port and interfaces, NTP and mDNS, and the `net`, `ping` and `ntp`
# commands.
#
# Ethernet framing, ARP, IPv4 text, the multicast filter, the real-time classifier, ICMP's rate
# limits, NTP's arithmetic, PTP and the sockets are portable: also built as a plain CMake library for
# the host tests (tests/host), with lwIP itself as bmds_lwip (the same lwipopts.h; the host tests
# define the port's functions and NetStack's).
#
# WHOLE_ARCHIVE: nothing references the commands by symbol -- they are found through the linker
# section.

set(BMDS_LWIP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/lwip/src)

set(BMDS_NET_PORTABLE_SRCS
        src/ethernet.cpp
        src/icmpratelimit.cpp
        src/multicastfilter.cpp
        src/netclassifier.cpp
        src/ntp.cpp
        src/ptp.cpp
        src/ptpport.cpp)

# lwIP: the core, IPv4, DNS and mDNS's responder (IPv6 is vendored for later, not built yet).
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
        ${BMDS_LWIP_DIR}/netif/ethernet.c
        ${BMDS_LWIP_DIR}/apps/mdns/mdns.c
        ${BMDS_LWIP_DIR}/apps/mdns/mdns_domain.c
        ${BMDS_LWIP_DIR}/apps/mdns/mdns_out.c)

set(BMDS_LWIP_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/lwip ${BMDS_LWIP_DIR}/include)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/bmds.cmake)

if(COMMAND idf_component_register)
        idf_component_register(SRCS ${BMDS_NET_PORTABLE_SRCS}
                                    ${BMDS_LWIP_SRCS}
                                    src/ethernetdevice.cpp
                                    src/netcommand.cpp
                                    src/netflow.cpp
                                    src/netinterface.cpp
                                    src/netmdns.cpp
                                    src/netnames.cpp
                                    src/netstack.cpp
                                    src/netstats.cpp
                                    src/netstatus.cpp
                                    src/ntpclient.cpp
                                    src/ntpcommand.cpp
                                    src/pingcommand.cpp
                                    src/ppsout.cpp
                                    src/ptpclock.cpp
                                    src/ptpcommand.cpp
                                    src/ptpservice.cpp
                                    src/socket.cpp
                                    src/socketfastpath.cpp
                                    src/sshservice.cpp
                               INCLUDE_DIRS include
                               PRIV_INCLUDE_DIRS ${BMDS_LWIP_INCLUDES}
                               REQUIRES bmds_core bmds_kernel
                               PRIV_REQUIRES bmds_board bmds_fs bmds_shell bmds_sysconf esp_driver_gpio esp_driver_gptimer esp_hw_support esp_mm
                                             hal soc
                               WHOLE_ARCHIVE)
        bmds_target_options(${COMPONENT_LIB})
else()
        add_library(bmds_lwip STATIC ${BMDS_LWIP_SRCS})
        target_include_directories(bmds_lwip PUBLIC ${BMDS_LWIP_INCLUDES})
        bmds_target_options(bmds_lwip)

        add_library(bmds_net STATIC ${BMDS_NET_PORTABLE_SRCS} src/socket.cpp src/socketfastpathnone.cpp)
        target_include_directories(bmds_net PUBLIC include PRIVATE ${BMDS_LWIP_INCLUDES})
        target_link_libraries(bmds_net PUBLIC bmds_core bmds_kernel bmds_lwip)
        bmds_target_options(bmds_net)
endif()
