# lwIP (vendored)

The lwIP TCP/IP stack, <https://savannah.nongnu.org/projects/lwip/> (mirror:
<https://github.com/lwip-tcpip/lwip>), used by `components/bmds_net` (`devplan/network.md` §3,
§15). License: `COPYING` (BSD-3-Clause), and each file's header.

- **Version:** 2.2.1, tag `STABLE-2_2_1_RELEASE`, commit
  `77dcd25a72509eb83f72b033d219b1d40cd8eb95` (2025-02-05). The 133 files under `src` are as
  released: sha256 of them all, concatenated in sorted path order
  (`cd src && find . -type f -print0 | sort -z | xargs -0 cat | sha256sum`):
  `efd5ae212270dfbd481c902f13fe55b44109ad2fdf6eb4e4047c1e9afe80c953`.
- **Changes:** none.
- **Vendored:** `src/core` (with `ipv4/` and `ipv6/`), `src/include/lwip` (of the apps' headers,
  mDNS's only), `src/include/netif/{ethernet,etharp}.h`, `src/include/netif/ppp/{ppp_opts,ppp_impl}.h`
  (`init.c` includes them whatever the options; PPP is off), `src/netif/ethernet.c`,
  `src/apps/mdns` (`network.md` step 4).
- **Built:** the core, IPv4, DHCP, IGMP and Ethernet (`components/bmds_net/CMakeLists.txt`);
  IPv6, DNS and mDNS are vendored for later steps. The configuration is
  `components/bmds_net/lwip/lwipopts.h` and the port `components/bmds_net/lwip/` (`arch/cc.h`,
  `lwipport.h`); the firmware's port functions are in `components/bmds_net/src/netstack.cpp`, the
  host tests' in `tests/host/lwip.cpp`.
- **Not vendored:** `contrib`, `test`, `doc`, PPP, 6LoWPAN, SLIP, the sequential and socket APIs
  (`src/api`: BMDS's sockets are its own, on the raw API) and the other apps.

To update: copy the same paths from the new release's tag, check its `UPGRADING` against
`lwipopts.h`, update the checksum above, and run the host tests (`tests/host/lwip.cpp`).
