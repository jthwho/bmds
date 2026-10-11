# The network stack

Designed 2026-10-10 (the user's requirements and answers, same day; decisions in §13). Step 1 (the
driver) built and tried on the board (§14).

## 1. Goal

Wired Ethernet on `eth0` with an IPv4 stack that serves two kinds of traffic. **Ordinary traffic**
(TCP, DNS, SSH, NTP, file transfer) goes through a complete, conventional stack with BSD sockets.
**Real-time traffic** (PTP, AES67's RTP) doesn't wait behind it in either direction:
- a real-time frame goes on the wire as soon as possible;
- a received real-time frame reaches its consumer without going through the IP stack.

In the user's order:
1. **SSH and SCP**, client and server, as soon as possible. They are the development tools: a
   shell, and pushing files and programs over the network instead of the serial cable.
2. **Every parameter a sysconf setting**: addresses, DHCP, DNS, NTP servers, SSH keys' paths.
3. **Numbered interfaces** (`eth0`, later `eth1`, `wlan0`, `lo`). Nothing assumes there is one.
4. **Multicast first class:** joining, leaving, filters in the MAC, IGMP, sockets' options.
5. **NTP soon, PTP with AES67.** Both are kernel services that discipline the clocks
   (`devplan/time.md`), not programs.
6. **BSD sockets, as compatible as possible.** Programs are musl programs (`devplan/programs.md`
   §12), so this means Linux's socket system calls, as `sdk/crt/linux.c` already does for files.
7. **IPv4 only**, with IPv6 kept possible: nothing in the APIs, keys or structures that is
   v4-only where it needn't be.
8. **Measure it:** a tool for real-time latency and jitter, between two BMDS boards or against a
   Linux host.

AES67 itself is a later phase (`audio.md` step 10). This plan builds what it will need.

## 2. Facts that shape the design

- **The hardware:**
  - An IP101GRR PHY on RMII at PHY address 1. It is 10/100 only, so the wire runs at 100 Mbit/s.
  - The PHY supplies the 50 MHz reference clock into GPIO50 and is reset through GPIO51.
  - **No interrupt line from the PHY** (`docs/HARDWARE.md`). Link changes can only be found by
    reading the PHY over MDIO. That forces a slow timer (§4).
  - GPIO35 is both RMII TXD1 and the BOOT button, so the button is lost while Ethernet runs.
  - The MAC address comes from the eFuses, as the board's serial number already does.
- **The MAC is a Synopsys DesignWare GMAC with the IEEE 1588 timestamp unit.** ESP-IDF uses only
  part of it. From the register files (`soc/esp32p4/register/hw_ver1/soc/emac_reg.h`):
  - **1588 timestamps:**
    - Hardware timestamps for frames received and sent, recorded in their descriptors.
    - A system time that can be adjusted finely: an addend register, so frequency correction
      needs no software interpolation. Its clock is the crystal (25 ns ticks) or PLL_F80M
      (12.5 ns).
    - Target-time interrupts.
    - PPS 0, which can be routed to any GPIO (`EMAC_PTP_PPS_PAD_OUT_IDX`). The register file has
      four; the feature register says no flexible PPS (below).
  - **Filters:** perfect address filters (eight addresses in the register file) and VLAN tags.
    The register file also has a 64-bin multicast hash and a layer 3/4 filter; **this chip has
    neither** (below).
  - **Registers for three DMA channels:**
    - `CHANNEL1*`/`CHANNEL2*`, each with a credit-based shaper and "slot" control.
    - An "AV MAC control" register that sends PTP frames and VLAN-priority frames to a chosen
      receive channel.

    That is the 802.1 AVB configuration of the GMAC: **separate hardware queues for real-time
    traffic**. ESP-IDF's driver uses channel 0 only, and its structure headers leave the others
    out.

    **The silicon doesn't have them** (step 1, 2026-10-10): the DMA feature register
    (`EMAC_HWFEATURE_REG`) reads `4b056725`: one channel each way, no AV feature, no hash
    filter, no layer 3/4 filter, no flexible PPS (`docs/platforms/esp32p4.md` §4a). The real-time
    design is §5's single-ring one.
- **The wire's times at 100 Mbit/s:**

  | Frame | Size on the wire | Time |
  |---|---|---|
  | Full-size frame (with preamble and gap) | 1538 bytes | 123 µs |
  | AES67, 1 ms, 2 channels, L24 | 366 bytes | 29 µs |
  | AES67, 1 ms, 8 channels | ~1230 bytes | 98 µs |

  A real-time frame behind *n* full-size frames waits *n* × 123 µs. The queue's depth ahead of it
  is the design's main number.
- **ESP-IDF's lwIP, `esp_netif` and `esp_eth` aren't in the build** (the minimal build has 75
  components and none of them). Vendoring our own stack causes no clashes. ESP-IDF's HAL
  (`hal/emac_ll.h`) is available for register access.
- **The kernel already has the pieces sockets need:**
  - `Stream`, `FdTable` and readiness with `WaitList` for `poll`/`select` (`programs.md` §16).
  - Pseudo-terminals (`pty.md`).
  - Processes with `vfork`, `posix_spawn` and pipes.
  - The clock model's slew and frequency calls, there for an NTP or PTP loop (`time.md` §2).
  - Sysconf's run-time declarations and watches (`sysconf.md` §2, §5).
  - `system/hostname` and `${HOSTNAME}`.
- **musl hardcodes `/etc/resolv.conf`, `/etc/hosts` and `/etc/services`.** Settings live in
  sysconf, so `/etc` becomes a pseudo filesystem built from them (§8).
- **Modern `scp` speaks SFTP.** Since OpenSSH 9.0, `scp` uses the SFTP protocol unless given `-O`.
  Dropbear has no SFTP server, so "SCP" from a current Linux host needs one (§9). With it come
  `sftp` and `sshfs`.
- **Budget:**
  - Internal RAM: 455 KB of DIRAM is free of static data today (`idf.py size`, 2026-10-10). The
    heap then takes from it at run time.
  - Flash: the firmware is 1.5 MB of its 4 MB partition.

## 3. The shape

```
 programs (musl):  socket() bind() sendto() recvmsg() poll() …    ssh  scp  sftp-server  netlat
                       │  linux.c → BMDS socket slots
 ─────────────────────┼─────────────────────────────────────────────────────────────────────
 kernel:      Socket (a Stream: fds, poll, readiness)           NTP service    PTP service
                 │                     │                            │              │
           ordinary sockets      fast-path sockets ◄────────────────┴──────────────┘
                 │                     │  (UDP/raw on registered flows; header built here)
           lwIP core (vendored)        │
           TCP UDP IGMP ARP DHCP       │
                 │                     │
           Netif eth0 ─────────────────┤
                 │                     │
           EthernetDevice eth0:  RX classifier ── real-time flows ──► consumer, woken directly
                                 TX queues: real-time (first) │ bulk (bounded depth)
                                 EMAC DMA (one ring each way), 1588 timestamps
```

- **Vendor upstream lwIP (2.2.x, BSD-licensed)** into `thirdparty/lwip`, with BMDS patches kept
  as `.diff` files as for TinySoundFont.
  - **Not ESP-IDF's fork, and not our own TCP.** A correct TCP (congestion control, retransmission,
    window and timer edge cases) is a large and slow job. SSH, the first goal, needs it first.
  - Alternatives considered:

    | Stack | Why not |
    |---|---|
    | FreeRTOS-Plus-TCP | Weaker multicast support |
    | picoTCP | GPL |
    | uIP | Too small |
    | smoltcp | Rust |
- **Push back on "we'll need to change the stack":** most of what the real-time goals need lives
  below lwIP, in code that is ours anyway: the driver, the classifier and the queues. lwIP's core
  should need few patches, which keeps updates easy. Expected patches:
  - zero-copy receive buffers (custom pbufs, already supported);
  - hooks for the classifier and the multicast filter;
  - later, IGMPv3. lwIP has v2 only, and source-specific multicast needs v3.
- **lwIP's raw API under the kernel's own sockets.** lwIP's `sockets.c` and `netconn` layer are
  not used.
  - They add a message hop to the `tcpip` thread for every call.
  - They don't fit BMDS's `Stream`/`poll` model.

  Our `Socket` calls the raw API under the core lock (`LWIP_TCPIP_CORE_LOCKING`), in the caller's
  task.
- **One stack, many interfaces.** lwIP's state is one static instance, like any OS's IP stack.
  Interfaces are numbered devices; nothing is a singleton of the interface kind.
- **No polling:**
  - lwIP's timers run from the network task's wait. It sleeps until the next timeout
    (`sys_timeouts_sleeptime`) or a message, never on a fixed tick.
  - The exception is the PHY: a 1 s link check, because there is no interrupt line (§4). It costs
    one MDIO read, about 30 µs, per second.

## 4. The driver: `EthernetDevice` (`bmds_net`, `eth0`)

Our own driver on `emac_ll.h`, not `esp_eth`. `esp_eth` gives one receive task, copies every frame
into a heap buffer, offers no queue choice, and hides the descriptors that carry the timestamps.

- **A numbered device:** `EthernetDevice::open(n)` gives `ethN`. Pins, PHY address and reset come
  from the board definition (`.ethernet`, a list).
- **The PHY:** a small `Phy` interface with an IP101 implementation (reset, autonegotiation,
  link, speed, duplex).
  - **Link changes are found by a 1 s timer** that reads the status register. There is no
    interrupt pin: the polling exception named above.
  - The timer is a FreeRTOS software timer, not a task.
- **Descriptor rings and buffers in internal RAM** (MemSpace `Dma`):
  - 1536-byte buffers in 128-byte-aligned slots, kept in sync with the caches per
    `docs/platforms/esp32p4.md` §3.
  - Proposed: 16 receive and 16 transmit buffers, about 50 KB. TCP's windows and the socket
    buffers live in PSRAM (MemSpace `Psram`).
- **Zero-copy receive:**
  - A received buffer goes up as a custom pbuf, or to a fast-path consumer as is.
  - Its free gives it back to the ring. The ring is refilled from a free list, so a consumer
    holding buffers costs ring slots, not copies.
  - A per-flow limit stops one consumer from emptying the ring.
- **Interrupts:**
  - The ISR acknowledges and wakes the receive task, which is high priority (above audio's
    engine) and short.
  - The receive watchdog (interrupt coalescing) is off for real-time traffic. It may be on for
    bulk traffic if that's measured to help.
- **Timestamps:** the 1588 unit runs from boot. Every received frame carries its arrival time. A
  sent frame can ask for its departure time, which is returned on completion.
- **The 1588 clock is a device clock:** `ptpclock0`, with time, adjustment, frequency (the
  addend), target-time events and PPS. PTP disciplines it (§10), and the system and audio clocks
  follow it.
- **Counters:** frames, bytes, errors, drops by cause, the deepest queue seen, and receive latency
  (hardware arrival to consumer). `net` shows them (§11).

## 5. Real time: the fast path

### 5.1 Receive

- **The classifier** runs in the receive task on each frame, before lwIP. It costs tens of cycles:
  EtherType, then IP protocol and destination, then UDP port.
  - Frames matching a **registered flow** go straight to that flow's ring and wake its consumer:
    - PTP over Ethernet (0x88F7);
    - UDP 319/320;
    - a multicast group and port (an RTP stream).
  - Everything else is queued for the network task and lwIP.
- **What a consumer gets:** the buffer, the hardware arrival timestamp and the interface. There
  are no copies and no IP stack. A flow is a fast-path socket (§7) or, in the kernel, a `NetFlow`
  that PTP and AES67 use directly.
- **Classifying in the ISR** would save the task switch, a few µs. Measure it first (§11); the
  task is the default because it keeps the ISR tiny.

### 5.2 Send

- **A real-time send runs in the sender's task:**
  1. Build the headers. A fast-path socket keeps its precomputed Ethernet, IP and UDP headers,
     and its destination MAC (from ARP, or from the group for multicast).
  2. Write the descriptor, write the cache back and kick the DMA.
  3. Don't fragment.

  It never waits for lwIP or the network task.
- **Head-of-line blocking is bounded by queue depth.** A frame already on the wire can't be
  preempted, so the floor is one full frame (123 µs).
  - **One ring** (the chip has no AV channels): the driver keeps at most *k* bulk frames in the
    ring, and the rest wait in a software queue. Real-time frames are written ahead of that
    queue.
    - With *k* = 2, the worst wait is about 250 µs. A 1 ms AES67 packet time tolerates that.
    - Bulk throughput should barely suffer: refilling takes tens of µs against 123 µs per frame.
      *k* is a setting, measured in step 6.
- **Departure timestamps** on request: needed by PTP's Sync and Delay_Req, and by the
  measurement tool.
- **Pacing** (sending at a given time) comes from the sender's own wake-up: the audio engine's
  period for AES67, the 1588 target-time interrupt for the measurement tool. It never comes from
  the 1 kHz tick.

## 6. Interfaces, addresses and sysconf

- **Interfaces:** `Netif` objects named from their device (`eth0`) plus `lo`.
  - Each interface's parameters are **declared at run time when the device appears**
    (`sysconf.md` §2), under its name.
  - **Keys** (proposed; frozen once shipped):

    | Key | Type | Default | Meaning |
    |---|---|---|---|
    | `net/eth0/enabled` | bool | `true` | |
    | `net/eth0/mac` | string | empty | override; empty means the eFuse address |
    | `net/eth0/ipv4/mode` | enum `dhcp static off` | `dhcp` | |
    | `net/eth0/ipv4/address` | string | | `192.168.1.50/24` (CIDR) |
    | `net/eth0/ipv4/gateway` | string | | |
    | `net/eth0/mtu` | int | 1500 | |
    | `net/eth0/rt-depth` | int | 2 | *k*, §5.2 |
    | `net/dns/servers` | string | empty | `;`-separated; DHCP's are used when empty |
    | `net/dns/search` | string | empty | |
    | `net/hosts` | string | empty | `name=address` entries, `;`-separated (§8) |
    | `net/ntp/enabled` | bool | `true` | |
    | `net/ntp/servers` | string | `pool.ntp.org` | DHCP option 42 added |
    | `net/mdns/enabled` | bool | `true` | `${HOSTNAME}.local` |
    | `net/ssh/server` | bool | `false` | off until the user turns it on (§9) |
    | `net/ssh/port` | int | 22 | |
    | `net/ssh/password-login` | bool | `true` | only with a password set |
    | `net/ssh/key-login` | bool | `true` | |
    | `net/ssh/host-keys` | path | `/boot/ssh` | generated on first start |
    | `net/ssh/authorized-keys` | path | `${HOME}/.ssh/authorized_keys` | the user's file, OpenSSH format |
    | `system/password` | secret | empty | salted hash (§9); empty means no password |
    | `net/ptp/…` | | | domain, profile, priorities, slave-only (§10) |
  - The `ipv4/` level leaves room for `ipv6/` beside it.
- **Live changes:** each interface watches its keys. A change of mode or address takes effect
  without a reboot, as sysconf's watches already do for the panel.
- **The hostname** is `system/hostname`. DHCP sends it (option 12) and mDNS answers for it.
- **Multicast:**
  - Joining a group (a socket's `IP_ADD_MEMBERSHIP`, a `NetFlow`, mDNS) programs the MAC
    filters: the perfect address filters first; with more groups than filters, the MAC passes
    all multicast and the receive path drops the groups nobody joined (the chip has no hash
    filter). lwIP's IGMP (v2) reports and leaves.
  - `IP_MULTICAST_IF`, `IP_MULTICAST_TTL` and `IP_MULTICAST_LOOP` are supported. Loopback
    delivery is in software.
  - The groups each interface has joined, with their users, are shown by `net`.

## 7. Sockets: the kernel and the ABI

- **`Socket` is a `Stream`.** It gets fds, `read`/`write`, `close`, `dup`, inheritance across
  `spawn`, and `poll` readiness through `WaitList`, all as files and pipes already do. Blocking
  calls honour Ctrl-C and signals (`EINTR`, `SA_RESTART`) like `read` on a pipe.
- **Families and types:**
  - `AF_INET`: `SOCK_STREAM`, `SOCK_DGRAM`, and `SOCK_RAW` for ICMP (`ping`).
  - `AF_PACKET`: raw Ethernet, for PTP over layer 2 and for tools.
  - `AF_UNIX` later. Programs such as Dropbear's agent forwarding use it; it comes after the
    first SSH.
  - `AF_INET6` later. The calls carry `sockaddr_storage`-sized addresses, so nothing changes in
    the ABI when it arrives.
- **New system call slots (appended):** `socket`, `socketpair`, `bind`, `listen`, `accept4`,
  `connect`, `getsockname`, `getpeername`, `sendmsg`, `recvmsg`, `setsockopt`, `getsockopt`,
  `shutdown`.
  - `send`, `recv`, `sendto` and `recvfrom` are `sendmsg` and `recvmsg` in `linux.c`, which keeps
    the slot count down.
  - Boundary structs start with `uint32_t size` (`docs/ABI.md`).
- **Linux's numbers:**
  - The option, flag and family constants are Linux's values, so musl's headers are right as they
    stand: `SO_*`, `IPPROTO_*`, `MSG_*`, `IP_*`, `TCP_NODELAY`, `TCP_KEEP*`.
  - The interface `ioctl`s are supported (`SIOCGIFCONF`, `SIOCGIFADDR`, `SIOCGIFFLAGS`,
    `SIOCGIFHWADDR`, `SIOCGIFINDEX`, …), as are `if_nametoindex` and `getifaddrs`.
  - Not netlink. BusyBox's `ip` needs it; `ifconfig` and BMDS's own `net` don't.
- **Real-time options:**
  - `SO_TIMESTAMPING` with hardware stamps, delivered in `recvmsg`'s control messages, the way
    Linux PTP software (linuxptp) expects.
  - `SO_PRIORITY` ≥ 6, or DSCP EF/CS6 in `IP_TOS`, on a UDP socket picks the real-time transmit
    queue.
  - **`SO_BMDS_FASTPATH`** (a BMDS option number, one bit) makes a bound UDP socket a classifier
    flow: its datagrams bypass lwIP both ways.

  A program written for Linux with timestamps and priorities works as it stands. One line more
  makes it use the fast path.
- **Yardsticks,** as BusyBox was for processes:
  - BusyBox's network applets: `nc`, `wget`, `telnet`, `ping`, `ifconfig`, `ntpd -q`, `httpd`;
  - Dropbear (§9);
  - a timestamping test from linuxptp's test programs.

## 8. Names: DNS, hosts, services

- **DNS:** musl's resolver does the work in the program. It reads `/etc/resolv.conf`.
- **`/etc` is a pseudo filesystem** (decided: the user, 2026-10-10): a read-only volume mounted
  at `/etc`, like `/rom`, whose files the kernel builds from sysconf when they are opened. **Every
  system setting stays in sysconf**, so a control panel later edits one place; the files are a
  view for programs that expect them, and the shell can `cat` them.
  - `resolv.conf`: `net/dns/*` and DHCP's answer;
  - `hosts`: `localhost`, `${HOSTNAME}` and `net/hosts`;
  - `services`, `protocols`: rodata tables;
  - `hostname`, `localtime` (the zone's file from `/rom`), and `passwd`/`group` with the one user
    (Dropbear and BusyBox look the user up);
  - more as ported programs ask, each built from settings, never stored.

  musl is unpatched, and nothing on a card is needed.
- **Kernel services resolve names themselves:**
  - lwIP's `dns.c` serves NTP's servers and the shell's `ping`.
  - **mDNS** (lwIP's responder) answers for `${HOSTNAME}.local` and advertises `_ssh._tcp`, so
    `ssh bmds-e2e222.local` works with no DNS server. RAVENNA-style AES67 discovery uses it later
    too.

## 9. SSH, file transfer and crypto

### 9.1 SSH

- **Dropbear (MIT-style licence), built with the SDK as programs on the card** (decided): the
  server `dropbear`, the client `dbclient` as `ssh`, and its `scp`.
  - Dropbear's no-MMU mode uses `vfork`, which BMDS has.
  - Its crypto is libtomcrypt/libtommath (public domain), with hardware behind it where it pays
    (§9.3).
  - It is the sockets ABI's yardstick, and it keeps "built-in apps use only public APIs" true.
- **Off until the user turns it on:** `net/ssh/server` defaults to false. When true, the kernel
  starts `dropbear -F` once the interface is up and stops it when the setting goes false. Each
  session gets a pseudo-terminal and a shell, as the terminal application does.
- **Logins, keys and passwords both** (decided):
  - **Keys:** from `net/ssh/authorized-keys` (OpenSSH's format).
  - **Password:** `system/password`, the machine's one user's password, not SSH's own, so a
    console login can use it later. It holds a salted hash in crypt(3)'s form: `$6$` (SHA-512
    crypt, which musl's `crypt()` also reads), checked in the kernel with the SHA engine, with
    enough rounds for about 0.1 s a check on the board. Empty means no password, and then password login is
    refused whatever `net/ssh/password-login` says.
  - **Set with `passwd`**, which hashes it; the plain password is never stored or logged.
  - **A `secret` setting type** is new in sysconf: stored like a string, but `sysconf get` and
    listings print `(set)` or `(not set)` instead of the value, and only a hash goes in. Programs
    check a password through a call (`bmds_password_check`), not by reading the hash.
  - Host keys are generated on first start into `net/ssh/host-keys` (Ed25519, and ECDSA P-256 if
    §9.3 makes it worth having).
  - Failed logins are logged and slowed down (a delay that grows per source address).

### 9.2 A network file system: SFTP

The user wants a network file protocol that maps to a filesystem, as `sshfs` does; SCP and SFTP
were only the guess at the least effort. Compared:

| Protocol | Mounts on a PC | Security | Server's size | Notes |
|---|---|---|---|---|
| **SFTP** (over SSH) | Linux/macOS `sshfs`; Windows SSHFS-Win; GNOME Files, Dolphin, WinSCP, FileZilla open `sftp://` directly | SSH's: encrypted, keys or password | Small: protocol v3 is about 20 requests | One port, one login for shell and files. `scp` (OpenSSH 9+) and `sftp` come with it. |
| WebDAV | Natively in Windows, macOS, Linux file managers | Needs TLS, else passwords in clear | An HTTP server plus TLS | Slow small-file behaviour in Windows' client |
| NFSv3 | Linux and macOS kernels | None to speak of (addresses) | Medium (RPC, XDR, mountd) | Mounting needs root on the PC |
| 9P | Linux kernel (`v9fs`) | None | Small | Little support outside Linux |
| SMB | Everywhere | Good (SMB3) | Very large | Out of proportion |

- **Decision proposed: SFTP, with our own server**, not OpenSSH's. OpenSSH is out of proportion
  for this machine (the user agrees) and its `sftp-server` drags in its compatibility library.
  - `sftp-server` is an SDK program Dropbear runs as the `sftp` subsystem: protocol version 3
    (what `sshfs` and OpenSSH use), on the public file API: `open`, `read`, `write`, `stat`,
    `opendir`, `rename`, `remove`, `mkdir`, `realpath`, `symlink` (BMDS's emulated links).
  - About 1500 lines; fuzzed, since it parses a peer's input.
- **The board mounting others** (a PC's folder as a path on the board) is the other direction. It
  wants a kernel VFS client; SFTP's would need SSH in the kernel. Left until there is a need;
  9P or NFS then, on a trusted network.

### 9.3 Crypto acceleration

The P4's engines (ESP-IDF's `soc_caps.h`):

| Engine | Does | Use |
|---|---|---|
| AES | 128/256-bit, ECB/CBC/CTR/GCM, with DMA | SSH's `aes128-ctr`, `aes256-gcm`; TLS later |
| SHA | SHA-1, SHA-224/256/384/512, with DMA | HMAC-SHA2, SSH's exchange hash, `$6$` password checks |
| MPI (RSA) | Modular exponentiation and multiplication to 4096 bits | RSA host keys and signatures, classic Diffie-Hellman |
| ECC | P-192 and P-256 point operations, extended modes | `ecdh-sha2-nistp256`, `ecdsa-sha2-nistp256` |
| HMAC, DS, key manager | Keys held in eFuses, never readable | Later: protect the host key so a stolen card isn't a stolen identity |
| RNG | True random numbers | Already used (`random.md`) |

- **Not accelerated: Curve25519, Ed25519 and ChaCha20-Poly1305**, the algorithms OpenSSH clients
  prefer. A client chooses the first of its algorithms the server offers, so using the engines
  means the server offering AES-GCM ahead of (or instead of) ChaCha20, and P-256 beside
  Curve25519. **Measure first:** software against hardware for SSH's packet sizes (a small packet
  can lose to the engine's setup), then choose the server's offer.
- **A kernel crypto service** (`bmds_crypto`): it owns the engines, one user at a time each, and
  offers hash, cipher, AEAD, big-number and P-256 operations. Programs reach it through SDK calls
  (appended slots, operating on handles); a system call is a function call on BMDS, so the cost
  is the engine's, not the crossing.
- **Dropbear's patch is small:** libtomcrypt takes cipher and hash descriptors (`register_cipher`,
  `register_hash`) and libtommath's hooks; the hardware ones call the SDK.
- **The kernel uses it too:** password hashing; later TLS (an HTTPS `wget`) and NTS.
- Placement: a step of its own after SSH works in software (step 5a), so SSH isn't held up and
  the gain is measured, not assumed.

## 10. Time: NTP and PTP in the kernel

- **NTP (step 4)**, a kernel service on a kernel socket:
  - Several servers (`net/ntp/servers`, DHCP's).
  - Per server, the clock filter: the minimum-delay sample of the last 8.
  - A simple selection across servers.
  - The discipline uses the clock model: a step beyond 128 ms, else slew; frequency through
    `setFrequency()`.
  - `clock` and `ntp` show its state: offset, delay, jitter, the server chosen.
  - It is RFC 5905's client side without the full intersection algorithm. A server mode (BMDS
    serving time to the LAN) is a small addition.
- **PTP (step 7)**, IEEE 1588-2008 with AES67's media profile (and the default profile).
  **Client and server both** (the user, 2026-10-10): two BMDS boards alone on a network must do
  AES67, with the best master clock algorithm (BMCA) making one of them the grandmaster.
  - **One ordinary clock, either role:** it listens for Announce messages, runs BMCA on what it
    hears and on its own data set, and becomes master or slave accordingly. `net/ptp/slave-only`
    keeps a board from ever being master.
  - **As master:**
    - It sends Announce, and Sync with Follow_Up (two-step: the Follow_Up carries Sync's hardware
      departure stamp, so the MAC needn't rewrite frames on the fly).
    - It answers Delay_Req with Delay_Resp (end-to-end delay, AES67's choice).
    - Its time is `ptpclock0` running free from the crystal, seeded from the system clock at start
      (from NTP when it has it) so PTP time is near TAI.
  - **What it advertises:**
    - clockClass 248 (default, no external reference), or better when the system clock is
      NTP-locked;
    - priority1 and priority2 from settings;
    - its identity from the MAC address (EUI-64), which settles ties.

    A real grandmaster (the user's GPS one, clockClass 6) always wins over a board, so the same
    firmware does the right thing on both networks.
  - **Failover:** a master that falls silent is noticed by the announce receipt timeout. The
    slaves rerun BMCA and one becomes master. The new master keeps its last frequency correction
    (holdover), so the media clock doesn't jump when the role changes.
  - **The media profile's rates:**
    - Sync 8 a second (2^-3);
    - Announce every 2 s, with a timeout of 3 intervals;
    - domain 0.

    All are settings within the profile's ranges.
  - **Settings:**

    | Key | Meaning |
    |---|---|
    | `net/ptp/enabled` | on or off |
    | `net/ptp/interface` | which interface |
    | `net/ptp/domain` | the PTP domain |
    | `net/ptp/profile` | `media` or `default` |
    | `net/ptp/transport` | `udp` or `l2` |
    | `net/ptp/priority1`, `net/ptp/priority2` | BMCA priorities |
    | `net/ptp/slave-only` | never become master |
    | `net/ptp/sync-interval`, `net/ptp/announce-interval` | message rates |
  - **`ptp` shows:**
    - the role and the grandmaster;
    - offset, path delay and frequency;
    - the foreign masters heard and BMCA's reasons.
  - **Transport:** UDP 319/320 on multicast, and layer 2; both are fast-path flows with hardware
    timestamps.
  - **As slave, it disciplines `ptpclock0`** (the MAC's 1588 clock, through the addend). **In
    either role** the system clock and the audio device's clock (`audio.md`:
    `i2s_channel_tune_rate()`/APLL) follow `ptpclock0`, so AES67's media clock has one source
    whether the board is master or slave.
  - **PPS out on a free pin** (GPIO26/27 on EXT1) puts two boards' clocks side by side on a scope:
    the honest measure of sync.
- **The test bed:** the user's PTP grandmaster (Linux, an Intel NIC with hardware timestamps,
  GPS PPS into it) on the same switch as both boards. The switch snoops IGMP but isn't PTP-aware,
  so its queueing adds delay variation; one switch keeps that small. The grandmaster's PPS and a
  board's PPS out on a scope give the true offset.
- **AES67's sending, as the user did it before:** one of the chip's timers makes the packet time;
  its interrupt wakes a high-priority task that assembles and sends the packet. That is §5.2's
  path as it stands: the send runs in the sender's task, straight into the real-time queue. The
  user's code comes in at the AES67 step.

## 11. Measuring: `netlat` and the counters

- **`netlat`** (an SDK program on the public socket API with `SO_TIMESTAMPING` and
  `SO_BMDS_FASTPATH`):
  - A sender sends packets at a fixed rate and size (1 ms, AES67-sized, by default). Each carries
    a sequence number and its intended send time.
  - The peer reflects them, or, when both clocks follow PTP, records their arrival.
- **What it reports, as histograms with minimum, percentiles and maximum:**

  | Measure | What it is | Needs |
  |---|---|---|
  | Send jitter | Intended time → hardware departure stamp | Nothing but the sender |
  | Delivery latency | Hardware arrival stamp → the consumer having it | Nothing but the receiver |
  | Round trip | Out and back | A reflector |
  | One-way delay and its variation | Departure → arrival | Both boards on PTP |

  Plus loss and reordering.
- **Under load:** `netlat --load` runs bulk TCP at the same time, to show the real-time queue
  holding against it. That is the test of §5.
- **Against Linux:** a small reflector script (Python, UDP) lets a PC be the peer; the user's
  PTP server, with hardware timestamps, gives exact stamps on the PC's side. Between the two
  boards, once both follow PTP, one-way delay is measured directly.
- **In the kernel:** the driver's counters (§4) show the deepest queue and the receive latency
  without a tool.

## 12. Steps

| Step | What | Done when |
|---|---|---|
| 1 | **Driver.** EMAC and IP101 bring-up; read the feature register (AV channels?); rings, zero-copy buffers, the 1588 clock running, link timer; `net` shows link and counters. **Done** (§14). | Frames out and in (an ARP request answered by hand); the feature register's answer in `docs/platforms/esp32p4.md` |
| 2 | **lwIP.** Vendored; `Netif` glue; DHCP or static from sysconf (live); ARP, ICMP, IGMP and the MAC filters; `lo`; the shell's `ping`. Host tests: lwIP builds on the host against a loopback netif. | Ping both ways, DHCP lease, `net` shows addresses and groups; flood ping without leaks |
| 3 | **Sockets.** `Socket` as a `Stream`; the slots; `linux.c` (sockets, ioctls); the `/etc` pseudo filesystem; DNS. | BusyBox `nc`, `wget`, `telnet` and `ping` work; `poll` over sockets; throughput measured (TCP both ways) |
| 4 | **NTP and mDNS.** The kernel NTP client disciplining the clock; `ntp`; `${HOSTNAME}.local`. | The clock within a few ms of the pool after a boot; `ping bmds-….local` from a PC |
| 5 | **SSH.** Dropbear (server, client, scp) as SDK programs; the `secret` setting type, `system/password` and `passwd`; host keys; authorized keys; the server's on/off setting; our `sftp-server`. | `ssh` with a key and with a password, `scp`, `sftp`, `sshfs` and a file manager's `sftp://` from Linux; `ssh`/`scp` from the board to Linux |
| 5a | **Crypto.** `bmds_crypto` over the engines; SDK calls; Dropbear's hardware descriptors; software-against-hardware measurements decide the server's algorithm offer. | Transfer speed and login time measured both ways, recorded here |
| 6 | **Real time.** The classifier and flows; fast-path sockets; the real-time transmit queue (the bounded ring); hardware timestamps through `SO_TIMESTAMPING`; `netlat`. | `netlat` histograms with and without bulk load, recorded here |
| 7 | **PTP.** The service in both roles, the profiles, BMCA and failover, `ptpclock0` discipline, PPS out; the system clock follows. | Both boards locked to the user's grandmaster; with it off, the two boards alone elect a master and lock; unplugging the master hands over without a jump; interoperates with linuxptp `ptp4l` both ways; PPS offsets on a scope recorded |
| 8 | AES67 | `audio.md` step 10 |

- **The order puts SSH before real time,** as asked. The driver's design (queues, timestamps,
  zero-copy buffers) is real-time-ready from step 1, so step 6 adds a path rather than reworking
  one.
- **Each step:**
  - keeps the build warning-free and the host tests passing;
  - fuzzes what parses network input that is ours (the classifier, the NTP and PTP packet
    parsers, `sftp-server`);
  - is recorded here as built.

## 13. Decisions (the user's, 2026-10-10)

1. **SSH login by keys and by password.** The password is a salted hash in sysconf. The server
   is off by default; the user turns it on.
2. **Test equipment:** a second board; Linux machines with hardware timestamps, among them a PTP
   grandmaster (Intel NIC, GPS PPS); an IGMP-snooping switch, not PTP-aware, with the boards and
   the grandmaster on the same switch.
3. **The earlier AES67 work** is shared at the AES67 step: a timer makes the packet time, and its
   interrupt drives a high-priority task that builds and sends the packet (§10).
4. **Dropbear built with the SDK, run from the card.** Not OpenSSH. A network file protocol that
   maps to a filesystem is what matters; SFTP with our own server is proposed (§9.2).
5. **`/etc` is a pseudo filesystem** built from sysconf; all system settings stay in sysconf.
6. **Explore the crypto engines** where they apply (§9.3).
7. **PTP as client and server:** two BMDS boards alone on a network do AES67, BMCA choosing the
   grandmaster (§10).

## 14. As built: step 1, the driver (2026-10-10)

- **`bmds_net`**, a new component: `EthernetDevice` (`ethernetdevice.h`, target), Ethernet
  framing and ARP (`ethernet.h`, portable, host-tested), the `net` command. Boot stage `net`,
  after USB.
- **On ESP-IDF's register layer** (`hal/emac_ll.h`, `soc/emac_periph.h`'s pin tables), the
  bring-up following `esp_eth`'s order. ESP-IDF's `emac_hal.c` isn't built without its driver.
- **Rings and buffers:** one internal-DMA block (63.5 KB): 16 receive and 16 transmit
  descriptors (64 bytes, a line each), 24 receive buffers (the ring's 16 and 8 spares) and 16
  transmit buffers, 1536 bytes each. Received buffers go to the receiver as they are
  (`RxFrame`, `release()` from any task); a ring short of buffers is refilled when they come
  back. Sending copies into the descriptor's buffer.
- **One task per device** (`eth0`, `CONFIG_BMDS_NET_PRIORITY`, 21, above audio's 20, 4 KB stack):
  receive, refill, reclaim, the link once a second. The ISR only records the DMA's status and
  wakes it. Step 6 may split the real-time path off.
- **The 1588 clock** runs from boot: the crystal, 40 ns steps (the fine method's addend), set
  from the system clock if a source has set it. Every received frame and every frame sent with
  `TxStamp` is stamped. `setPtpTime()`, `setPtpFrequency()` (ppb, through the addend).
- **The PHY is kept across reboots:** if the MAC's reset gets the PHY's reference clock, the PHY
  isn't reset and a link it has is adopted (link up at 0.51 s after reset). Only a PHY that
  gives no clock is reset. The reason, found the hard way: after each reset the switch forwarded
  nothing for 13-20 s, which looked like a dead receive path (`docs/HARDWARE.md`).
- **The link** comes from the PHY's status and both sides' abilities; the MAC and its DMA run
  only while it is up (ESP-IDF's order).
- **`net`:** state, the feature register decoded, counters, the 1588 clock; `net phy [REG
  [VALUE]]`; `net phyreset`; `net arp ADDRESS [-s FROM]` (a probe from 0.0.0.0 by default: sends,
  waits for the reply, prints the time between the two hardware stamps); `net regs` (the DMA's
  and MAC's state).
- **Measured on the board** (100 Mbit/s full duplex, the user's LAN):
  - the gateway's ARP reply 52-57 µs after the request left (hardware stamps both ends);
  - 5 reboots × 3 probes, all answered (link kept);
  - 0 receive errors, 0 dropped buffers in all of it.
- **Firmware:** +15 KB (1 571 488 → 1 586 624 bytes).
- **Not yet:** throughput (needs the IP stack: step 2's flood ping and step 3's TCP), the
  address filters' count on this chip (eight in the register file; step 2 programs them), PPS
  output.
