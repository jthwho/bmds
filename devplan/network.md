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
  - **Filters:** perfect address filters (15 in the register file, 8 on this chip: §16) and VLAN
    tags.
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
    | `net/ssh/host-keys` | path | `/boot/ssh` | a directory; keys generated on first start |
    | `net/ssh/authorized-keys-dir` | string | `${HOME}/.ssh` | holds OpenSSH's `authorized_keys` (§24) |
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
  - **Keys:** `authorized_keys` (OpenSSH's format) in `net/ssh/authorized-keys-dir` (§24).
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
    §9.3 makes it worth having: it doesn't, §26).
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
  Measured in §26-27: 25519 needed a faster implementation, not an engine; the offer stays.
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
| 2 | **lwIP.** Vendored; `Netif` glue; DHCP or static from sysconf (live); ARP, ICMP, IGMP and the MAC filters; `lo`; the shell's `ping`. Host tests: lwIP builds on the host against a loopback netif. **Done** (§16). | Ping both ways, DHCP lease, `net` shows addresses and groups; flood ping without leaks |
| 3 | **Sockets.** `Socket` as a `Stream`; the slots; `linux.c` (sockets, ioctls); the `/etc` pseudo filesystem; DNS (§18). | BusyBox `nc`, `wget`, `telnet` and `ping` work; `poll` over sockets; throughput measured (TCP both ways) |
| 3a | **The tray's network icon** (§17). **Done** (§20). `NetStatus` (public kernel API, change-driven), the SDK's calls for it; `NetworkStatus` in the panel's tray; `panel/tray/network`. | Unplugging and replugging the cable, DHCP and static changes and `enabled` off show in the icon and the tooltip at once, with no timer running |
| 4 | **NTP and mDNS.** The kernel NTP client disciplining the clock; `ntp`; `${HOSTNAME}.local`. | The clock within a few ms of the pool after a boot; `ping bmds-….local` from a PC |
| 5 | **SSH.** Dropbear (server, client, scp) as SDK programs; the `secret` setting type, `system/password` and `passwd`; host keys; authorized keys; the server's on/off setting; our `sftp-server`. | `ssh` with a key and with a password, `scp`, `sftp`, `sshfs` and a file manager's `sftp://` from Linux; `ssh`/`scp` from the board to Linux |
| 5a | **Crypto.** `bmds_crypto` over the engines; SDK calls; Dropbear's hardware descriptors; software-against-hardware measurements decide the server's algorithm offer (§26). | Transfer speed and login time measured both ways, recorded here |
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
  address filters' count on this chip (eight, step 2 found: §16), PPS output.

## 15. Step 2's design: lwIP (2026-10-11)

- **Vendored:** lwIP 2.2.1 in `thirdparty/lwip`: `src/core` (with `ipv6/`, not built yet),
  `src/include`, `src/netif/ethernet.c`, `src/apps/mdns` (step 4). No patches so far.
- **`NO_SYS=1`, with our own network task** (`net`, `CONFIG_BMDS_NET_STACK_PRIORITY`, 6: above
  the window manager, below audio), not lwIP's `tcpip` thread:
  - lwIP runs only under **the core lock** (a `Mutex`), taken by the network task and, from
    step 3, by sockets in their callers' tasks. `LWIP_ASSERT_CORE_LOCKED` checks it.
  - The task sleeps on one semaphore until lwIP's next timeout (`sys_timeouts_sleeptime()`) or a
    wake: frames in, a call queued, a setting changed. Nothing ticks.
  - No `sys_arch` threads, mailboxes or semaphores: lwIP's `sockets.c`/`netconn` aren't built.
  - Loopback (`lo`, and packets to the interface's own address) schedules `netif_poll` on the
    task through `tcpip_try_callback`, which BMDS provides (`LWIP_NETIF_LOOPBACK_MULTITHREADING`).
- **Memory:**
  - lwIP's heap (`mem.c`, 256 KB) is one block of PSRAM (MemSpace `Psram`) allocated at boot;
    its pools (`MEMP_MEM_MALLOC`) are carved from it too, so lwIP adds nothing to internal RAM
    but its globals.
  - Received frames stay in the driver's buffers (internal DMA memory): each buffer has a
    `pbuf_custom` whose free gives it back. When lwIP holds more than 6 of them (TCP's queues),
    frames are copied into the heap instead, so the ring never starves.
  - Sending gathers the pbuf chain into a transmit buffer (one copy, as before).
- **Checksums in software for now.** The MAC can insert them, but only with transmit
  store-and-forward, and the transmit FIFO's size is not known yet. Step 3 measures it.
- **The interface (`NetInterface`)** joins an `EthernetDevice` and lwIP's `netif`:
  - received frames: the driver's task queues them, the network task feeds them to lwIP;
  - the link: `netif_set_link_up/down` from the driver's link handler;
  - the settings: `net/eth0/{enabled, mac, mtu, ipv4/mode, ipv4/address, ipv4/gateway}`,
    declared when the interface appears and watched, so a change applies at once;
  - DHCP sends `system/hostname`;
  - multicast: lwIP's IGMP asks for MAC filters; a reference-counted list
    (`MulticastFilter`, portable) goes to the driver, which uses its perfect filters, or passes
    all multicast when there are more groups than filters (IP's own check then drops the rest).
- **`ping`** in the shell, on a raw ICMP PCB: `-c`, `-i`, `-s`, `-W`, `-f` (flood), statistics as
  iputils prints them, Ctrl-C.
- **`net`** adds the addresses, gateway, DHCP's state, the groups joined, and lwIP's memory use.
- **Host tests:** lwIP with the same `lwipopts.h`, two interfaces wired back to back: ARP, ping,
  UDP, IGMP's filter requests; `MulticastFilter`.

## 16. As built: step 2, lwIP (2026-10-11)

- **As designed in §15,** with these details:
  - `NetStack` (`netstack.h`): the boot stage `ip` after `net`, the core lock (`NetStack::Locker`;
    releasing it wakes the task, whose timers the holder may have moved), the task, lwIP's port.
  - `NetInterface` (private: it shows lwIP's types): received frames queued by the device's task
    in a ring as long as the device's buffers (it can't overflow); the link from a new driver
    callback (`setLinkHandler()`); `net/eth0/*` declared with the new
    `SysConf::declareKernel()`, which keeps a parameter's check (`declare()` drops a program's,
    whose code may go away), so a bad address, gateway or MAC is refused by `sysconf set`.
  - The driver gained gathered sends (`transmit()` over pieces), `setAddress()`,
    `setMulticast()` (the probed filters, else pass-all), and a receiver swap that waits for a
    call to the old receiver to end (`net arp` now passes frames on to lwIP while it listens).
  - Changing `net/eth0/mac` on DHCP releases the lease and asks again with the new address; a
    gratuitous ARP tells the neighbours. The address is logged once a change has applied.
  - `net join GROUP` / `net leave GROUP`: IGMP on eth0 for testing until sockets can (step 3).
- **Measured on the board** (the user's LAN, the gateway on the same switch):
  - DHCP lease about 50 ms after the stage starts (the link kept across the reset: 0.62 s after
    reset).
  - Ping to the gateway: 0.15-0.30 ms; 4000 and 6000 bytes (fragmented both ways) fine.
  - Flood ping (`-f`) to the gateway: 5000 in 1.03 s, 0 lost; 1000 of 1472 bytes (full frames)
    in 0.51 s, 0 lost. From this host (Wi-Fi), 1000 of 1400 bytes every 5 ms, 0 lost.
  - **No leaks:** lwIP's heap back to 284 bytes after every run (the most used: 6.5 KB); no
    driver buffers held; 0 receive errors, 0 dropped buffers, nothing copied (lwIP never held
    more than 5 buffers).
  - Live settings: static address and back to DHCP, a MAC override (seen in this host's ARP
    cache) and back, `enabled` off and on: each applied at once.
  - The link: a PHY reset makes sends fail ("Network is unreachable") while it is down, 3 s;
    after it comes back the switch forwards nothing for 22 s (§14, `docs/HARDWARE.md`); the lease
    is confirmed after.
  - Multicast: the board answered this host's ping to 224.0.0.1 (the all-hosts group, in an exact
    filter; pass-all is off by default, so the filter passed it); "pass all multicast" follows
    the group count (frame filter `00000010` with 11 groups, `0` with 1). **Not verified on the
    wire:** groups beyond 224.0.0.1 and the pass-all path. Multicast from this host (Wi-Fi)
    barely reaches the wired side (2 of 100 frames for 224.0.0.1, none for 239.x); a wired
    sender is needed: the second board in step 3, or one of the user's Linux machines.
- **Resources:** firmware +57.7 KB (1 586 624 → 1 644 320 bytes); `bmds_net`'s static internal
  RAM 5.2 KB in all (driver, interface, lwIP's globals); the network task's stack 6 KB (2.2 KB
  used at most under flood and DHCP: kept until TCP is measured in step 3); lwIP's heap 256 KB of
  PSRAM.
- **Host tests** (`tests/host/lwip.cpp`): lwIP with the firmware's `lwipopts.h` on two interfaces
  back to back: ARP and ping both ways, an address nobody has (ARP gives up, the packet freed),
  UDP, IGMP's filter requests (one per MAC address, with users counted) and IP dropping a group
  not joined, multicast ping, loopback through the scheduled poll; lwIP's heap checked after
  each. `MulticastFilter`, `Ipv4` (CIDR), `declareKernel()`.
- **Not yet:** checksum offload (§15), the receive path's copy threshold under TCP (step 3),
  DNS (step 3), hardware stamps for lwIP's sockets (step 6).

## 17. The tray's network icon (the user's, 2026-10-11)

Built in step 3a: §20 has what changed on the way (the tooltip's length, the lease's time left
in the menu, a drawn icon).

A network icon in the panel's tray, its tooltip the state of every interface. Step 3a, after the
sockets' ABI (step 3), whose slots it appends to.

- **Where:** `NetworkStatus`, a `TrayIcon` in the panel's process (category Hardware), as the
  volume control and the CPU meter are (`gui.md` §4s, `sysmon.md` §3.5): no process or task of its
  own. It uses only public API (the built-in apps' rule).
- **What it reads: `NetStatus`**, a new public kernel API (`bmds/netstatus.h`), so the panel never
  sees lwIP or the driver:
  - `count()`, and `info(index, Info &)`: a snapshot taken under the core lock. `Info`: the name,
    MAC address, enabled, link (up, speed, duplex), the IPv4 mode (DHCP, static, off), address and
    prefix, gateway, DHCP's state (asking, bound), server and lease left; counters (bytes and
    frames each way, errors). Later steps add DNS servers (3), NTP's sync (4) and PTP's state (7).
  - **Change-driven:** a watch released on every change to any of it but the counters (link,
    addresses, DHCP's state, settings). The tray icon waits on it in the toolkit's loop, as it
    waits on sysconf's watches: no timer. The lease's time left is worked out when the tooltip is
    drawn.
  - `renewLease(index)` for the menu below.
  - **For programs** the same as SDK calls: `bmds_net_info(index, bmds_net_info_t *)` (a
    boundary struct starting with `uint32_t size`), brought forward to step 3 for the interface
    `ioctl`s (§18), and `bmds_net_watch()` (step 3a appends it), a descriptor that is readable
    after a change (as `sysconf_watch` is).
- **The icon:** the theme's `network-wired`, drawn in four states:
  - connected (an interface has a link and an address);
  - acquiring (a link but no address yet: DHCP asking, or static without one), amber;
  - no link (cable out), dimmed with a cross;
  - off (every interface disabled or `ipv4/mode` off), dimmed.
  With several interfaces it shows the best one's state.
- **The tooltip**, one block per interface:

  ```
  eth0: connected, 100 Mbit/s full duplex
    172.20.200.221/16, gateway 172.20.0.1
    DHCP from 172.20.0.1, lease 1 h 52 min left
    MAC e8:f6:0a:e2:e2:22
  ```

  - static: "static" in the DHCP line's place; no link: "eth0: cable unplugged"; off: "eth0: off".
  - Later lines: DNS servers (step 3), "clock synced to pool.ntp.org" (step 4), the PTP role and
    offset (step 7).
- **A click:** a menu with the same lines; Copy Address; Renew Lease (DHCP only); Network
  Settings... (the control panel, later); Remove from Panel.
- **`panel/tray/network`** (bool, default true; `gui.md` §4t), as `panel/tray/cpu`.
- **Not done (the user, 2026-10-11):** a notification when a link goes down or comes up, or an
  address is gained or lost. The icon is enough.

## 18. Step 3's design: sockets, `/etc`, DNS (2026-10-11)

- **The kernel's `Socket`** (`bmds_net`, public `bmds/socket.h`, no lwIP types in it) is a
  `Stream`, so descriptors, `read`/`write`, `dup`, inheritance, `poll` and `close` work as they do
  for pipes (§7). Step 3 has `AF_INET` with `SOCK_STREAM` (TCP), `SOCK_DGRAM` (UDP) and
  `SOCK_RAW` with `IPPROTO_ICMP` (BusyBox `ping`). `AF_PACKET` comes with PTP (step 6/7); `AF_UNIX`
  and with it `socketpair` after the first SSH (step 5).
  - **lwIP's raw API under the core lock**, in the caller's task: a call locks, works on its PCB,
    unlocks, and waits outside the lock (`Interrupt::waitCurrent`, so Ctrl-C and signals give
    `EINTR`). lwIP's callbacks (received, sent, accepted, connected, error) run in the network
    task, change the socket's state, release its semaphores and notify its `WaitList` (`poll`).
    lwIP's `sockets.c` and `netconn` stay unbuilt.
  - **Memory:** a socket is one `Psram` allocation (about 200 bytes), freed when its last handle
    closes. Data waiting lives in lwIP's heap (256 KB of PSRAM): TCP keeps the received pbufs and
    opens its window as they are read (`TCP_WND`, 8 segments); UDP and raw sockets queue
    datagrams with their source up to the receive buffer (`SO_RCVBUF`, 16 KB by default) and
    count what they drop. A pbuf over a driver buffer that a socket holds counts against the
    receive path's copy threshold (`MaxHeld`, §15), which step 3 measures under TCP.
  - **Sending:** TCP copies into lwIP's send buffer (`TCP_SND_BUF`, 8 segments) and waits for
    the `sent` callback when it is full; one `tcp_output` per call. UDP and raw: a pbuf per
    datagram, sent at once.
  - **Listening:** lwIP's backlog; each connection lwIP accepts gets its socket at once (data may
    arrive before `accept()`), queued on the listener until `accept()` takes it.
  - **Closing** (the last handle): a TCP socket with unread data is reset, as Linux does;
    otherwise `tcp_close`, and lwIP finishes sending and closing on its own (`SO_LINGER` with 0
    resets). The PCB's callbacks are cut first, so nothing reaches a freed socket.
  - **Not waiting:** every socket call that can wait takes `MSG_DONTWAIT` (Linux's value) in its
    flags: `sendmsg`, `recvmsg`, `accept` and `connect` (which then answers `EINPROGRESS`; `poll`
    for writable and `SO_ERROR` tell the outcome). `O_NONBLOCK` stays `linux.c`'s, per descriptor
    (programs.md §16); `linux.c` passes `MSG_DONTWAIT` for a non-blocking descriptor, and its
    non-blocking `read`/`write` try `recvmsg`/`sendmsg` first (`ENOTSOCK` falls back), so a
    non-blocking TCP write takes what fits instead of waiting for all of it.
  - **Readiness:** readable with data, a connection to accept, the peer's FIN (reads end) or an
    error; writable with room in the send buffer, or once a connect has ended; an error or reset
    sets `HungUp` and `Broken` (`POLLHUP`, `POLLERR`). Writing after a reset gives `EPIPE` and
    `SIGPIPE`, unless `MSG_NOSIGNAL`.
  - **Options** (Linux's numbers): `SO_REUSEADDR`, `SO_KEEPALIVE` with `TCP_KEEPIDLE`,
    `TCP_KEEPINTVL`, `TCP_KEEPCNT`; `SO_RCVBUF`, `SO_SNDBUF` (the fixed size), `SO_RCVTIMEO`,
    `SO_SNDTIMEO` (kept in the kernel, so `read` honours them), `SO_ERROR`, `SO_TYPE`,
    `SO_DOMAIN`, `SO_PROTOCOL`, `SO_ACCEPTCONN`, `SO_BROADCAST`, `SO_LINGER`, `SO_BINDTODEVICE`;
    `TCP_NODELAY`; `IP_TTL`, `IP_TOS`, `IP_ADD_MEMBERSHIP`, `IP_DROP_MEMBERSHIP`,
    `IP_MULTICAST_IF`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`. Others answer `ENOPROTOOPT`.
    `SO_TIMESTAMPING`, `SO_PRIORITY` and `SO_BMDS_FASTPATH` are step 6's.
- **The ABI: 12 slots appended** (96-107), each socket slot's address in Linux's `sockaddr`
  layouts with its length (so `AF_INET6`, `AF_PACKET` and `AF_UNIX` need no new slots):
  - `socket(family, type, protocol, flags)`: `flags` `BMDS_SOCK_CLOEXEC`;
  - `bind(fd, addr, len)`, `listen(fd, backlog)`;
  - `accept(fd, addr, len *, flags)`: `BMDS_SOCK_CLOEXEC` for the new one, `BMDS_SOCK_DONTWAIT`;
  - `connect(fd, addr, len, flags)`: `BMDS_SOCK_DONTWAIT`;
  - (`accept` and `connect` have BMDS's own flag: `BMDS_O_CLOEXEC` is 0x40, Linux's
    `MSG_DONTWAIT`, so the two can't share a word. `sendmsg` and `recvmsg` take Linux's
    `MSG_*`);
  - `sock_name(fd, which, addr, len *)`: its own address or the peer's (one slot for
    `getsockname` and `getpeername`);
  - `sendmsg(fd, const bmds_msghdr_t *, flags)`, `recvmsg(fd, bmds_msghdr_t *, flags)`:
    `bmds_msghdr_t` starts with `uint32_t size` (`docs/ABI.md`); its `iovec`s and control
    messages are Linux's layouts. `send`, `recv`, `sendto` and `recvfrom` are these in `linux.c`;
  - `setsockopt(fd, level, name, value, len)`, `getsockopt(fd, level, name, value, len *)`;
  - `shutdown(fd, how)`;
  - `net_info(index, bmds_net_info_t *)`: one interface's state, step 3a's `NetStatus::info()`
    brought forward because `linux.c` needs it for the interface `ioctl`s (`SIOCGIFCONF`,
    `SIOCGIFADDR`, `SIOCGIFNETMASK`, `SIOCGIFBRDADDR`, `SIOCGIFFLAGS`, `SIOCGIFHWADDR`,
    `SIOCGIFINDEX`, `SIOCGIFNAME`, `SIOCGIFMTU`) and `if_nametoindex`. Setting addresses is
    sysconf's. musl's `getifaddrs` and `if_nameindex` use netlink, which the plan leaves out:
    they fail for now (step 5 decides whether Dropbear needs them).
  - `sys.h` gets `bmds_sockaddr_in_t`, `bmds_iovec_t`, `bmds_msghdr_t`, `bmds_net_info_t` and the
    family, type and flag constants the slots use; option numbers are Linux's.
- **`/etc`** (§8): a virtual directory in the VFS, as `/share/zoneinfo` is. Components register
  its files at boot (`Vfs::addEtcFile(name, generate)`); opening one runs its generator into a
  read-only, seekable memory stream (`Psram`), so a file is a snapshot of the settings when it
  was opened. `stat` says 0 bytes, as Linux's `/proc` does: a generator reads settings, whose
  store is on `/boot`, so it never runs under the VFS's lock (sysconf writes `/boot` under its
  own). Step 3's files:
  - `resolv.conf`: `nameserver` for each of `net/dns/servers`, or DHCP's when that is empty;
    `search` from `net/dns/search`;
  - `hosts`: `127.0.0.1 localhost`, each interface's address with `system/hostname`, and
    `net/hosts`;
  - `hostname`;
  - `services` and `protocols`: rodata tables of the common ones.
  `passwd` and `group` come with Dropbear (step 5), which decides the user's shell and home.
- **DNS:**
  - In programs, musl's resolver, from `/etc/resolv.conf` and `/etc/hosts`: nothing in the kernel.
  - In the kernel, lwIP's `dns.c` (`LWIP_DNS`) for the shell's `ping NAME` now and NTP in step 4,
    with `net/hosts` as its local table.
  - DHCP's DNS servers (option 6) are caught by `LWIP_HOOK_DHCP_PARSE_OPTION` per interface,
    not by lwIP's own handling (which would overwrite the setting); the same hook takes NTP's
    (option 42) for step 4. `net/dns/servers` wins when set.
  - New keys: `net/dns/servers`, `net/dns/search`, `net/hosts` (§6's table), declared by
    `bmds_net` and watched.
- **Order of work:**
  1. `Socket` with host tests: two lwIP interfaces back to back (§16), a thread standing in for
     the network task, sockets used from test threads: TCP connect, accept, both ways, half
     close, reset, refused, timeouts, `MSG_DONTWAIT`, readiness; UDP with multicast; ICMP raw.
  2. The slots, `linux.c`, the ABI lock test; `sdk/tests` programs.
  3. `/etc`, DNS, the DHCP hook; `ping NAME`.
  4. BusyBox's `nc`, `wget`, `telnet`, `ping`, `nslookup` on (`dist/busybox/config`); TCP
     throughput both ways (with `nc` and the host's `nc`; the host is on Wi-Fi, so a wired
     peer's figure is wanted too); the network task's stack under TCP; the copy threshold;
     the transmit FIFO and checksum offload (§15).

## 19. As built: step 3 so far (2026-10-11)

- **The kernel's `Socket`** (`bmds/socket.h`, `socket.cpp`), as §18 designed:
  - TCP, UDP, and raw sockets of one protocol (ICMP), each a `Stream` in `Psram`, freed with its
    last handle; lwIP's raw API under the core lock in the caller's task, waits outside it.
  - TCP keeps lwIP's segments (window 8 segments); UDP and raw keep up to 16 datagrams or
    `SO_RCVBUF` bytes (16 KB) and count what they drop. A datagram in a driver buffer is copied
    as it is queued, so a socket nobody reads never holds the receive ring's buffers; raw
    sockets always copy (lwIP goes on with the packet: ICMP answers the echo).
  - Options are kept in the socket and applied to its PCB, and an accepted connection inherits
    the listener's (lwIP's listening PCB has no room for most of them).
  - A close with data unread, or `SO_LINGER` 0, resets; otherwise lwIP closes on its own. The
    listener's connections not yet accepted close with it.
  - New `Error` codes for sockets (`AddressInUse`, `ConnectionRefused`, `ConnectionReset`,
    `InProgress`, `NetworkUnreachable`, ...), and their `BMDS_E*` numbers (Linux's).
  - `NetStack::unlockQuietly()`: `poll` asks a socket's readiness under the lock without waking
    the network task each time.
  - `bmds_netif_named()` (the port): `SO_BINDTODEVICE` takes BMDS's names (`eth0`, `lo`), not
    lwIP's two letters.
- **The ABI:** slots 96-107 as §18 (with the flags noted there), `bmds_sockaddr_in_t`,
  `bmds_iovec_t`, `bmds_msghdr_t` and `bmds_net_info_t` (120 bytes: an explicit reserved word
  keeps its counters 8-aligned), all in the ABI lock. `netcalls.cpp` turns Linux's option
  numbers, both `SO_RCVTIMEO` layouts (musl tries the 64-bit one first), `ip_mreq`/`ip_mreqn`
  and `struct linger` into the socket's calls; a gone connection's send raises `SIGPIPE` unless
  `MSG_NOSIGNAL`.
- **`linux.c`:** the socket calls; `sendmmsg`/`recvmmsg` one message at a time; on a
  non-blocking descriptor the calls pass the kernel's don't-wait flags, and `read`, `write`,
  `readv` and `writev` try `recvmsg`/`sendmsg` first (`ENOTSOCK` falls back), so only
  non-blocking descriptors pay for it; `FIONBIO`; the interface `ioctl`s from `net_info`
  (`SIOCGIFCONF`, `NAME`, `INDEX`, `MTU`, `FLAGS`, `ADDR`, `NETMASK`, `BRDADDR`, `HWADDR`);
  accept, send and receive restart after a handler with `SA_RESTART`. `if_nametoindex` fails:
  musl's opens an `AF_UNIX` socket for its `ioctl`.
- **`NetStatus`** (`bmds/netstatus.h`): `count()` and `info()` for `net_info`, `lo` first (the
  interface index order). Step 3a adds its watch.
- **Names (`netnames.cpp`):**
  - lwIP's resolver on (4 entries, names to 128 bytes); DHCP's DNS and NTP servers caught from
    the ACK by `LWIP_HOOK_DHCP_PARSE_OPTION` (lwIP's own use of them is off), dropped with the
    lease; `net/dns/servers` wins when set.
  - `DNS_LOOKUP_LOCAL_EXTERN` answers `localhost`, the host name and `net/hosts` itself.
  - `NetStack::resolve()`: a lookup from any task, with a fixed pool of four; one that times
    out keeps its slot until lwIP answers, so a late answer never reaches a dead stack.
  - `ping` takes names (`ping pool.ntp.org`).
  - `/etc`: `resolv.conf`, `hosts`, `hostname`, `services`, `protocols`.
- **BusyBox:** `nc` (with `-l` and the extras), `wget`, `telnet`, `ping`, `nslookup` and
  `/etc/services` lookups on. `nslookup` is the full one (`FEATURE_NSLOOKUP_BIG`): the small
  one prints the server from glibc's `_res`, which musl hasn't (`can't resolve '(null)'`).
- **Host tests:** the lwIP harness split out (`lwipwire.{h,cpp}`) with a network thread and the
  core lock checked while it runs; `tests/host/socket.cpp`: TCP 200 KB one way and gathered
  pieces back, half closes, refusals, connects and accepts without waiting, peeks, a receive
  timeout, resets (linger 0, data unread), an interrupted receive, UDP (sources, a cut
  datagram, the queue's drops, connected, shut), multicast with and without loopback, raw ICMP,
  inherited options, a listener closed with connections waiting; lwIP's heap back where it was
  after each. `/etc` in the VFS tests.
- **Resources:** firmware 1 673 520 bytes (+29.2 KB on step 2).
- **`lo`'s MTU** is 65535: lwIP leaves it 0, its "no limit", which programs would show
  (`ifconfig`'s `MTU:0`). lwIP only takes the smaller of it and `TCP_MSS` for TCP, and checks
  it for fragmenting, so nothing on the wire changes.
- **On the board (2026-10-11),** the host on Wi-Fi through the office switch:
  - `progtest net` passes; DHCP's DNS server reaches `/etc/resolv.conf`; `ping localhost`,
    `ping bmds-e2e222` (the host name, answered locally), `ping example.com`; a missing name
    fails at once with NXDOMAIN.
  - `wget` 4 MB from the host in 1.8 s; BusyBox `nslookup`; `nc` echo to the host.
  - TCP with `nc`, 8 MB each way: 19.4 Mbit/s in, 20.6-23.7 Mbit/s out, both about what this
    host's Wi-Fi gives; a wired peer is still wanted for the board's own ceiling. Over
    loopback (both ends, `dd` and the pipe on the board) 16 MB in 2.84 s, 47 Mbit/s.
  - The network task used 2.2 KB of its 6 KB stack through all of it (the sending side runs
    `tcp_output` in the caller's task); `eth0`'s task 1.8 KB of 4 KB; lwIP's heap at most
    28 KB of 256 KB; no frame errors, missed frames or drops; 27 frames copied by the receive
    threshold out of 17 560.
  - BusyBox's small `nc` (without `NC_110_COMPAT`) ends when the network side ends, as on
    Linux: a listener with `< /dev/null` half-closes at once and the sender stops early. Not
    ours; the half-close itself carries 2 MB from this host to a board listener.
- **Found missing, then built:** `alarm`/`setitimer` returned `ENOSYS` (left out in programs.md's
  signals step), so BusyBox `ping` sent one request and waited, and `nc -w`, `wget -T` and
  `timeout` couldn't time out. Now slot 108 (`proc_timer`, programs.md §18): `ping -c 3`,
  `nc -w 1` (1.02 s), `wget -T 2` (2.02 s) and `timeout 1 sleep 5` behave as on Linux.
- **Not yet looked at:** checksum offload and the transmit FIFO (§19's step 2 notes); a wired
  peer; `telnet` against a server.

## 20. Step 3a: the tray's network icon (2026-10-11)

§17's design, built. What changed on the way:

- **Changes are found by comparing, not by hooks alone.** Everything that may change what
  `NetStatus` reports calls `NetStatus::noteChange()`:
  - a link change (`applyLink()`);
  - settings applied;
  - lwIP's status callback (address, up/down);
  - a DHCP acknowledgement (in the option hook).
  
  At the end of each pass the network task's `NetStatus::publish()` compares every interface's
  state with the state last told. Only a real difference counts a change and tells the
  watchers. Compared: everything but the counters and the lease's time left. A pass with no
  mark costs one atomic load; nothing runs on a timer. lwIP has no hook for a lease renewed
  unchanged, and none is needed: a renewal that changes the lease's length or its DNS servers
  comes with an acknowledgement, which marks.
- **`NetStatus::Watcher`** (owned by the listener, an intrusive list):
  - The list is kept under the stack's core lock, with no lock of its own.
  - The notification runs on the network task with the core lock held, so it must not block.
  - `stop()` takes the core lock, so a notification running elsewhere has ended when it
    returns.
  - `NetStatus::changes()` is the change count.
  - `NetStatus::renewLease()` serves the menu.
  - `info()` now fills `dns` (DHCP's servers). It was in the ABI and left zero before.
- **For programs:** slot 109, `net_watch(flags)` (`BMDS_O_CLOEXEC`). It returns a descriptor:
  - A read takes one `uint32_t`, the change count, and waits while nothing changed since that
    descriptor's last read (or its opening).
  - It is readable for `poll` when something did; the reader then calls `net_info`.
  - There is no overflow to report: a count says "something changed", and that is all a reader
    needs.
  - `progtest net` checks it, and `progtest netwatch N` prints the interfaces after each change.
- **The tray's tooltips were too small:** 127 bytes and 4 lines, set by the tray protocol, the
  tray's row and the tooltip window. One interface's block is about 150 bytes in 5 lines.
  - `TrayProtocol::Update` gains `toolTipMore[256]` (appended, as the protocol allows; an older
    sender's message reads it empty).
  - `setToolTip()` and `toolTipOf()` split the text and join it again.
  - Tooltips now hold 383 bytes and 12 lines.
- **The icon is drawn, not themed:** the theme has no network icon. `NetworkView` (pure,
  host-tested) draws an RJ45 socket whose two lights tell the state:
  - connected: both green;
  - acquiring: the left one amber;
  - no link: dark, dimmed and crossed out in red;
  - off: dark and dimmed.
- **The lease's time left is in the menu, not the tooltip.** A tooltip is text sent once, and
  keeping a countdown right needs a timer. The tooltip gives the lease's length ("lease 2 h");
  the menu, read when it opens, adds "Lease: 1 h 52 min left".
- **`NetworkStatus`** lives in the panel (category Hardware) and follows `panel/tray/network`
  (default true; the panel's menu has Show Network).
  - A change posts one refresh to the loop (an atomic flag, as the folder watches do).
  - A click, left or right, opens the menu: each interface's lines, Copy Address (the first
    connected interface's), Renew *eth0*'s Lease (an interface holding a lease) and Remove from
    Panel.
- **Resources:** firmware 1 681 472 bytes (+6.4 KB on the build before it, with `alarm`). The
  watch costs nothing between changes; `publish()` keeps one `Info` per interface (`.bss`).
- **On the board (2026-10-11):**
  - `progtest net` passes (with `net_watch`'s checks).
  - `progtest netwatch` printed each change once and at once, its count running on with no
    gap: the MTU set and put back, static mode, back to DHCP (the address dropped, then
    bound), and `enabled` false and true. Setting the static address and gateway while in DHCP
    mode told nothing, as it should, since nothing that is reported changed.
  - The icon showed connected (both lights green) with the tooltip block above; with
    `enabled` false it dimmed, and the tooltip, open under the pointer, changed to "eth0: off"
    at once.
  - A left click opened the menu, which showed "Lease: 1 h 58 min left".
    - Copy Address put `172.20.200.221` on the clipboard (`clip paste`).
    - Renew eth0's Lease renewed it (7200 of 7200 s left after).
    - Remove from Panel saved `panel/tray/network` false and the icon went; the default
      brought it back.
  - The user pulled the cable and put it back: the icon went to the crossed-out socket, then
    back to connected with the right address in the tooltip, and the board answered pings
    from their PC.
- **No notification** when a link or an address comes or goes: the user decided against §17's
  proposal (2026-10-11).

## 21. The network in sysmon (the user's ask, 2026-10-11)

The network's counters join the system's registry (`sysmon.md` §3.2, §6), so `stats`, `top`,
the System Monitor and the tray meter's alarms see the network like any subsystem.
- **`net.ethN`,** one group per interface (`netstats.cpp`), read under the core lock:
  - the link (state `down|up`), its speed;
  - bytes and frames each way, both bytes kept for graphs;
  - receive and send errors, and frames dropped either way: any increase warns, as the
    rule `zero` does;
  - frames copied by the receive threshold, receive buffers held, link changes.
- **`net.ip`, the stack's group:**
  - lwIP's heap: in use (kept for graphs), the most in use (a peak: warns past ¾ of the
    256 KB heap, critical past 15/16), allocations refused (warns);
  - allocations its pools refused (warns);
  - TCP connections open.
  - lwIP's MIB-II counts, `MIB2_STATS` turned on (about 200 bytes of counters and an increment
    per packet):
    - IP packets in and out, and those dropped coming in;
    - TCP opens, both active and passive, attempts that failed, segments each way, fast
      retransmits, segments with errors;
    - UDP datagrams each way, those sent to no open port, those with errors;
    - ICMP messages each way.
- **What lwIP's MIB-II counters don't mean,** found on the board:
  - `tcpEstabResets` is counted for every close lwIP starts from an open connection, a FIN
    included, so every `wget` "reset" its connection. It isn't offered.
  - `tcpRetransSegs` counts only fast retransmits, so the counter is `tcp_fast_retransmits`.
  - A ping's replies are taken by its raw socket before ICMP sees them, so `icmp_in` doesn't
    count them.
- **The groups are added after the interfaces,** without the core lock: the registry's lock is
  held while a read takes the core lock, so nothing holding the core lock may add a group.
- **The System Monitor:**
  - a Network tab, with graphs of eth0 received and sent over the `net.*` counters;
  - a line on the Overview: "Network: in 3.0 KB/s, out 1.0 KB/s", every interface's bytes
    together.
  - The window had nine tabs, past `TabWidget::MaxTabs` (8): the window failed to open, so the
    limit is 16 now.
- **`top`'s headline** names each interface with traffic, after the storage devices: `eth0 in
  1.2 KB/s out 300 B/s`.
- **On the board (2026-10-11):**
  - `stats 'net.*'` and `stats rules 'net.*'` list both groups and their rules.
  - After `wget`s, the TCP opens and segments, UDP (the DNS lookups) and IP counts moved as
    expected.
  - The System Monitor's Network tab showed the received and sent graphs and the counters, and
    the Overview its Network line.
  - `top -n 5` during 30 `wget`s gave `..., eth0 in 14.2 KB/s out 4.8 KB/s`.
  - Firmware 1 687 568 bytes (+6.1 KB).

## 22. Step 4's design: NTP and mDNS (2026-10-11)

- **The host name first** (the user's ask): this board is `bmds0` now (`sysconf set
  system/hostname bmds0`, saved). A change of `system/hostname` renews a DHCP lease at once, so
  the DHCP server, and the DNS it feeds, learn the new name without waiting for the lease's
  renewal. mDNS answers for the new name at once too (below).
- **NTP: a kernel service on lwIP's raw UDP API**, in the network task under the core lock, run
  by lwIP's timers (`sys_timeout`): nothing runs between polls. `src/ntpclient.cpp`.
  - **Servers:** each of `net/ntp/servers` (names or addresses, `;`-separated, at most 4; default
    `pool.ntp.org`), and each interface's DHCP option 42 (two at most), 6 in all. A name is
    resolved through lwIP's resolver when it is needed, and again after 8 polls without an
    answer (a pool gives another server).
  - **Requests:** one UDP socket on a random port. A request's transmit timestamp is 64 random
    bits, not the time (RFC 9109's advice): a reply must echo them as its origin, from the server's
    address, or it is dropped. T1 and T4 are the system clock's when the request goes and the
    reply is taken (step 6's hardware timestamps can replace them).
  - **The parser** (`include/bmds/ntp.h`, pure, host-tested and fuzzed: `tests/fuzz/ntp.cpp`):
    48 bytes at least (extensions are ignored), version 1-4, mode 4 (server), the origin ours,
    the receive and transmit stamps non-zero, leap not 3 (unsynchronised), stratum 1-15, root
    delay and dispersion under 16 s. Stratum 0 is a kiss-o'-death: `RATE` doubles the poll,
    `DENY` and `RSTR` stop asking that server until the settings change. NTP's era: a seconds
    field below 2^31 is era 1, so 1968 to 2104 read right, whatever the clock says.
  - **The clock filter:** per server, the last 8 samples (offset, delay, when); the best is the
    least delay plus 15 ppm of its age (RFC 5905's dispersion growth). Jitter is the RMS of the
    other samples' offsets from the best's.
  - **Selection:** among servers with a sample, the least root distance (root delay / 2 + root
    dispersion + the best sample's delay / 2 + jitter). Not RFC 5905's intersection algorithm:
    the plan's simple selection (§10).
  - **The discipline** (pure too, `Ntp::Discipline`, host-tested), applied when the chosen server's
    newest sample is its best (a sample taken now, so its offset is now's):
    - the clock unset, or an offset beyond 128 ms: `Clock::set()` (a step, source `ntp`);
    - otherwise `Clock::adjust(offset)` (a slew at 500 ppm: 1 ms takes 2 s), and the frequency:
      the offset not explained by the slew still pending is the rate's error over the time since
      the last update, which, from 32 s on, moves `Clock::setFrequency()` by interval / (interval
      + 1024 s) of it (6% at 64 s, half at 1024 s and beyond), within ±500 ppm. A pool server's
      few ms of jitter over 64 s is tens of ppm of noise, so a short interval moves it little.
      (First built taking all of the first estimate and a quarter of each after; the board's
      pool server showed 2-5 ms of jitter, which makes that too noisy.) Since §31g, a large
      error is also taken whole once the intervals' estimates agree on it (4 standard errors).
  - **The poll:** a burst of 4 requests 2 s apart to each server at start, when the servers or
    an interface's address change, and on `ntp poll`; then every 64 s, doubling to 1024 s while
    the offsets stay within 4 jitters (at least 1 ms), and back to 64 s on a step or an offset
    beyond it.
  - **Settings:** `net/ntp/enabled` (bool, `true`), `net/ntp/servers` (string, `pool.ntp.org`).
    Live: a change restarts the client with a burst.
  - **`ntp`** shows the state (synced to which server, stratum, poll), the clock's offset, delay,
    jitter and frequency, and each server: name, address, stratum, reach (the last 8 polls,
    octal as ntpq shows it), last answer, offset, delay, jitter. `ntp poll` sends a burst now.
  - **Counters:** `net.ntp` in sysmon: `synced` (no|yes), `offset_us` (with history), `delay_us`,
    `jitter_us`, `stratum`, `poll_s`, `requests`, `replies`, `rejected`.
  - Later: the tray's "clock synced to …" line (a public status call, decided with step 7's PTP
    status), and serving time to the LAN.
- **mDNS: lwIP's responder** (`src/apps/mdns`, unpatched), for each Ethernet interface:
  - It answers for `${HOSTNAME}.local` (A records; IPv6 comes later) and probes for it first. A
    conflict (another host has the name) is logged and the responder takes `NAME-2`, `NAME-3`, …,
    as Avahi does; `net` shows the name in use.
  - It follows `system/hostname` live (`mdns_resp_rename_netif`), and lwIP's extended netif
    callback restarts it on a link or address change (`LWIP_NETIF_EXT_STATUS_CALLBACK`).
  - `net/mdns/enabled` (bool, `true`) adds or removes it live.
  - One service slot (`MDNS_MAX_SERVICES`), for step 5's `_ssh._tcp`; no searching
    (`LWIP_MDNS_SEARCH` 0). Two since network MIDI (2026-10-20, `audio.md` §23), through
    `NetServices::advertise()` (`netservices.h`), which the SSH server uses too; lwIP sends no
    goodbye when one is taken back (a browser's cache shows it a while).
  - **Kernel services on the network task** (2026-10-20): `NetUdp` (`netudp.h`), a UDP port
    whose datagrams go to a callback under the core lock, and `NetStack::callLater()`, lwIP's
    timers: a protocol with no thread of its own (network MIDI's).
  - **The board asking:** lwIP's resolver sends a `.local` name to the mDNS group
    (`LWIP_DNS_SUPPORT_MDNS_QUERIES`), so the shell's `ping other.local` works; its own
    `${HOSTNAME}.local` is answered locally. Programs' resolver (musl) asks the DNS server only.
  - **Memory:** one `mdns_host` (about 1.5 KB) per interface from lwIP's heap; packets in pbufs.
- **Tests:** `tests/host/ntp.cpp` (the parser against hand-built packets, era, offset and delay,
  the filter, the discipline's steps, slews and frequency); lwIP's host build compiles mDNS. The
  fuzz target runs the parser over arbitrary packets with arbitrary cookies.
- **Done when:** after a boot the clock is within a few ms of the pool (`ntp`'s offset, and a PC
  on NTP comparing, `date` both sides), and `ping bmds0.local` from a PC answers.

## 23. As built: step 4, NTP and mDNS (2026-10-11)

- As designed in §22, except the frequency's update, which is weighted by the interval (§22).
  Files: `include/bmds/ntp.h` and `src/ntp.cpp` (pure), `src/ntpclient.{h,cpp}`,
  `src/ntpcommand.cpp`, `src/netmdns.{h,cpp}`; lwIP's `apps/mdns` built (unpatched).
  `NetNames`' list and name helpers moved to `netnames.h` for the client's setting.
- **What it costs:** firmware 1 713 616 bytes (+26 KB, most of it lwIP's mDNS); one
  `mdns_host` per interface and one UDP PCB from lwIP's heap; the client's state is static.
- **Tests:** `tests/host/ntp.cpp` (6 cases: time stamps and eras, the request, replies good and
  bad, samples, the filter, the discipline). `ntp_fuzz` ran 117 million inputs in 13 minutes
  (old and new discipline) with no finding.
- **On the board (2026-10-11):**
  - The first boot with it synced to `pool.ntp.org` (stratum 2) within the first burst: a step,
    then offsets of a fraction of a ms. The board's clock against this PC's (NTP-synced),
    read over the serial console: -0.3 to -1.1 ms, inside the ±5 ms the console's round trip
    allows.
  - Over 30 minutes after a reflash: offsets under 1 ms but one (+4.5 ms), delay 82-87 ms,
    the server's jitter 1-3.5 ms; the poll went from 64 s to 256 s; the frequency went from
    -6.6 ppm (kept across the reset in the clock's reset record) to -1.6 ppm and was still
    settling. Requests 17, replies 17, rejected 0.
  - `ntp poll` looks the pool up again (another server answered) and bursts.
  - `stats 'net.ntp*'` shows the counters; the System Monitor's Network tab picks them up.
  - `ping bmds0.local` from the PC answers; the board pings `clarke.local` (the PC) and its own
    `bmds0.local`. `net eth0` shows `mDNS bmds0.local` and the group 224.0.0.251.
  - A live rename (`system/hostname` to `bmds1`) answers in about 3 s (probing); the old name
    stops answering (a direct one-shot query), though avahi's cache answers for it a while: lwIP
    sends no goodbye.
  - A conflict (the PC publishing `bmds2.local`, then the board renamed to `bmds2`): the board
    took `bmds2-2.local`. Renamed back to `bmds0`, it answered as `bmds0.local` again.
  - **Not settled:** the network's own DNS (172.20.0.2) still maps 172.20.200.221 to
    `bmds-e2e222` after reboots and renewals with `bmds0`. Whether the DHCP server isn't feeding
    that DNS, or keeps a fixed entry, couldn't be told from here (no capture without root).

## 24. Step 5's design: SSH, SFTP (2026-10-11)

What Dropbear 2025.88 and BMDS each need, found by reading both before porting:

- **Dropbear without `fork`:** its server accepts in one process and handles each connection in a
  child. Its re-exec mode (`DROPBEAR_REEXEC`, Linux's default) already has the child exec the
  server again with the connection on stdin (`-2 pipe`), so nothing is shared after the exec:
  exactly `vfork` + `exec`. Patch: the accept loop `vfork`s and `execv`s the server's own path
  (musl's `fexecve` needs `/proc`), and a child whose exec fails `_exit`s instead of falling back
  to an in-process session (which would run in the parent's memory). With `HAVE_FORK` undefined,
  its uClinux paths (`DROPBEAR_VFORK`) start shells and commands with `vfork` already.
  - Built as one program (`dropbearmulti`) with links: `dropbear`, `dbclient`, `ssh` (to
    `dbclient`), `dropbearkey`, `dropbearconvert`, `scp`. One image in the loader's cache serves
    the server, its sessions and the client.
  - `localoptions.h`: no utmp, wtmp, lastlog, shadow, syslog (it logs to stderr with `-E`;
    syslog came later, `logging.md`),
    multi-user changes (`DROPBEAR_SVR_MULTIUSER` 0: every process is root); `SFTPSERVER_PATH`
    is `sftp-server`, found on `PATH` by the shell, not a fixed path.
- **Programs need pseudo-terminals** (pty.md §6 left them for "when the program ABI needs them";
  Dropbear's `openpty()` does):
  - `/dev/ptmx` opens a new pair: the descriptor is the emulator's end (`Pty`), in `Psram`.
    `/dev/pts/N` is the program's end (`PtyConsole`), a directory the VFS lists while the pairs
    are open (`Vfs::DeviceDirectory`: entries named by number, from another component).
    `/dev/tty` is the caller's terminal, as `/dev/console` is.
  - **Ends are counted by their handles** (`Stream::handleClosed`): the last emulator handle
    closing hangs the program's end up (its session gets `SIGHUP`, as now); the last program
    handle closing ends the emulator's reads once the output is read; both done frees the
    pair. The terminal application's ptys keep their explicit calls.
  - **A controlling terminal:** a session leader that opens `/dev/pts/N` without `O_NOCTTY`, or
    calls `TIOCSCTTY` on it, makes it its terminal: the process's console (what termios, the
    line editor, `/dev/tty` and job control use) becomes that pty's, the console's session is
    the caller's. Children and the programs they exec inherit it, as they inherit the console
    now. `TIOCNOTTY` lets go.
  - **One slot appended, 110, `term_ctl(fd, op, arg)`:** `BMDS_TERM_PTY_NUMBER` (an emulator
    end's N: `TIOCGPTN`), `BMDS_TERM_PTY_UNLOCK` (`TIOCSPTLCK`; pairs are never locked),
    `BMDS_TERM_SET_SIZE` (columns and rows: `TIOCSWINSZ` on either end, so `SIGWINCH`),
    `BMDS_TERM_CONTROL` (`TIOCSCTTY`), `BMDS_TERM_RELEASE` (`TIOCNOTTY`). musl's `posix_openpt`,
    `grantpt`, `unlockpt`, `ptsname` and `openpty` then work unchanged.
- **The one user, in `/etc`:** `passwd` is `root:x:0:0:root:/home:/bin/sh` (`getuid()` is
  already 0; `/home` is the user's home link, sysconf.md §7) and `group` is `root:x:0:`. The
  `x` never means a hash in `/etc`: Dropbear asks the kernel (below).
- **Passwords:**
  - **`secret`, a new setting type** (`Config::Type::Secret`, `BMDS_SYSCONF_TYPE_SECRET` 7):
    stored like a string, but every reader gets `(set)` or the empty string instead of the
    value, through the shell's `sysconf` and the SDK alike. Its declaration's check decides
    what may go in.
  - **`system/password`** (secret, empty): empty, or SHA-512 crypt (`$6$rounds=N$salt$hash`,
    which musl's `crypt()` also reads). Its check takes only that form, so a plain password
    can't be stored by mistake. `/boot/sysconf` holds the hash, as `/etc/shadow` would; every
    program is root, so this is a hash's protection, not a file's.
  - **SHA-512 and SHA-512 crypt in `bmds_core`** (pure, host-tested against the algorithm's
    published vectors), in software; step 5a puts the SHA engine under them. The rounds are
    chosen when a password is set so a check takes about 0.1 s on the board (measured), and
    kept in the hash.
  - **`passwd`** (a shell command): asks for the new password twice without echo, hashes it
    with 16 random salt characters, sets and saves `system/password`. `passwd -d` clears it.
    The plain password is never stored or logged.
  - **Slot 111, `password_check(password, length, source)`:** 0 if it matches,
    `-BMDS_EACCES` if not, `-BMDS_ENOENT` with no password set. A failure delays that source's
    (the caller's text: Dropbear passes the peer's address) next checks: 1 s after the third
    failure, doubling to 60 s, forgotten after a success or 10 minutes; 8 sources are tracked,
    the oldest giving way. Failures are logged with the source.
  - Dropbear's patch: `svr_auth_password` calls it instead of `crypt()` on `pw_passwd`.
- **The server as a kernel service** (`SshService`, `bmds_net`):
  - **Settings** (§6's table, revised before shipping): `net/ssh/server` (bool, `false`),
    `net/ssh/port` (int, 22), `net/ssh/password-login` (bool, `true`; refused anyway without a
    password), `net/ssh/host-keys` (path, `/boot/ssh`: a directory), and
    `net/ssh/authorized-keys-dir` (string, a template, `${HOME}/.ssh`: the directory holding
    OpenSSH's `authorized_keys`, which is how Dropbear takes it, `-D`). §6's
    `net/ssh/key-login` is dropped (Dropbear can't turn keys off) and `net/ssh/authorized-keys`
    becomes the directory key.
  - **While `net/ssh/server` is true**, a kernel thread (PSRAM stack; none while off) runs it:
    `dropbear` is looked up on `shell/path`, the host key made if missing
    (`dropbearkey -t ed25519 -f DIR/dropbear_ed25519_host_key`, the directory made first;
    ECDSA P-256 joins in step 5a if the engine makes it worth it), then `dropbear -F -E -p PORT
    -r KEY -D DIR [-s] -P /dev/null` started, its stdout and stderr to the kernel log
    (`LogStream`: lines, prefixed `ssh:`). A change of any `net/ssh/` key stops it (`SIGTERM`)
    and starts it again with the new arguments; `false` stops it. If it ends on its own it is
    started again after 5 s, doubling to 5 minutes (a missing program, a card not in yet).
  - **The launcher's additions** (kernel API, not ABI): `ProgramLauncher::signal(pid, sig)`,
    and `Request::ended`, a semaphore `start()` releases when a program nobody waits for ends,
    so the thread sleeps on one semaphore for both "settings changed" and "it ended".
  - **mDNS** advertises `_ssh._tcp` on the port while it runs (§22's one service slot).
- **Our `sftp-server`** (§9.2): an SDK program in this repository (`programs/sftp-server`,
  packaged as `dist/sftp-server`), SFTP version 3 as OpenSSH's `sftp`, `scp` (9.0 and later),
  `sshfs` and file managers speak it:
  - `INIT`, `OPEN`, `CLOSE`, `READ`, `WRITE`, `STAT`, `LSTAT`, `FSTAT`, `SETSTAT`, `FSETSTAT`
    (size and times; modes and owners are FAT's, ignored), `OPENDIR`, `READDIR`, `REMOVE`,
    `MKDIR`, `RMDIR`, `REALPATH`, `RENAME`, `READLINK`, `SYMLINK` (BMDS's emulated links); the
    extensions `posix-rename@openssh.com`, `fsync@openssh.com`, `limits@openssh.com` (so
    clients size their reads) and `statvfs@openssh.com` (`df` in `sshfs`), on musl's POSIX
    calls.
  - The protocol is a portable core (`sftp.c`: a packet in, replies out, the file calls through
    a table) and a loop (`main.c`), so the core is fuzzed on the host with fake file calls
    (`tests/fuzz/sftp.cpp`), as §12 asks of what parses a peer's input.
  - Up to 32 handles; reads and writes up to 64 KB (what `limits` reports).
- **Order of work:**
  1. Kernel: `/dev/ptmx`, `/dev/pts`, `/dev/tty`, the controlling terminal, slot 110 and
     `linux.c`'s `ioctl`s; `/etc/passwd` and `group`; host tests (`pty`, VFS) and a `progtest`
     case (`openpty`, a child on the slave, `TIOCSWINSZ`).
  2. SHA-512 and its crypt (host vectors), `secret`, `system/password`, `passwd`, slot 111; the
     check's time measured on the board and the rounds chosen.
  3. `dist/dropbear` (recipe, options, patches); `dbclient` from the board to this PC first (no
     server side needed), then the server run by hand.
  4. `LogStream`, the launcher's additions, `SshService`, mDNS's service.
  5. `sftp-server` with its fuzz target.
- **Done when** (§12): from this PC, `ssh` with a key and with a password, `scp` (SFTP, and `-O`
  for Dropbear's own), `sftp`, `sshfs`, and a file manager's `sftp://`; from the board, `ssh`
  and `scp` to this PC. Measured: login time, and `scp` both ways (step 5a's baseline).

### 24a. Revised (the user's, 2026-10-11)

Asked whether SSH should be built in and whether the Dropbear patches belonged in BMDS instead:

- **SSH is in `/rom`:** `dropbearmulti` with its links, `sftp-server` and Dropbear's licence under
  `/rom/bin` and `/rom/share/licenses`, so the server and client work with no card. The firmware's
  build runs `tools/bmds-dist --out build/rom/dist --nested build dropbear sftp-server` (nested:
  no `build` wrapper, whose project lock the outer build holds, and one job at a time) and
  `mkrom.py` takes the result. They stay SDK programs, run by the loader as from the card; not
  linked into the firmware (whose `ota_1` is 2 MB). `shell/path` defaults to
  `/bin:/rom/bin:/sd0/p0/bin`: the ROM's before the card's.
- **Patch 0004 (the environment after `vfork`) is BMDS's now:** `linux.c` keeps the parent's
  `environ` with the rest of what a `vfork` child gets a copy of, and puts it back when the parent
  goes on, so every program's child may `clearenv()` and `setenv()` before its exec.
- **Passwords as on Linux:** `/etc/shadow` (built from `system/password` when opened: `root:HASH`,
  or `root:!` without one) and musl's `crypt()`. Patch 0002 and slot 111 (`password_check`) are
  gone, with the kernel's per-address delays: every program is root and `/boot/sysconf` is
  readable anyway, so hiding the hash bought little; Dropbear's own delay (a quarter second or
  more), its 10 tries a connection and its cap on connections not yet in remain. `fs_info` takes
  slot 111. The `secret` type stays: `sysconf` and listings still show `(set)`.
- **Left as patches:** 0001 (the accept loop's `fork()`: nothing without an MMU can give it; worth
  offering upstream for no-MMU systems) and 0002, formerly 0003 (Dropbear's fixed `PATH`; the
  alternatives put fixed paths in).

## 25. As built: step 5, SSH and SFTP (2026-10-11)

As §24 and §24a designed; what changed on the way, and what was found:

- **Pseudo-terminals for programs** (`bmds_console`, `bmds_fs`, `bmds_loader`):
  - `Pty::openCounted()`: a pair whose ends count their users (`Stream::handleOpened()`, new, beside
    `handleClosed()`; `Console::addUser()`/`removeUser()` for processes that have it as their
    terminal). The emulator end's writes take what fits (a write that waited for all of it
    deadlocked against a shell waiting for its output to be read).
  - `/dev/ptmx`, `/dev/pts/N` (`Vfs::DeviceDirectory`: numbered entries another component lists,
    with their identity, so `stat` and `fstat` agree), `/dev/tty`.
  - `Process::setConsole()`: the console is an atomic pointer now; the old one's line editor is
    dropped with it.
  - Slot 110, `term_ctl`: `PTY_NUMBER`, `PTY_UNLOCK`, `SET_SIZE`, `CONTROL`, `RELEASE`, and
    `PTS_NUMBER` (a program end's N, for `ttyname()`).
- **POSIX gaps found by Dropbear, fixed in BMDS:**
  - termios on a terminal that isn't the caller's own: Dropbear's `ttyname()` of a fresh pty failed
    (musl's `isatty()` is `TIOCGWINSZ`). Its modes are the console's; the line input's are
    reported as a new terminal's.
  - `ttyname()`: `linux.c` answers `readlink("/proc/self/fd/N")` for terminals (`/dev/pts/N`,
    `/dev/console`), the way musl reads it.
  - `setgroups` succeeds (root's `initgroups()`): Dropbear's single-user mode insists that
    `getgroups` fail with `ENOSYS`, which it rightly doesn't, so Dropbear stays multi-user.
  - The environment after `vfork` (§24a).
  - `statfs` (slot 111, `fs_info`, over `Vfs::statfs()`): `df` on an sshfs mount shows the card.
  - A shell with no terminal runs (`NullConsole`): `ssh host command` has none; it refused.
  - A new shell, interactive or not, takes `shell/path` when it inherits no `PATH` (`sh -c` over
    SSH found nothing).
- **Passwords:** SHA-512 and SHA-512 crypt in `bmds_core` (OpenSSL's and the specification's
  vectors); 5000 rounds took 265 ms on the board, so new hashes get 2000 (about 0.1 s);
  `secret`, `system/password`, `passwd` (`-d` removes it), `/etc/passwd`, `/etc/group`,
  `/etc/shadow`.
- **`SshService`** (`bmds_net`): as §24, with `ProgramLauncher::sendSignal()` and
  `Request::ended`, and `LogStream`. Dropbear without syslog has no `-E` (it logs to stderr
  always). Since `logging.md` it is built with syslog: its lines reach the log through
  `/dev/log`, tagged `dropbear[PID]`; `LogStream` keeps what else it writes. `Boot`'s most stages went from 24 to 32: the new stage was the 25th, and the assert
  stopped every boot.
- **Dropbear 2025.88** (`dist/dropbear`): `localoptions.h` and two patches (§24a). `scp` runs
  `dbclient` by name (`DROPBEAR_PATH_SSH_PROGRAM`).
- **`sftp-server`** (`programs/sftp-server`, `dist/sftp-server`): version 3 and
  `posix-rename`, `fsync`, `limits`, `expand-path`, `statvfs` (OpenSSH's). Fuzzed with made-up
  file calls (`sftp_fuzz`: 2.1 million inputs in 5 minutes, no finding). libFuzzer stalled on this machine printing new functions (its
  symbolizer); `-print_funcs=0` runs it at about 28,000 inputs a second.
- **`bmds-dist`:** sends a User-Agent (Dropbear's site refuses Python's), and has `--out` and
  `--nested` for the firmware's build.
- **On the board (2026-10-11),** this PC on Wi-Fi, OpenSSH 9.6 on it:
  - From the PC: `ssh` with a key (1.4 s for a command, connection included) and a password
    (1.2 s), a wrong password refused; an interactive session (prompt, commands, `exit`); `scp`
    (SFTP) 4 MB up 4.3 s and down 3.3 s, `scp -O` up 6.5 s and down 3.0 s; `sftp` (`ls -l`,
    `put`, `get`, `rename`, `rm`, `mkdir`, `rmdir`); `sshfs` (listing, writing, a 1 MB copy
    intact, renaming, removing, `df`); `bmds0.local` and `_ssh._tcp` from avahi.
  - From the board: `ssh` to the PC (3.0 s for `uname -n`), `scp` 4 MB down 6.2 s and up 5.0 s.
  - The service: started by `net/ssh/server`, again on a new port, again 5 s after a `kill -9`,
    stopped by `false` (its thread gone); the host key made the first time.
  - Memory: the server process 17 KB of heap, a session 42 KB, besides each one's copy of the
    program (739 KB); the service's thread used 2.7 KB of its 6 KB stack.
  - These times are software crypto over this PC's Wi-Fi: step 5a's baseline.
- **Size:** firmware 1 729 504 bytes (+16 KB); the ROM 2.0 MB of its 9.4 MB (Dropbear 739 KB,
  `sftp-server` 211 KB).
- **`ssh-copy-id`** (found by the user): it sends `exec sh -c '...'`, and the shell had no `exec`
  (nor `umask`, which the script uses). `exec CMD` runs the command and the shell then leaves with
  its status (the shell is kernel code: it can't be replaced in place); `exec` alone keeps its
  redirections; `umask` keeps and shows a mask (FAT has no modes). In the corpus, agreeing with
  bash. `ssh-copy-id` then added two keys in turn from this PC, one a line, and they logged in.
- **Not yet:** a file manager's `sftp://` (GNOME Files) was not tried from here (it needs the
  desktop's keyring or agent); `fstatfs`; `AF_UNIX` for agent forwarding.

## 26. Step 5a's design: crypto (2026-10-11)

Measured first, as §9.3 asked: an SSH login from this PC (OpenSSH 9.6, Wi-Fi), the server traced
(Dropbear built with `DEBUG_TRACE`), and each algorithm timed on the board, in software (a program
linked with Dropbear's own objects, `-Os` as built) and on the engines (a temporary shell command).

| Work | Software | Engine |
|---|---|---|
| X25519 (Dropbear's `curve25519.c`, TweetNaCl's) | 75 ms | none |
| Ed25519 sign / verify (the same) | 126 / 252 ms | none |
| X25519; Ed25519 sign / verify (Monocypher 4.0.2, `-O2`) | 6.7 ms; 3.4 / 9.7 ms | none |
| P-256 point multiplication | 105 ms (libtomcrypt) | 4.5 ms |
| RSA-2048: private exponent / public (65537) | (not measured) | 179 / 16 ms |
| ChaCha20 / Poly1305, 16 KB | 8.4 / 11.7 MB/s | none |
| AES-128-CTR, 64 B / 16 KB | 2.6 / 2.6 MB/s | registers: 15.5 / 15.7 MB/s; ESP-IDF's DMA: 4.1 / 28 MB/s |
| HMAC-SHA-256, 64 B / 16 KB | 0.6 / 3.7 MB/s | SHA-256 registers: 25 / 43 MB/s; ESP-IDF's DMA: 2.3 / 41 MB/s |
| SHA-512, 64 B / 16 KB | 1.4 / 3.0 MB/s | registers: 44 / 44 MB/s |
| AES-128-GCM, 16 KB | (Dropbear's is off) | ESP-IDF's: 4.1 MB/s |

- **A login took 1.03 s** (curve25519, an Ed25519 user key, `ssh bmds0 true`). The server's
  share: X25519 145 ms, the host key's signature 124 ms, the user's signature checked 242 ms,
  and **0.2 s idle**: lwIP's delayed ACK waits for its fast timer (every 250 ms), and OpenSSH
  without a terminal keeps Nagle on, so its second small segment waited for that ACK. Not crypto,
  but the biggest single cost; fixed first (§27).
- **25519 needs no engine, just a better implementation.** TweetNaCl is written for size;
  Monocypher (BSD-2 or CC0, audited, 2 files) is 11 to 37 times faster here and agrees bit for
  bit. The engines' P-256 (4.5 ms) is no faster than Monocypher's X25519 (6.7 ms), and clients
  prefer 25519 and mostly hold Ed25519 keys, so the ECC engine isn't worth taking clients there.
- **Bulk: AES-CTR with HMAC-SHA-256 on the engines** comes to about 11.5 MB/s of one core,
  against ChaCha20-Poly1305's 4.9 MB/s in software (the wire is 12.5 MB/s).
- **Register mode, not DMA:** the CPU writes each block to the engine and reads it back. It is
  as fast as ESP-IDF's DMA for SHA at every size and for AES up to about 1 KB, and it needs no
  descriptors, no cache maintenance and no allocation. ESP-IDF's DMA path allocates descriptors
  and alignment buffers on each call (`esp_aes_dma_core.c`), which a hot path mustn't, and its GCM
  is slow (4 MB/s). DMA can come later for AES if transfers on the wire want it.
- **MPI and RSA:** nothing uses RSA (the host key is Ed25519; RSA user keys need only the public
  operation, cheap in software). Left.

**The design:**

1. **Delayed ACKs within 40 ms** (Linux's least): the network task starts a one-shot lwIP timeout
   when a connection owes one, and sends what the fast timer would have sent (no new timer
   running otherwise).
2. **Monocypher for 25519** (`thirdparty/monocypher`): Dropbear's `curve25519.c` becomes four
   wrappers over it (patch 0003), compiled `-O2`. Logins, the client and `dropbearkey` all gain.
3. **`Crypto` (`bmds_crypto`, kernel):** the AES and SHA engines in register mode on ESP-IDF's
   HAL (`aes_hal`, `sha_hal`) and `esp_security`'s clocks and locks, not mbedtls. Stateless: every
   call carries its key and its state, so the kernel keeps no secrets and no handles:
   - AES-128 and AES-256 (the engine has no AES-192): ECB, CBC and CTR (a 128-bit big-endian
     counter, as SSH's and NIST's), lengths a multiple of 16.
   - SHA-1, SHA-256 and SHA-512 compression of whole blocks from a chaining value the caller
     gives and gets back (the engine's resume), so SHA-224 and SHA-384 come with them (their
     initial values are the caller's) and padding stays the caller's.
   - One lock for both engines (they share the crypto DMA in ESP-IDF's locks); a call holds it for
     its blocks only (a 32 KB SSH packet: about 2 ms).
4. **Slot 112, `crypto(op)`**, with `bmds_crypto_op_t` (`size` first): the algorithm, key, state,
   input, output, length. `-BMDS_EINVAL` for a wrong length or key size, `-BMDS_ENOTSUP` for an
   algorithm this kernel doesn't have. A `progtest crypto` case checks NIST's and FIPS 180's
   vectors through the SDK.
5. **Dropbear's libtomcrypt on the slot** (patch 0004): SHA-1, SHA-256 and SHA-512 compress
   their whole blocks in one call; AES's descriptor gains `accel_ctr_encrypt` (and keeps the raw
   key for it). Software stays the fallback on a kernel without the slot.
6. **Then measured** (as §12 asks): login time, and transfers both ways with ChaCha20-Poly1305
   against AES-CTR with HMAC-SHA-256, speed and CPU. The server's offer follows: OpenSSH takes
   the first of its own list that the server offers, and it lists ChaCha20 first, so the engines
   only serve OpenSSH if the server leaves ChaCha20 out (`dropbear -c`). The board's own
   client (`dbclient`, `scp`) chooses for itself.

- **ABI:** slot 112 and `bmds_crypto_op_t` with its algorithm numbers.
- **Not doing:** GCM on the engine (ESP-IDF's is slow; CTR and HMAC are what Dropbear offers
  besides ChaCha20), P-256 and RSA (above), the HMAC engine and the digital signature peripheral
  (keys in eFuses: a later step, if a stolen card shouldn't mean a stolen host key).

## 27. As built: step 5a, crypto (2026-10-11)

As §26 designed; measured from this PC (Wi-Fi, OpenSSH 9.6):

- **Delayed ACKs** (`netstack.cpp`): after each round of the network task, a connection owing an
  ACK starts a one-shot lwIP timeout of 40 ms (`scheduleDelayedAcks`), which sends what lwIP's fast
  timer would (`sendDelayedAcks`); an answer sent meanwhile carries the ACK. The pause after the
  key exchange went from 0.21 s to 0.046 s, and a login (curve25519) from 1.03 to 0.80 s.
- **Monocypher** 4.0.2 (`thirdparty/monocypher`, built by `dist/dropbear/build.sh` at `-O2`;
  patch 0003): Dropbear's four 25519 functions wrap it. A login (curve25519) 0.80 s to 0.20 s;
  the server's crypto in it is now about 27 ms (two X25519, a signature, a check). OpenSSH's
  default, sntrup761x25519, takes 0.55-0.8 s, of which the PC making its own sntrup761 key is
  0.2-0.4 s. Dropbear grew 18.7 KB.
- **`Crypto`** (`bmds_crypto`): register mode on ESP-IDF's `aes_ll`/`sha_ll`, ESP-IDF's lock, the
  clock left on after first use (`docs/platforms/esp32p4.md` §4b: the lock and the clock were
  most of a small call's cost). **Slot 112, `crypto`**, `bmds_crypto_op_t`; `progtest crypto`
  passes FIPS 197's AES-128 and AES-256, SP 800-38A's CBC and CTR (two calls chained, in place)
  and FIPS 180's SHA-1, SHA-256 and SHA-512 (from an unaligned block) vectors, and the errors.
  Through the call, 16 KB: AES-CTR 12.4-12.9 MB/s, SHA-256 34-35, SHA-512 38-39; 64 bytes: 3.4-4.0
  and 3.8-4.7 (about 13 µs a call).
- **Patch 0004:** libtomcrypt's SHA-1, SHA-256 and SHA-512 send each `process` call's whole
  blocks in one `crypto` call (`HASH_PROCESS_BLOCKS`); AES keeps its raw key and gains
  `accel_ctr_encrypt`; software stays the fallback. 8 MB went up intact (checked by `sha256sum`
  on the board) with `aes128-ctr`, `aes256-ctr` and ChaCha20-Poly1305, and 4 MB down with
  `aes128-ctr`.
- **Transfers,** 8 MB to `/dev/null` and from `/dev/zero`, the session's CPU from `top`:

  | Cipher | Before (software) up / down | Engines up / down |
  |---|---|---|
  | ChaCha20-Poly1305 | 1.34 MB/s (43%) / 2.03 MB/s (87%) | 1.37 MB/s (44%) / 1.69-1.92 MB/s (86-89%) |
  | AES-128-CTR, HMAC-SHA-256 | 0.87 MB/s (66%) / 1.06 MB/s (92%) | 1.63 MB/s (35-37%) / 1.91-2.11 MB/s (76-83%) |

  Uploads are the Wi-Fi's speed; downloads are CPU-bound. Per MB/s, AES-CTR on the engines took
  21-22% of a core up and 36-43% down, ChaCha20 32% and 44-53%: a fifth to a third less.
- **The server's offer stays Dropbear's** (OpenSSH keeps choosing ChaCha20): besides the crypto,
  Dropbear's own work costs about 12% of a core per MB/s up and 25% down, so the engines gain
  about a tenth on downloads, not enough to drop the algorithm clients prefer. A PC that wants
  them says `Ciphers aes128-ctr` for the board in its `~/.ssh/config`. **The board's client**
  (`dbclient`, `ssh`, `scp`) lists `aes128-ctr` and `aes256-ctr` first (patch 0004), so it uses
  the engines; tried over loopback with a key made for the test.
- **Size:** the firmware 1 737 520 to 1 739 280 bytes; Dropbear 743 768 to 763 604.
- **Not yet:** a wired peer's numbers (the downloads' CPU suggests 2.3-2.8 MB/s at most from
  Dropbear as it is: its download path costs twice its upload's besides the crypto, worth a look:
  done in §28);
  DMA for AES; the HMAC, DS and key manager engines (a host key in eFuses). Found on the way: the
  shell's `head` reading `/dev/zero` (a line with no end) fails with `Out of memory` (fixed: it
  streams, and has `-c`).

## 28. SSH downloads and the TCP path (2026-10-11)

§27 left Dropbear's downloads costing twice its uploads besides the crypto. Measured with the
board's busy time (`stats`' `cpu.core*.busy_us` before and after: `top`'s one look and `tasks`'
`IDLE` totals read over a login misled by up to a second a run) and the new profiler (`prof`,
`tools/bmds-prof`: `sysmon.md` §9), the cost was mostly not Dropbear's:

- **Raw TCP was most of it:** BusyBox `nc` sending `/dev/zero` cost 250 ms of CPU per MB at
  2.7 MB/s, SSH's download 423. The profile: FreeRTOS's semaphore and mutex operations with
  interrupts masked (on `vPortExitCriticalMultiCore`, 40% of `nc`'s samples), the `eth0` task 72 µs
  a frame, copies.
- **The send buffer was the ceiling:** 8 segments (11.7 KB) a round trip over Wi-Fi is about
  2.3-2.9 MB/s, where both directions stopped; and every ACK woke the sender for a segment or two
  of room, moving the core lock between cores each time.
- **Changes:**
  - `TCP_WND` and `TCP_SND_BUF` 16 segments (23 KB; `MEMP_NUM_TCP_SEG` 128). Received data past
    the ring's share was already copied into the heap.
  - A TCP socket is writable, and its writers woken by an ACK, with half its send buffer free
    (Linux's `poll` rule), not at each ACK.
  - The Ethernet driver's sender takes back sent descriptors itself; a frame asks for the sent
    interrupt only with a stamp, with the ring half full, or every 8th (the counters of frames sent
    trail by fewer than 8 when traffic stops); the room semaphore is used only when the ring is full.
  - `Crypto::aes` writes the engine's registers itself (ESP-IDF's `aes_ll` copied each word
    through `memcpy`) and XORs whole words: AES-CTR through the call 12.4 to 15.5 MB/s.
- **After** (this PC on Wi-Fi, 16 MB, MB/s and the board's CPU-ms per MB):

  | | Before | After |
  |---|---|---|
  | TCP down / up (`nc`) | 2.20-2.67 (249-252) / 2.19-2.86 (225-232) | 3.36 (233) / 4.55 (211) |
  | SSH AES-128-CTR down / up | 2.16-2.46 (423-432) / 1.20-1.74 (326-343) | 3.15 (349) / 2.25 (305) |
  | SSH ChaCha20-Poly1305 down / up | 1.97-2.25 (459-509) / 1.02-1.52 (431-435) | 2.43 (439) / 1.85 (406) |

  Downloads now cost little more than uploads; what is left is the engines' register time (AES
  a sixth of the busy samples, SHA a tenth), the semaphores' masked time (a sixth), and copies.
- **Bigger pipes made it worse,** tried twice: 16 KB and 32 KB pipes (16 KB SSH packets) cost
  417-491 ms/MB against 4 KB's 349; they stay 4 KB.
- **Next, if wanted:** DMA for AES (the CPU free while it runs; ESP-IDF's DMA measured 28 MB/s),
  checksum offload (`lwip_standard_chksum`, 1.3% of the download's samples), zero-copy transmit.

## 29. Checksum offload, transmit in place, AES by DMA (2026-10-11)

§28's three leftovers, the user's ask. Measured as §28 was (16 MB over this PC's Wi-Fi, MB/s and
the board's CPU-ms per MB from `stats`).

- **Checksums: the MAC checks received ones; it can't insert them in full-size frames.**
  - Receiving: the MAC (type 2 offload, already on) checks IPv4 headers and TCP/UDP/ICMP payloads
    as frames pass and reports in RDES4 (header error, payload error, bypassed, payload type).
    Tested in the MAC's loopback (a temporary `net csumtest`, UDP to our own address at 100 to
    1400 bytes): good frames reported checked at every size; bad ones arrive with the error
    summary set, which the driver already dropped; checksum 0 (UDP's "none") passes. So
    `RxFrame::checksummed`, and the netif turns lwIP's TCP/UDP/ICMP checks on only for a frame the
    MAC didn't check (set before `netif->input`, put back after): a fragment or a bypassed frame is
    checked by lwIP, a reassembled datagram by its last fragment's flags, and frames looped back on
    `eth0` (never on the wire) not at all. `lo` neither makes nor checks them.
  - Sending: TDES0's CIC field (3: IP header and payload, pseudo-header too) needs the whole frame
    in the transmit FIFO, which is **about 256 bytes**, like the receive FIFO: with
    store-and-forward on, TCP segments up to 210 bytes of payload (264-byte frames) arrived with
    good checksums, 211 and up with bad ones (the PC's `InCsumErrors`), and the MAC reported no
    error. lwIP keeps making them (only ACK-sized frames would gain).
- **Transmitting in place, from internal RAM.** The EMAC can't send from PSRAM: read in place from
  lwIP's PSRAM heap, every frame over 256 bytes underflowed (TDES0 bit 1), aligned or not (copied
  to an aligned PSRAM buffer, most frames vanished); from internal RAM, even 2-byte aligned, none
  did. So (the user's call) **lwIP's heap is internal DMA memory**, 96 KB from boot (64 KB
  since 2026-10-12, `foundation.md` §9) (`CONFIG_BMDS_NET_HEAP_KB`; peaks seen: 28 KB for a bulk transfer either way, 52 KB over SSH and
  `progtest net` together; when full, senders wait and frames are dropped and retransmitted), out of
  reach of the display's PSRAM traffic too.
  - `EthernetDevice::transmitInPlace(pieces, flags, token, timeout)` and `setTxDone(fn, arg)`: a
    descriptor a piece (up to 4), the first handed over last; the caller's buffers held until the
    reclaim (in the sender or `eth0`) passes `token` to `fn`. A piece outside internal DMA memory,
    a stamp, or more pieces fall back to the copy, done at once.
  - The netif `pbuf_ref()`s the frame and frees it in the done call: at once when the core lock is
    held (a sender), else queued for the network task, which frees them before taking received
    frames. lwIP doesn't retransmit a segment the driver holds (`tcp_output_segment_busy`), so
    `eth0` reclaims before it takes received frames: an ACK finds every frame sent before it
    released. Frames held across a link down go out when it comes back.
  - TCP down 3.36 (233) to 3.8 (220), up 4.55 (211) to 4.25 (212): the Wi-Fi's noise is as large.
- **AES by DMA** (`bmds_crypto`), for calls of 1 KB or more with word-aligned buffers the DMA
  reaches (internal or PSRAM); register mode below.
  - Our own AXI-GDMA channel pair connected to the engine at the first DMA call and kept (ESP-IDF's
    mbedtls DMA, unused by BMDS, can't connect after), with 7 + 9 descriptors and two 128-byte
    bounce lines in one internal block. A run is up to 7 × 3968 bytes; the output's whole lines are
    written in place, its partial lines at the ends through the bounce lines; the engine in DMA
    block mode (ECB, CBC, CTR with `AES_INC_SEL` 1, the 128-bit increment); the task sleeps until
    the receive channel's end-of-frame interrupt. The IV or counter is carried from run to run in
    software, not read back.
  - A cache operation costs 4-5 µs however short: merging them (descriptors with bounce lines, in
    place data once) and resetting the channels only after a timeout took a 1 KB call from 11.9 to
    13.6 MB/s (register mode's speed); 4 KB 25.6, 16 KB 32.8 MB/s (register mode 15.5).
  - `progtest crypto` checks long calls against the same bytes in one-block calls (register mode):
    each mode, 1 KB to 60000 bytes (past a run), misaligned, in place, counters carrying past 32 and
    64 bits.
  - SSH AES-128-CTR down 3.15-3.26 MB/s at 297 ms/MB (334 before it, 349 in §28), up 2.23-2.25 at
    268-275 (308, 305). ChaCha20-Poly1305, which uses no engine, is unchanged (407 / 418).
- **On the wire** (sartre, wired at 10 Gbit/s; this PC is on Wi-Fi, which capped the numbers
  above): the driver saturates the link. A raw UDP blast through `transmit()` (a temporary command)
  sent 8130 full frames a second (line rate 8127) and 21560 554-byte frames (99.7%), all received
  intact; minimum frames reached 47000 a second of 96000 (the per-frame cost, about 21 µs). A UDP
  flood into the board was taken at 7300-8150 frames a second, losing about 0.01% (missed by the
  MAC with the ring momentarily empty). Raw TCP over `nc`: 4.83 MB/s out, 5.93 in, CPU-bound
  (about 1.3 s busy a second: `dd`, the pipe, `nc`, lwIP), the ring never more than 2 deep.
- **Deferred (the user: premature):** `eth0` costs about 55 µs a received frame, three quarters of
  it cache operations (about five `esp_cache_msync` a frame, each with interrupts masked) and
  semaphores (a wake a frame). If small-frame rates come to matter: the receive interrupt watchdog
  (RIWT) to take several frames a wake, descriptors in uncached memory if the chip has an uncached
  view of internal RAM, one network-task wake a batch.

## 30. Step 6's design: real time, and a guarantee for it (2026-10-11)

§5 and §7 stand (the classifier, flows, fast-path sockets, `SO_TIMESTAMPING`, `netlat`); this adds
the user's requirement: **a real-time flow can always send and receive its packets, however much
TCP has taken.** Today it can't: frames queued for lwIP can hold every receive buffer, so the ring
empties and the MAC drops everything (the flood's misses, §29), and bulk frames can fill all 16
transmit descriptors.

- **What "always" means:** with the link up and each flow within its own budget, no amount of
  other traffic can take a real-time frame's descriptor, buffer, memory or CPU. The limit is
  `eth0`'s own speed: at full-size frames it keeps up with the line (§29); a flood of small frames
  (up to 148000 a second) outruns its per-frame cost, and then the MAC misses frames of every kind.
  The deferred optimizations of §29 are the answer if that matters.
- **The partitions follow real-time use** (the user: throughput may drop while real-time traffic
  runs). With no real-time flow, bulk has the whole transmit ring and every spare receive buffer,
  as now. While any flow is registered, bulk is held to *k* transmit descriptors and 8 receive
  buffers. The driver has the mechanism (`setBulkTxLimit()`, `setRxReserve()`); the flows layer
  sets it as flows come and go.
- **What AES67 allows** (from memory of AES67-2018; to check against the standard): no sender
  jitter figure as such. A receiver keeps at least 3 packet times of buffer (20, or 20 ms, is
  recommended) and plays at a fixed link offset, so the sender's lateness plus the network's delay
  variation must fit in what the far end's link offset leaves. 1 ms packets are the required
  packet time; 125, 250 and 333 µs and 4 ms are options. At 1 ms, a few hundred µs is comfortable;
  at 125 µs even one full bulk frame ahead (123 µs at 100 Mbit/s, the wire's floor) is a packet
  time.
- **Receive: the buffers partitioned.**
  - The pool grows from 24 to 32 buffers (+12 KB internal): 16 keep the ring full; the other 16
    are bulk's with no real-time flow, else 8 bulk's and 8 a real-time reserve.
  - The classifier (§5.1) runs in `eth0` before anything is queued. A real-time frame goes to its
    flow (a per-flow cap, so one flow can't take the reserve). A bulk frame goes to lwIP only while
    bulk holds fewer than its 8 (frames queued for the network task plus those lwIP holds; lwIP's
    copy threshold, `MaxHeld`, stays within it); past that `eth0` drops it at once (counted), so its
    buffer goes straight back and the ring always refills. TCP recovers from the drop.
  - Real-time flows never touch lwIP's heap or the network task: a full heap costs them nothing.
  - Multicast: flows' groups get the MAC's 8 exact filters first; past that "pass all multicast"
    (more frames for `eth0` to drop, nothing lost).
- **Transmit: the descriptors partitioned.**
  - Bulk (lwIP's frames, and any without `TxRealTime`) may hold at most *k* descriptors: all 16 with
    no real-time flow, 2 with flows of 1 ms packets or longer (about 250 µs of full frames ahead
    of a real-time one, plus the frame on the wire), 1 with a shorter packet time. The rest are
    real-time's. lwIP's sender waits for bulk room (it holds the core lock, which only bulk paths
    need); a bulk frame asks for the sent interrupt once bulk is at half its *k*, so the wait is
    woken (with *k* at 1 or 2, every bulk frame: the throughput cost the user accepted).
  - **The jitter budget:** `netlat`'s send jitter under load at 1 ms packets under 300 µs: the bulk
    ahead, the frame on the wire, the sender's wake (a timer interrupt waking a high-priority task,
    tens of µs).
  - A real-time frame is copied into its descriptor's internal buffer (or sent in place from
    internal memory): never lwIP's heap. The transmit lock is a mutex with priority inheritance,
    and bulk holds it for one copy and a cache operation at most.
  - Unicast destinations: the flow's MAC comes from lwIP's ARP table when it connects, and the flow
    refreshes it itself while in use, with a frame of its own (`Arp::writeFrame`) on the real-time
    path, so a full lwIP heap can't stop it. Multicast's MAC comes from the group.
- **CPU:** `eth0` runs at priority 21, above audio; it wakes a flow's consumer directly. The network
  task (6) and the TCP senders can't delay a real-time frame.
- **The pieces, in order:**
  1. The driver's partitions and counters (bulk dropped, real-time sent and received, the deepest
     real-time wait).
  2. The classifier and `NetFlow` (kernel API): match on EtherType (PTP's 0x88F7) or UDP destination
     port and address (a group); a ring of frames per flow, its wake, its send; hardware stamps
     both ways.
  3. Fast-path sockets (`SO_BMDS_FASTPATH`), `SO_PRIORITY`/DSCP for the queue, `SO_TIMESTAMPING`
     (receive stamps in `recvmsg`'s control messages, send stamps through `MSG_ERRQUEUE`, as
     Linux and linuxptp expect). These are ABI: frozen once shipped.
  4. `netlat` and a Python reflector (§11).
- **Done when** (the test of the guarantee): `netlat` sends AES67-sized packets every 1 ms both
  ways between the board and sartre, while sartre pulls TCP from the board, pushes TCP into it and
  floods it with full-size UDP at line rate: **no real-time packet lost**, the bulk drops counted,
  the histograms (send jitter, delivery latency, round trip) recorded here with and without load.

### 30a. As built: piece 1, the driver's partitions (2026-10-11)

- **The mechanism:** `EthernetDevice::setBulkTxLimit()` and `setRxReserve()`, `TxRealTime`, 32
  receive buffers; counters for real-time sent and waited and bulk frames dropped (`net`: "real
  time: …").
  Nothing sets them yet: the flows layer (piece 2) will. Tested with temporary commands (a bulk
  blast and a real-time sender of 342-byte frames every 1 ms, stamped), against sartre.
- **Two bugs the tests found:**
  - A second sender waiting for transmit room lost its wake (one `_txWaiting` flag for any number
    of waiters): it slept to its timeout, and the blast fell from 8130 frames a second to 2300. Now
    a count, and a woken sender passes the wake on.
  - The queue of in-place frames to free (§29) overflowed under a flood and corrupted memory (a
    store fault in `NetInterface::txDone`): a reclaimed descriptor carries the next frame, so the
    descriptors don't bound it. Now the frames out in place are counted, and copied past the
    queue's size; the network task frees them as it goes.
- **The transmit lock was the real-time sender's worst wait:** under a UDP flood (the network task
  sending an ICMP reply a frame, `eth0` reclaiming), the FreeRTOS mutex was held at most 127 µs at
  a time, yet a real-time sender waited up to 2.6 ms for it: a released mutex goes to whoever takes
  it first, and the bulk paths on the other core kept taking it. Now:
  - the CPU reaches the descriptors through internal RAM's uncached view (+0x40000000), so no
    descriptor needs a cache operation (several a frame, each 4-5 µs with interrupts masked);
  - copy buffers are a pool of their own, so a frame is copied and written back before the lock;
  - the lock is a spinlock held only to claim and hand over descriptors and to reclaim; the done
    calls, stamp wakes and semaphore releases come after it.

  The real-time sender's wait to get into the ring fell to 313 µs at most, which is `eth0`
  preempting it (the test ran at 20, `eth0` at 21). Raw TCP also gained: 4.83 to 5.43 MB/s out
  (281 to 259 ms/MB), 5.93 to 6.23 in (211 to 195).
- **Measured** (real-time frames every 1 ms; sartre wired):

  | Load | *k* | Real-time lost | Wait, mean / worst | Bulk |
  |---|---|---|---|---|
  | Bulk blast (full frames) | 16 | almost all | - | 8052/s |
  | Bulk blast | 2 | 0 of 5000 | 139 µs / 300 µs (one 1.2 ms) | 7929/s |
  | Bulk blast | 1 | 0 of 5000 | 75 µs / under 700 µs | 7203/s |
  | UDP flood in, reserve 8 | 2 | 0 of 30003 | 20 µs / 313 µs | - |

  The one long wait at *k* 2 is the first frame after the limit drops: the 16 bulk frames already
  in the ring drain first (under 2 ms, once, when the first flow registers).
- **Not yet explained** (then; explained in §30b): under a line-rate UDP flood the MAC missed about
  3 frames in 65000.
- **The stamps:** a stamp is kept in its descriptor's slot until the slot is reused, 16 frames
  later; under bulk churn a slow reader loses it (`waitTxStamp()` returns `NotExist`). PTP's and
  `netlat`'s stamps must be delivered to the flow when reclaimed (piece 2), not looked up.

### 30b. The flood's missed frames, and a receive path that wedged (2026-10-12)

The user's ask: get to the bottom of §30a's missed frames. A timeline of the driver's events (the
interrupt, the task's wake, each frame taken and its length, type and source, each refill), kept in
a ring and printed when the MAC reported a miss, showed it:

- **The frames lost were in bursts of small broadcasts.** About 20-30 ARP frames (64 bytes, from six
  hosts on the LAN) arrived back to back, one every 6.7 µs (line rate for minimum frames), between
  full flood frames 123 µs apart. The flood fills the switch's queue to the board's port, so the
  LAN's ordinary broadcasts collect behind it and leave together. Any traffic that congests the
  port (TCP to the board) clumps them the same way.
- **The task couldn't keep up with a burst:** each frame raised an interrupt (about 7 µs apart, on
  the task's core), so the task took a frame every 20-27 µs during the burst and every 11-13 after
  it; the 16-descriptor ring ran out after about 20 frames. Fixed:
  - **Receive interrupts masked while the task drains the ring** (as Linux's NAPI): the handler
    masks them, the task unmasks them once it has taken every frame and refilled; the status bits
    latch meanwhile, so nothing is missed.
  - **The network task woken by the first frame queued,** not by each.
  - **A buffer's cache cleaned when it is released,** by the receiver's task (only a receiver can
    have written it), not by `eth0` at refill. `eth0`'s own drops skip it.

  Per small frame in a burst, about 10 µs: the 16 descriptors absorb bursts of 40 or so.
- **The receive path could wedge for good.** Twice the MAC stopped receiving altogether: its debug
  register read 0x340 (receive FIFO full, its read controller "reading frame status"), the DMA ran
  waiting with every descriptor its own, and frames counted only as FIFO overflow. A 5 ms stall of
  `eth0` during a flood (a temporary command) reproduced it; stopping and restarting the MAC's
  receiver and the receive DMA didn't clear it. The cause was flushing frames when the ring has no
  descriptor (operation mode DFF clear, as ESP-IDF sets it): with it off, a frame waits in the FIFO
  for a descriptor and later ones overflow (counted). Twenty stalls of 3-50 ms in a 50 s flood, and
  stalls up to 1 s, left it running. (The first wedge, after a reboot during a flood, and the
  second, while the timeline was printed from `eth0`, were both long stalls under a flood.)
- **After:** 4 to 8 line-rate floods (260000-520000 frames) with no frame lost: no missed, no
  buffer-unavailable, no overflow. Raw TCP unchanged (4.95 MB/s out, 6.06 in).

### 30c. Hardening: the scenarios, a watchdog, a stress tool, the cores (2026-10-12)

The user asked for all of §30b's follow-ups:

- **`tools/bmds-netstress`** (Python, from any machine that reaches the board and a wired peer over
  ssh; the peer needs `python3` and `nc`): `tcp` (raw TCP each way, MB/s and the board's CPU-ms per
  MB), `flood` (full-size UDP at line rate into the board), `link` (the PHY restarted during a
  flood), `reboot` (the board rebooted during a flood), each a flood after. A test fails if a loss
  counter moved (`stats`' `net.eth0.rx_errors`, `tx_errors`, `rx_missed`, `rx_overflow`,
  `tx_realtime_waited`, `rx_restarts`) or the MAC's receive FIFO is stuck full (`net regs`). It
  takes the board's address from `net` and the peer's from its route.
- **The scenarios the wedge first appeared in** pass: rebooted during a flood and the PHY restarted
  during one, receive came back each time and a flood after lost nothing.
- **A watchdog anyway:** `eth0` checks the MAC's debug register on each wake (an overflow interrupt
  or the 1 s link check wakes it); its receive FIFO full for 100 ms with no frame taken is a wedge,
  and `recoverRx()` resets the MAC: senders see the link down meanwhile, queued frames are dropped
  (counted as errors), the rings are rebuilt, the address filters reapplied, the 1588 clock put
  back where it was with its frequency (its addend kept), and receivers aren't told (the link
  didn't change). `rx_restarts` counts it (a warning in `stats`; `net` shows it). Tested by
  provoking the wedge (flushing turned back on in a test build, a 5 ms stall in a flood): detected,
  reset, and pings, mDNS (the multicast filters) and TCP worked after.
- **The stats** gain (appended) `rx_no_buffer`, `rx_missed`, `rx_overflow`, `rx_bulk_dropped`,
  `rx_checksummed`, `tx_in_place`, `tx_realtime`, `tx_realtime_waited` and `rx_restarts`.
  `rx_dropped` no longer counts `rx_no_buffer`: with flushing off a dry ring loses nothing (the
  frame waits in the FIFO; those that then don't fit count as `rx_overflow`), and the stat is
  "frames lost".
- **The cores:** a priority-20 real-time sender, sending every 1 ms under a line-rate flood, waited
  to get into the ring at most:

  | Sender on | `eth0` unpinned | `eth0` on core 0 |
  |---|---|---|
  | core 0 (`eth0`'s interrupt's) | 235-252 µs | 349 µs |
  | either | 167-170 µs | - |
  | core 1 | 25-36 µs | 26-44 µs |

  So `eth0` is pinned to its interrupt's core (the boot core, 0), and **real-time senders (the
  AES67 sender, PTP's) run on core 1**, the decision for step 6 piece 2 and the AES67 step. The
  audio engine (priority 20, unpinned) is `audio.md`'s to decide; core 1 suits it the same way.
  Raw TCP after: 4.7-4.9 MB/s out, 5.9-6.1 in.


### 30d. Piece 2's design: the classifier and `NetFlow` (2026-10-12)

- **`NetFlow`** (`bmds/netflow.h`, kernel API, not ABI): a real-time flow on one Ethernet device,
  an object its owner keeps (PTP's service, AES67's streams, piece 3's sockets): nothing is
  allocated. `open(device, config)` registers it, `close()` takes it out.
  - **What it receives:** frames of an EtherType (PTP's 0x88F7, with a group to receive, such as
    01:1B:19:00:00:00), or UDP datagrams to a port at an address: a group (joined through IGMP,
    so the switch forwards it), or 0 for any address that reaches the interface. IPv4 only (§6);
    fragments, and datagrams with a bad checksum, go on to lwIP as before.
  - **Depth:** the frames a flow may hold, queued and taken together (1 to 8). All the open flows'
    depths together are at most the receive reserve (8): `open()` fails with `Full` past it, so a
    flow within its depth can't be starved by another. A frame past its flow's depth is dropped
    and counted on the flow (the consumer fell behind), never queued for lwIP.
  - **Packet time** (the shortest time between its packets, either way) sets the bulk limit while
    it is open (below).
  - `receive(timeoutMs)` and `tryReceive()` give a `Packet`: the frame (its buffer, no copy), its
    hardware arrival time, the payload's offset and length, and the sender's address and port;
    `release()` gives the buffer back. Waiters are a `WaitList` (piece 3's `poll` joins it).
  - **Sending:** `connect(address, port, timeoutMs)` (UDP) or `setDestination(mac)` (EtherType)
    fixes where its packets go; `send(payload, flags, timeoutMs)` puts the flow's prebuilt
    Ethernet, IPv4 and UDP headers (checksums in software: the MAC can't insert them past 264
    bytes, §29) before the payload and sends it with `TxRealTime`: copied into a driver buffer in
    the sender's task, never through lwIP or its heap. Fragmenting is refused (`Invalid`). The
    source address is the interface's at the time of sending.
- **The classifier** (`bmds/netclassifier.h`, portable: host-tested and fuzzed) runs in `eth0`'s
  `receive()` for each frame while any flow is open, before the bulk share is checked: the
  EtherType (after one VLAN tag), then IPv4's header (version, length, no fragment), then UDP's
  destination port and address. Frames the MAC didn't checksum (`RxFrame::checksummed`) are
  checked in software. A match goes to its flow's ring (a spinlock, then the flow's waiters
  woken, outside it); its buffer doesn't count in bulk's share.
- **Destinations without lwIP:** a unicast destination's MAC (or its gateway's, off the subnet)
  comes from lwIP's ARP table at `connect()`; if lwIP has none, the flow sends its own ARP
  request. The classifier snoops every ARP frame (and still passes it on) for the senders flows
  are sending to, and keeps their MACs current; each flow asks again every 30 s while it sends.
  A full lwIP heap or bulk's drops can't stop a flow sending. A group's MAC is its own (RFC 1112).
- **The automatic limits:** while any flow is open, bulk holds at most 2 transmit descriptors (1 if
  a flow's packet time is under 1 ms) and the receive reserve is 8; the last flow closing gives
  bulk the 16 and 0 again. The driver does it as flows come and go.
- **Departure stamps go to the flow:** a stamped frame's descriptor names its flow; reclaiming it
  hands the stamp (with the ticket `send()` returned) to the flow's queue of 8 after the transmit
  lock, and wakes its waiters (`receiveTxStamp()`; piece 3's `MSG_ERRQUEUE`). None can be
  overwritten by later frames (§30a); a full queue drops its oldest (counted).
- **Closing** clears the flow from the classifier and the transmit ring's descriptors and waits
  for a delivery in progress (deliveries don't block), as `setReceiver()` does; queued frames are
  released.
- **Group filters:** an EtherType flow's group joins the MAC's address filters with the stack's
  groups; more than 15 together pass all multicast (more for `eth0` to drop, nothing lost).
- **The cores:** a flow's sender runs on core 1 (§30c); the receiver's wake comes from `eth0`
  (core 0, priority 21).
- **Shown:** `net` lists open flows (what they match, packets in and out, dropped, stamps); `stats`
  gains `net.ethN.rx_realtime` and `rx_realtime_dropped` (a warning).
- **Tested** by a temporary kernel command: a flow sends AES67-sized packets every 1 ms from core 1
  to a Python UDP reflector on sartre, another flow takes them back; the round trip and the send
  jitter from the hardware stamps, and the losses, while `bmds-netstress` loads the link. Piece 4's
  `netlat` repeats it through sockets.

### 30e. As built: piece 2, the classifier and flows (2026-10-12)

- **As designed (§30d):** `NetClassifier` (portable: 8 host test cases, `netclassifier_fuzz`: 54
  million inputs, seeded with a UDP and a PTP frame, no fault) and `NetFlow` in `bmds_net`. The
  driver keeps the flows' rules and classifies in `receive()` before the bulk share; a flow's
  buffers are a bitmask, so `release()` doesn't count them as bulk's. `NetFlow::list()` feeds
  `net`'s flow lines. `NetInterface` keeps its address in an atomic for senders outside the core
  lock. The image grew 4 KB.
- **Two bugs found reading it over:** the snooped ARP answer woke `connect()` inside the driver's
  flow spinlock (a semaphore can't be given in a critical section): now after it. A reclaim stops
  once it holds 4 flows' stamps (they're handed over after the lock); a sender waiting for room
  then slept though there were descriptors to reclaim: it now hands them over and reclaims again
  first.
- **Tested** with a temporary command (`rttest`, not kept: piece 4's `netlat` does it through
  sockets): a flow on core 1 (priority 20) sends 342-byte frames every 1 ms, stamped, to a Python
  reflector on sartre; a reader task (core 1, priority 19) takes them back. Departure is the
  hardware stamp less the 1588 time read just before `send()`; delivery is the reader's 1588 time
  less the hardware arrival stamp. Sartre's reflections marked EF (DSCP 46).

  | Load (from sartre) | Lost | Departure, mean / max | Delivery, mean / max | Round trip, max |
  |---|---|---|---|---|
  | None (10 s) | 0 of 10000 | 22 / 92 µs | 66 / 266 µs | 1.4 ms |
  | TCP both ways (50 s) | 0 of 50000 | 28 / 212 µs | 84 / 1966 µs * | 2.6 ms |
  | Flood paced at 95 Mbit/s (35 s) | 0 of 35000 | 24 / 202 µs | 91 / 1847 µs * | 3.6 ms |
  | TCP both ways and a 90 Mbit/s flood (60 s) | 0 of 60000 | 28 / 217 µs | 61 / 541 µs | 2.5 ms |

  \* The reader was then the shell's task; at priority 19 on core 1 the worst fell to 541 µs. The
  departure budget (§30: under 300 µs at 1 ms packets) holds in every case. The board lost
  nothing in any run: `rx_realtime_dropped`, `tx_realtime_waited`, `rx_missed` and `rx_overflow`
  stayed 0, while 38-80 bulk frames were dropped by the partition. A multicast flow (239.69.1.1,
  joined through IGMP, the group in the MAC's exact filters while open) and a `connect()` that had
  to ask ARP itself after a reboot also worked; to an address nobody has it gave
  `HostUnreachable` after its timeout.
- **An unpaced flood is lost in the switch, with the real-time packets queued behind it.** Sartre
  sends at 10 Gbit/s into the board's 100 Mbit/s port: the switch drops about 95% of the flood, and
  85-99% of the reflections sent while it runs (none outside it); the board received and delivered
  every one that reached it. Marking them EF cut the switch's queueing from 18 ms to 2 ms but not
  the drops (its buffer for the port is full). Nothing at the board can help: an AES67 network
  needs QoS in its switches (strict priority for EF, and policing), which this one doesn't give.
  So the done-when test (§30) floods at a paced rate (`bmds-netstress --flood-mbps`), and "no
  real-time packet lost" is counted at the board; it holds.
- **The cost to bulk while a flow is open** (*k* 2): raw TCP out 4.1-4.3 MB/s (4.9 without), in
  5.2-5.5 (6.0). With a flood running too, out fell to 1.7 MB/s: lwIP answers every datagram to a
  closed port with an ICMP port unreachable (no rate limit), about 7000 small frames a second in
  bulk's 2 descriptors. A follow-up: rate-limit ICMP errors as Linux does (and it closes an
  amplifier).
- **Next:** piece 3, the socket ABI (`SO_BMDS_FASTPATH` over `NetFlow`, `SO_PRIORITY`/DSCP,
  `SO_TIMESTAMPING` with `MSG_ERRQUEUE`), designed and brought to the user before it is built.

### 30f. Piece 3's design: the socket ABI (2026-10-12, for the user's review)

Frozen once shipped. Linux's option numbers, layouts and behaviour, checked against Linux 6.8 on
sartre (software stamps: the same paths as hardware ones), except one BMDS option.

- **`SO_BMDS_FASTPATH`** makes a UDP socket a `NetFlow` (§30d): its datagrams bypass lwIP both
  ways.
  - `setsockopt(fd, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &v, len)`: a level of BMDS's own
    (`0x424d4453`, "BMDS"), so no Linux number can collide. `v` is an `int` (0 off; otherwise on,
    a 1 ms packet time and a depth of 4) or a `bmds_fastpath_t` (`size`, `packet_us`, `depth`
    1-8); `getsockopt` gives the same back. UDP only (`ENOPROTOOPT`).
  - The flow opens once the socket is on and bound (either order; an unbound socket gets its
    ephemeral port then), on `SO_BINDTODEVICE`'s interface, else the bound address's, else the
    first Ethernet one. It matches the bound port and address (any, one of ours, or a group:
    joined with `IP_ADD_MEMBERSHIP` as now). `connect()` also narrows it to the peer's address and
    port, as Linux's connected UDP does: the classifier's rule gains a source (0 for any).
    `ENOBUFS` when the 8 flows or the 8-buffer reserve are taken, `EADDRINUSE` when another flow
    has the port, `ENODEV` without Ethernet. Off, or closing, closes the flow.
  - **Receiving** takes the flow's packets first, then lwIP's queue (fragments, which the
    classifier leaves to lwIP), copies into the iovecs and releases the buffer at once.
    `MSG_PEEK`, `MSG_TRUNC`, `msg_name` and the timeouts as for any UDP socket. The depth, not
    `SO_RCVBUF`, bounds the flow.
  - **Sending:** to the connected peer, a group, or any address in lwIP's ARP table (looked up,
    never waited for), the flow builds the headers and sends with `TxRealTime` in the caller's
    task. What it can't send that way, a datagram larger than one frame or one to an address
    ARP hasn't answered yet, goes through lwIP as before, so a program sees no difference but
    speed. `connect()` doesn't wait for ARP (Linux's doesn't): it asks, and sends go through
    lwIP until the answer comes. A group sent this way isn't looped back to local sockets.
  - `net`'s flow lines show socket flows like any other.
- **Priority:** `SO_PRIORITY` (any value: there is no privilege to check) and `IP_TOS` as Linux
  has them, `IP_TOS` setting the priority from the TOS bits as Linux does (EF gives 4,
  `IPTOS_LOWDELAY` 6). A plain UDP socket with priority 6 or more, or DSCP EF (46) or CS6 (48),
  sends with a new driver class, `TxPriority`: ahead of bulk's limit, but at most 2 descriptors
  of its own, so it can't take the flows' (§30's guarantee stands). It still goes through lwIP's
  core lock, which TCP holds, so real time means the fast path; priority is for the rest (NTP,
  mDNS answers). A fast-path socket is real time whatever its priority; the DSCP goes in its
  headers.
- **`SO_TIMESTAMPING`** (37, and 65 as musl's 32-bit headers have it: 64-bit times in the
  control message; 37 gives 32-bit ones), on every UDP socket, fast path or not:
  - Linux's flags (others `EINVAL`, as Linux): `RX_HARDWARE`, `TX_HARDWARE`, `RX_SOFTWARE`,
    `TX_SOFTWARE`, `SOFTWARE`, `RAW_HARDWARE`, `OPT_ID`, `OPT_TSONLY`, `OPT_CMSG`; accepted and
    not acted on: `TX_SCHED`, `TX_ACK` (TCP's), `OPT_STATS`, `OPT_PKTINFO`, `OPT_TX_SWHW`,
    `BIND_PHC` (there is one clock). The value is an `int` or Linux's `struct so_timestamping`;
    `getsockopt` gives the struct.
  - The hardware clock is the EMAC's 1588 clock (every frame is stamped, §4); it starts from the
    system clock at boot and step 7's PTP disciplines it. Software stamps are the system clock
    at `eth0`'s receive and at the descriptor's write.
  - **Received:** each datagram carries `SCM_TIMESTAMPING` (3 timespecs: software, 0, hardware).
    Through lwIP, the stamps ride in the pbuf (`LWIP_PBUF_CUSTOM_DATA`: 16 bytes more per pbuf,
    for both stamps and the transmit ticket below; 48 pool pbufs, the custom ones, the heap's).
  - **Sent:** each datagram sent while a transmit flag is on is stamped. The stamp goes to the
    socket's error queue (8; full, the oldest is dropped), read with `recvmsg(MSG_ERRQUEUE)` as on
    Linux (seen on sartre): the frame as sent, from its Ethernet header (nothing with
    `OPT_TSONLY`); `msg_flags` `MSG_ERRQUEUE`; no `msg_name`; `SCM_TIMESTAMPING`; and
    `SOL_IP`/`IP_RECVERR` with a `sock_extended_err` (`ENOMSG`, origin 4 timestamping, info 0
    `SCM_TSTAMP_SND`, data the `OPT_ID` count from 0) and an offender address (`AF_UNSPEC`; ours
    with `OPT_CMSG`). An empty queue answers `EAGAIN` at once, even blocking (Linux's). The
    frames are kept in PSRAM: 12 KB a socket, allocated when transmit stamps are turned on (none
    with `OPT_TSONLY`).
  - A plain socket's datagram carries a ticket in its pbuf (a registry slot and a generation), so
    the driver hands the stamp to the socket at reclaim, and a datagram that outlives its socket
    (in ARP's queue) finds nobody. Fragmented datagrams aren't stamped.
  - **`poll`:** a waiting error queue gives `POLLERR`, and with `SO_SELECT_ERR_QUEUE` (45)
    `POLLPRI` too (seen on sartre), as linuxptp waits for it. `BMDS_POLL_PRI`'s "never reported"
    becomes this. `Stream::Ready` gains `Error` and `Priority`.
  - `SO_TIMESTAMP` and `SO_TIMESTAMPNS` (29/63, 35/64: the software receive time) come along:
    the same stamps, a POSIX gap closed.
  - The interface `ioctl`s linuxptp checks (`SIOCSHWTSTAMP`, `SIOCGHWTSTAMP`, `ETHTOOL_GET_TS_INFO`)
    and `/dev/ptp0` are step 7's, with the clock they describe.
- **`sys.h` appends:** `BMDS_SOL_BMDS`, `BMDS_SO_FASTPATH`, `bmds_fastpath_t`, `BMDS_MSG_ERRQUEUE`
  (0x2000); the lock test checks them. Everything else is Linux's numbers from musl's headers.
- **Tested** by an `sdk/tests` program (options, errors, control message layouts, the error queue
  and `poll`, over `lo` and `eth0`, fast path and not), then piece 4's `netlat` against sartre.

### 30g. ICMP errors limited as Linux limits them (2026-10-12)

§30e's follow-up. `IcmpRateLimit` (portable, 4 host test cases) holds back lwIP's ICMP errors in
`NetInterface::linkOutput()` (under the core lock), with Linux's defaults: destination
unreachable, source quench, time exceeded and parameter problem (`icmp_ratemask`; not
"fragmentation needed") at most one a second to a destination with a burst of 6
(`icmp_ratelimit`, 16 destinations remembered), and 1000 a second in all with a burst of 50
(`icmp_msgs_per_sec`, `icmp_msgs_burst`). lwIP is vendored unchanged, so the frame is read as it
leaves. Counted in `net.ethN.tx_icmp_limited`. Image: +0.9 KB. Not yet measured on the board
(flashing waits for the user): expected, a flood to a closed port gets 1 answer a second, not
7000, and TCP out under the flood returns from 1.7 MB/s towards the 4.2 of a flow alone.

### 30h. As built: piece 3, the socket ABI (2026-10-12)

- **As designed (§30f), the user's yes.** `SocketFastPath` (`src/socketfastpath.h`) is how the
  portable `Socket` holds a `NetFlow`: the firmware's wraps one (`Internal` memory, opened at
  `setsockopt` or `bind`); the host tests' answers `NotSupported`. `NetFlow` gained what a socket
  needs: a listener (the socket's readers and pollers), `tryConnect()` (ARP asked, not waited
  for), `filterSource()` (the classifier's rule gained a source address and port), a destination
  per send (`macFor()` from lwIP's table), payloads in pieces (`writeUdpHeaders()` sums them, odd
  offsets byte-swapped: host-tested against the whole), `IP_MULTICAST_TTL` for groups. The driver's
  stamp receiver is now `TxStampSink` (a flow, or `NetInterface` for lwIP's datagrams, with a
  cookie), and `TxPriority` has its own limit (2 with flows, else 16).
- **lwIP's datagrams:** each pbuf carries the arrival stamps and a send class, cookie and number
  (`LWIP_PBUF_CUSTOM_DATA`, 21 bytes); `NetInterface` stamps arrivals and, for a marked datagram,
  records its frame with its socket and sends it stamped (a copy, not in place). Sockets are found
  by a lock-free registry of 16 (a slot and a 7-bit generation in the cookie), so a datagram that
  outlives its socket finds nobody. A socket's error queue is 8 records under a `Mutex` held for a
  few lines (`SpinLock` isn't in the host build; copies happen outside it), with the frames in
  PSRAM.
- **Not as Linux:** send stamps come from frames that leave by Ethernet: a datagram to the
  loopback gets none (Linux stamps it in software).
- **Tested:**
  - host: a real-time socket case (priorities, flags, receive stamps, the error queue's
    `WouldBlock`, `setFastPath` errors); the classifier's source rules and split payloads;
    `netclassifier_fuzz` seeded again and run 19 million times (rules with sources; the headers
    from split payloads must equal the whole's), no fault;
  - board: `progtest rtnet` (new) passes alone (options, `SO_TIMESTAMPNS` and `SO_TIMESTAMPING`
    on loopback, `MSG_CTRUNC`, the empty error queue at once, the fast path's errors) and against
    the reflector on sartre: four datagrams each through the fast path and through lwIP, with and
    without `OPT_TSONLY`, each with its error-queue report (the frame from its Ethernet header,
    `IP_RECVERR` with the `OPT_ID` count, `POLLPRI` and `POLLERR`) and the reflection's hardware
    arrival. Round trip, hardware stamp to hardware stamp: 81-158 µs both ways (the first of each
    socket the slowest). `net` showed the fast path used (9 real-time frames sent, 8 received)
    and the limits restored after.
- **Not measured yet:** `TxPriority` under load (piece 4's `netlat --load`).
- **Image:** +13.6 KB (1,766,928 bytes).

### 30i. As built: piece 4, `netlat` (2026-10-12)

- **`netlat`** (`programs/netlat`, the card's `netlat` package; public interfaces only): sends
  datagrams (300 bytes: AES67's 1 ms of 48 kHz stereo L24 and RTP's header) every interval to
  `tools/netlat-reflector` on a wired peer, through the fast path (`-n`: lwIP), stamped in hardware
  both ways (`SO_TIMESTAMPING` with `OPT_ID` and `OPT_TSONLY`, the error queue read with
  `POLLPRI`); `-l` loads the link with TCP both ways through the reflector meanwhile. It reports
  loss, reordering and, each as min, mean, 50, 99, 99.9 % and max:
  - **the sender's wake** against its deadline (`CLOCK_MONOTONIC`);
  - **the departure off the grid:** hardware departures against the even grid that fits them best
    (least squares on the sequence: the two clocks run at their own rates), less the least;
  - **the round trip,** hardware departure to the reflection's hardware arrival;
  - **driver to program:** the driver's software stamp to the reflection in the program's hands
    (`CLOCK_REALTIME` both).

  A program can't read the 1588 clock yet (step 7's `/dev/ptp0`), so the departure against the
  sender's own intent, and the arrival to the driver, aren't measured on one clock.
- **`tools/netlat-reflector`:** UDP back to its sender marked EF; a TCP sink on port + 1 and a
  source on port + 2 for `-l`.
- **A bug found by the first loaded run:** a stamp arriving while a send was in flight was given
  to it, even when it was an older datagram's whose record the full error queue had dropped (the
  departures came out milliseconds early). Only a ticket newer than every one written down is now
  the one in flight.
- **Measured** (sartre reflecting; 1 ms, 300 bytes, fast path; the program's threads at the
  priority every program has):

  | Load | Lost | Wake late, 50 % / max | Departure off grid, 50 % / max | Round trip, 50 % / max | Driver to program, 50 % / max |
  |---|---|---|---|---|---|
  | None (10 s) | 0 of 10000 | 501 / 1523 µs | 472 / 1604 µs | 100 / 2010 µs | 123 / 1518 µs |
  | 90 Mbit/s flood (30 s) | 10 of 30000 | 215 / 3756 µs | 254 / 3801 µs | 259 / 2096 µs | 178 / 4807 µs |
  | Flood and TCP both ways (`-l`, 30 s) | 5953 of 30000 | 282 / 5086 µs | 351 / 5708 µs | 290 / 3137 µs | 404 / 22581 µs |

- **What it shows:**
  - **The kernel holds:** in every run `rx_missed`, `rx_overflow` and `tx_realtime_waited` stayed
    0. Every lost datagram was dropped at its flow (`rx_realtime_dropped`): the program, not the
    path, fell more than its depth (4 ms) behind.
  - **A program's sleep follows the 1 kHz tick:** `clock_nanosleep` rounds up to it, so a 1 ms
    pace wakes anywhere in the next tick (half a millisecond late on average), and the departures
    follow the wake. §5.2's pacing (never from the tick) isn't open to programs.
  - **A program's threads can't be given a priority:** the receiver runs at the priority of
    everything else the shell starts, with `-l`'s TCP threads beside it, and under load waits up
    to 22 ms; the error queue (8) overflows too (28520 of 30000 stamps read). The kernel's own
    test thread (§30e), at priority 19 on core 1, lost nothing under the same load.
- **Next (ABI, for the user):** thread priorities for programs (`sched_setscheduler`,
  `pthread_setschedparam` with `SCHED_FIFO`, mapped under the drivers' tasks) and a sleep finer
  than the tick for real-time threads; with them `netlat` repeats the done-when test (§30).
  Done in §30j.

### 30j. Real-time program threads: `netlat -r` (2026-10-12)

Programs got Linux's scheduling calls, a band of kernel priorities (7-18) between the IP stack
and the drivers, Linux's 95 % budget, and a sleep that wakes from a hardware timer for real-time
threads (processes.md §3.12, slot 113). `netlat -r PRIO` runs its sender and receiver
`SCHED_FIFO` at PRIO; the TCP load (`-l`) stays at the normal priority.

- **Measured** (sartre reflecting; 1 ms, 300 bytes, fast path, `-r 50`: kernel priority 12):

  | Load | Lost | Wake late, 50 % / max | Departure off grid, 50 % / max | Round trip, 50 % / max | Driver to program, 50 % / max |
  |---|---|---|---|---|---|
  | None (10 s) | 0 of 10000 | 57 / 195 µs | 29 / 317 µs | 96 / 368 µs | 127 / 706 µs |
  | 90 Mbit/s flood (30 s) | 0 of 30000 | 33 / 974 µs | 46 / 1777 µs | 283 / 1785 µs | 111 / 2065 µs |
  | Flood and TCP both ways (`-l`, 30 s) | 1 of 30000 | 37 / 1962 µs | 47 / 2798 µs | 292 / 6073 µs | 129 / 3408 µs |

  Against §30i's: the wake's median from about 500 µs to under 60, and the losses under the
  full load from 5953 to 1 (every send stamp read: 30000 of 30000, from 28520). No frame was
  missed or waited for (`rx_missed`, `rx_overflow`, `tx_realtime_waited` 0); the one lost was
  dropped at the flow. The budget never throttled (the threads sleep most of each period).
- **What is left:** the tails. Under the full load the receiver's 99th percentile from the driver
  is 1.6 ms and the worst 3.4 ms, though it outranks everything but the drivers; a few wakes and
  departures run a millisecond or two late. Not yet analysed: what holds the receiver (a lock a
  normal-priority thread holds, such as the IP stack's while TCP runs, is the first suspect).

### 30k. The tails: the core lock, and the wake timer's core (2026-10-12)

Two causes, each found by taking one suspect away and measuring again.

- **The receiver took lwIP's core lock every time it looked.** Its datagrams come through its
  flow without the lock, but `Socket::readiness()` (every `poll()`) and a receive that found the
  flow empty both took the core lock to look at lwIP's own queue for the socket. TCP holds that
  lock for whole send-buffer copies (`sendTcp`'s `tcp_write` loop) and for its input. Priority
  inheritance hurries the holder, but the receiver still waits out the rest of its hold.
  A datagram or raw socket now reads its queue's count and its shutdown without the lock: both are
  atomics written under it. A datagram lwIP queues after the look releases `_readable`, a latched
  semaphore, so the waiter can't miss it. TCP sockets still take the lock. Under the full load the
  receiver's delay from the driver went from 1538 µs to 133 µs at the 99th percentile, and from
  3399 µs to 541 µs at worst.
- **The wake timer's interrupt was on core 0 half the time.** `gptimer_register_event_callbacks()`
  allocates it on the calling core: the first program thread to ask for `SCHED_FIFO`, unpinned.
  `esp_intr_dump()` showed it beside `ETH_MAC` and core 0's tick. It is now registered on core 1
  through `esp_ipc_call_blocking()`, away from `eth0` (21) and `esp_timer` (22). The wake under
  the full load:

  | Variant | Boots | 99 % | Worst |
  |---|---|---|---|
  | The caller's core | 2 | 116-124 µs | 630-1191 µs |
  | Core 1 | 5 | 51-66 µs | 253-455 µs |

  The handler in IRAM, with `CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM` and interrupt level 3, bought only
  3-4 µs at the median, so it wasn't kept.
- **Measured** on the final firmware (sartre reflecting; 1 ms, 300 bytes, fast path, `-r 50`):

  | Load | Lost | Wake late, 50 % / max | Departure off grid, 50 % / max | Round trip, 50 % / max | Driver to program, 50 % / 99 % / max |
  |---|---|---|---|---|---|
  | None (10 s) | 0 of 10000 | 31 / 178 µs | 2 / 326 µs | 99 / 1967 µs | 71 / 124 / 495 µs |
  | 90 Mbit/s flood (30 s) | 0 of 30000 | 32 / 290 µs | 4 / 528 µs | 287 / 3180 µs | 73 / 87 / 444 µs |
  | Flood and TCP both ways (`-l`, 30 s) | 0 of 30000 | 36 / 380 µs | 10 / 414 µs | 301 / 2614 µs | 74 / 116 / 679 µs |

  The loss and miss counters stayed 0: `rx_missed`, `rx_overflow`, `tx_realtime_waited` and
  `rx_realtime_dropped`. `rx_bulk_dropped` counted the flood. No datagram was lost in any of 26
  full-load runs with the socket change. The round trip's tail includes the reflector, Python on
  Linux. **Step 6's test (§30) passes.**
- **netlat ends when its peer stalls.** The TCP loaders blocked in `read`/`write` with no timeout,
  so a stalled reflector held `main` in `pthread_join` forever. The reflector serves one TCP
  connection at a time. `main` now shuts their sockets down first and closes them after the join;
  the sender uses `MSG_NOSIGNAL`. With the reflector stopped by `SIGSTOP` mid-run, netlat ended
  and reported.
- **Seen, not explained:**
  - One full-load run, early on, hung past its 75 s timeout. Two later runs woke about 1 ms late,
    on the tick's pace, while that process lived; once it was killed, the wakes were normal. It
    didn't happen again in 20 runs. A missed alarm isn't it: IDF's GPTimer fires at once on an
    alarm set behind the count. A real-time thread spinning would fit: the 95 % budget is global,
    so it would hold back every program's real-time threads. That boot's log was lost to a reflash.
  - The budget does throttle at the end of some runs. netlat sorts and fits its 30000 samples in
    soft-float doubles on its `SCHED_FIFO` main thread, after the measurement.
  - Each `ssh` login makes 196 reads of `flash0` through the driver, each one pausing the cache.
    Nothing reads it while idle. Not looked into.

## 31. Step 7's design: PTP, without the grandmaster for now (2026-10-12, for the user's review)

§10 stands: one ordinary clock in the kernel, either role by BMCA, two-step, end-to-end delay,
the media and default profiles, `ptpclock0` disciplined, and the system clock following it. The
user's grandmaster is down, and there is one board here, so this section sets what can be proven
without them, and builds so the rest is a test run later.

- **What can be tested now:**
  - **A simulated network on the host** carries most of the proof. The protocol is portable
    code: messages, the port state machine, BMCA, the servo. It runs against several simulated
    clocks with their own drift (±100 ppm), path delays, jitter and loss. Tests cover: BMCA's
    choice and its ties; a master falling silent and the next taking over with its last
    frequency (holdover, no jump); convergence time and steady-state offset for a given jitter;
    a better master arriving and the board standing down. The message parser is fuzzed.
  - **The board alone** becomes master once the announce timeout passes with nothing heard.
  - **The board against linuxptp on sartre.** `ptp4l` is installed there, but its NIC (mlx4) has
    software timestamps only (no PHC), and it needs root: the user runs it. Tests: `ptp4l`
    following the board (its log shows offset and path delay); the board following `ptp4l` (a
    better priority1 there); `ptp4l` stopped, the board taking over; restarted, the board standing
    down. Both transports, both profiles. Precision is bounded by sartre's software stamps (tens
    of µs), so this shows the protocol works with linuxptp, not how well it locks.
- **What waits for the grandmaster or a second board:** lock against GPS time, two boards
  electing alone, PPS offsets on a scope (§12's step 7 done-when, unchanged). PPS out is built
  now, so a scope can already show the board's second against anything else.
- **The ABI** (frozen once shipped; for the user):
  - **Settings** `net/ptp/*` as §10's table, with `net/ptp/enabled` off by default (as SSH).
  - **`ptp`:** role and state, grandmaster and its class, offset, path delay, frequency, the
    foreign masters heard and why BMCA chose; `ptp -v` per message counts.
  - **Counters** `net.ptp.*` in the stats registry: messages each way per type, timeouts, BMCA
    changes, steps, the last offset and delay.
  - **`/dev/ptp0`,** Linux's PHC interface: `clock_gettime(FD_TO_CLOCKID(fd))`,
    `PTP_CLOCK_GETCAPS`, `PTP_SYS_OFFSET_EXTENDED`. Programs read it (netlat's one-way delay,
    AES67 later). `clock_adjtime` and `clock_settime` on it are refused (`EBUSY`) while the PTP
    service runs, and allowed while it is off, so a ported `ptp4l` could drive it.
    `PTP_PEROUT_REQUEST` for the PPS pin, the same rule.
  - **`CLOCK_TAI`:** the system clock plus the UTC offset: from PTP when locked, otherwise from
    the leap-second file.
  - **The leap-second file** (the user's, 2026-10-12): IANA's `leap-seconds.list`, kept as is,
    about 5 KB of text: one line per leap second, a hash, and an expiry about six months ahead.
    - **Source:** the same signed tzdata release `tools/update-tzdata` already fetches and checks.
      It keeps that file in `thirdparty/tzdata` too, so there is no new source and no new trust.
    - **Where:** in the ROM as `/rom/share/leap-seconds.list`, and in the card's `tzdata` package
      as `share/leap-seconds.list`.
    - **`clock/leapseconds`** (`path`) works as `clock/tzdata` does:
      - it defaults to `/sd0/p0/share/leap-seconds.list`, so a newer file on the card overrides
        the ROM's without a reflash;
      - while no file is at that path, the ROM's is used;
      - an empty path means none (the offset is known only from PTP).
    - **A stale file loses to a fresh one:** if the file at the path expires before the ROM's,
      the ROM's is used, and `clock` says so.
    - **The hash is checked;** a file that fails it is ignored and logged.
    - **Past its expiry,** the last offset is still used, but a master announces it with
      `currentUtcOffsetValid` false and `clock` says it is stale.
  - **`clock`** adds the TAI offset, its source (PTP, or which file) and the file's expiry;
    `clock leapseconds` shows the file in use and its entries, as `clock tzdata` shows its file.
- **Inside:**
  - **Transport:** fast-path flows (§30e) for EtherType 0x88F7 (group 01:1B:19:00:00:00) and
    UDP 319/320 on 224.0.1.129, with hardware stamps both ways. A Delay_Req's departure comes
    back through the flow's stamp queue; a Sync's goes into its Follow_Up.
  - **The driver adds** a stepped adjustment (offset added to the 1588 clock) and the PPS output
    (the MAC's flexible PPS on GPIO26 or 27, from the board definition).
  - **The servo:** linuxptp's PI (its default gains scaled by the sync interval). It steps beyond
    1 ms at the start (and after that, only if `net/ptp/step` allows it); otherwise it slews
    through the addend. The system clock follows `ptpclock0` as a clock source above NTP; NTP
    keeps running and takes over if PTP loses lock.
  - **The service** is a kernel task at the network's real-time priority on core 1. It wakes on
    flows and timers, with no polling.
- **Steps:**
  1. The portable core and the host simulation; fuzz the parser.
  2. The driver's step and PPS output; `net` shows PPS.
  3. The service, settings, `ptp`, counters; the system clock following.
  4. `/dev/ptp0`, `CLOCK_TAI` (slots, `linux.c`), the leap-second file (`update-tzdata`, the ROM,
     the `tzdata` package, `clock/leapseconds`, `clock`); host tests for the parser, its hash
     and expiry; fuzzed.
  5. The board against `ptp4l` (the tests above); results recorded here.

### 31a. As built: step 1, the portable core (2026-10-12)

- **`bmds/ptp.h`** (`ptp.cpp`, `ptpport.cpp`; portable, in the host build and the firmware):
  - **Messages:** Sync, Delay_Req, Follow_Up, Delay_Resp and Announce built and parsed, byte for
    byte as IEEE 1588-2008 lays them out. TLVs are skipped, and 1588-2019's minor version is
    accepted. Peer delay, signaling and management messages are read as far as the header.
  - **The dataset comparison** follows figures 27 and 28 (better, or better by topology).
  - **The servo** is linuxptp's PI: the gains for hardware stamps, scaled by the sync interval
    (kp 1.31, ki 0.13 at 125 ms), and a step only for the first offset beyond 20 µs.
  - **The port:** an ordinary clock's state machine: listening, master, passive, uncalibrated,
    slave. Foreign masters qualify after 2 Announces in 4 intervals. Master: Announce, Sync and
    Follow_Up from the departure stamp, Delay_Resp. Slave: two-step Syncs in either order, path
    delay as the median of 7, randomized Delay_Req intervals. The owner gives it messages, stamps
    and the time; it acts through `Io` (send, now, step, frequency, the UTC offset).
- **Holdover, found by the failover test:** the frequency held once the master is gone was the
  last correction, which carries the last offset's proportional part. It was 1 ppm off at 1 µs of
  jitter, so the new master drifted 46 µs from the old one's time in 45 s. Two fixes:
  - **What it holds to:** an average of the frequency applied over 32 s, started after 8 s of lock
    so the pull-in is left out (until then, the integral). It holds 16 ppb from true.
  - **When it holds:** a slave whose Syncs stop for 4 intervals holds the averaged rate while it
    waits for the announce timeout (1588-2019's sync receipt timeout). Before, it ran 6-8 s on the
    last correction.

  Together: 1.2 µs in 45 s.
- **Times from the wire saturate:** a master's times plus their corrections, and the differences
  of four stamps, are kept within ±73 years. A hostile `t1` could otherwise overflow `int64_t`.
- **Host tests** (`tests/host/ptp.cpp`, 13 cases, 0.3 s): the messages byte by byte and refused,
  the comparison, the servo, and clocks on a simulated network. Each simulated clock has its own
  drift (to ±80 ppm); messages see 30 µs of path, jitter and loss; stamps are quantized to 8 ns.
  Every port sends and parses real bytes.

  | Case | Result |
  |---|---|
  | A slave locks (1 µs jitter) | within 251 ns over 30 s; path delay 30.4 µs (true 30.5); one step |
  | Election | the lowest identity, else priority1; a slave-only clock follows and never masters |
  | Failover | the next master in under 15 s, no step anywhere; the remaining slave follows it within 500 ns |
  | A better master arriving | takes over; the old master follows it |
  | 10 % loss, 5 µs jitter, Follow_Ups ahead of their Syncs | within 2.3 µs |
  | Another domain | ignored. A class-6 clock beaten by another goes passive |
  | priority1 changed live | the election runs again |
- **Fuzzed** (`ptp_fuzz`, `tests/fuzz/make-ptp-seeds.py`): every message round-trips. A port is
  driven by the input (messages, time, stamps, configurations), and what it sends must parse.
  The seeds reach slave (one step) and master. 10 minutes: 1.1 million inputs, 407 edges, nothing
  found.
- **Size:** 11.5 KB of code in the firmware, no RAM until step 3 opens a port.

### 31b. As built: step 2, the clock's step and PPS out (2026-10-12)

- **`EthernetDevice::stepPtpTime(ns)`** adds or takes away a step in one hardware update, so
  nothing is lost to the time a read and a set take. The update registers take the step as it
  is, with a subtract bit; Linux's stmmac writes a complement for a subtraction, which lands
  about a second out here (measured; docs/platforms/esp32p4.md §4a). `ptpAddend()` and
  `NominalPtpAddend` give the clock's rate.
- **PPS out:** §31 planned the MAC's own PPS. On this chip (v1.3) it doesn't reach a pin, and
  ESP-IDF documents its `esp_eth_mac_set_pps_out_gpio()` from revision 3 on (the user found
  the call). So there are two paths, chosen at start by the chip's revision:
  - **From revision 3:** the MAC's PPS, routed through the GPIO matrix as ESP-IDF does it: a
    25 ns pulse on each second (fixed mode 0). Untested: no such chip here.
  - **Before (this board):** `PpsOut` (`src/ppsout.cpp`). A GPTimer on PLL_F80M ÷ 2 (25 ns
    counts; the PLL is a whole multiple of the crystal the 1588 clock counts) toggles the pin
    through the ETM when its alarm matches, with no interrupt in between. Its interrupt, after
    each edge, arms the next: rising on the 1588 clock's next second, falling 100 ms later.
    - An alarm's count comes from the timer's count, latched between two reads of HP_SYSTEM's
      copy of the 1588 time bus, and the addend's rate. That bracket is 200-240 ns, so an edge is
      within about 120 ns.
    - A step or a set re-arms the pending edge from the clock as it is.
    - A toggle gone astray is put right before the next edge is armed.
- **Board definition:** `BoardEthernet::ppsOut` is GPIO26: EXT1 pin 4, which isn't fitted. It
  is the second FS PHY's D-; USB uses the UTMI PHY. `docs/HARDWARE.md` updated.
- **`net`** shows `PPS out on GPIO26: a timer's, rising on each second (within N ns), 100 ms
  high; P pulses, M missed`.
- **Measured on the board** (a temporary command polling the pin; it can only see an edge some
  microseconds late):
  - each rise at x.000000159 to x.000118 s on the 1588 clock, each fall 100 ms after;
  - after a +0.3 s step, the next rises still on whole seconds;
  - no missed edges.

  `progtest net` and `rtnet` still pass.
- **Costs:** one GPTimer, one ETM channel and one GPIO ETM task; an interrupt every 500 ms
  (each one a few µs). Code: `PpsOut` 1.6 KB, plus ESP-IDF's ETM drivers (gptimer_etm, gpio_etm,
  esp_etm): 8.3 KB as objects, less once unused functions are dropped.

### 31c. Step 3's design: the service and its ABI (2026-10-13, approved by the user)

- **The service** (`ptpservice.cpp`): a kernel task on core 1 at `CONFIG_BMDS_PTP_PRIORITY` (18:
  the top of real-time programs, under the audio clients and the drivers). It owns the
  `Ptp::Port` and its flows, and sleeps on one semaphore that the flows' listeners release (a
  packet or a departure stamp) with the port's deadline as its timeout. No polling, no core lock.
  - **Flows:** `udp`: two `Udp` flows on 224.0.1.129, ports 319 (event) and 320 (general),
    depth 2 each, TTL 1, DSCP 46 (AES67's for PTP). `l2`: one `EtherType` flow on 0x88F7 with
    group 01:1B:19:00:00:00, depth 3. Four of the eight receive buffers kept for flows stay
    free for AES67.
  - **The PTP clock** is `ptpclock0`: stamps from the frames, `now()` from the MAC, steps through
    `stepPtpTime()`, the frequency through `setPtpFrequency()` (whole ppb: 2.7 addend steps;
    the integral takes up the rounding, so no finer call).
  - **At start,** if the system clock is set, `ptpclock0` is set to it plus the UTC offset: it
    runs on TAI from then on (the driver seeds it with UTC).
  - **The UTC offset** until step 4: a valid master's; otherwise 37 s, announced as not valid.
- **Which clock follows which** (once a second, the two read around each other, the tightest of
  4 readings; a second PI servo with linuxptp's gains at 1 s, as `phc2sys` does):
  1. a slave with its servo locked: the system clock follows `ptpclock0` (source `ptp`); NTP
     keeps polling but doesn't touch the clock;
  2. otherwise, while NTP is synced: NTP has the system clock, and a master's `ptpclock0`
     follows it (`phc2sys -s CLOCK_REALTIME`), so a board alone serves NTP's time;
  3. otherwise, if the system clock's source is already `ptp` (a slave whose master went):
     it keeps following `ptpclock0`, which is in holdover;
  4. otherwise nobody follows: both run free.
- **What a master announces:** class 248 always (no reference of its own). While NTP is synced:
  timeSource NTP (0x50), and an accuracy from NTP's last offset and half its delay (e.g. 0x2B,
  within 10 ms), which ranks it above a board that isn't (0xFE, unknown) in BMCA.
- **Not done:** §31's `net/ptp/step` is dropped: the port steps only a first offset beyond 20 µs
  (after each change of master), as linuxptp; later offsets slew.
- **Settings** (`net/ptp/*`, applied live; a change of interface, transport or domain restarts
  the port):

  | Key | Type | Default | Meaning |
  |---|---|---|---|
  | `enabled` | bool | `false` | Run PTP |
  | `interface` | string | `eth0` | The Ethernet interface |
  | `domain` | int 0-127 | `0` | The PTP domain |
  | `profile` | `media\|default` | `media` | The intervals `auto` stands for |
  | `transport` | `udp\|l2` | `udp` | UDP/IPv4 multicast, or Ethernet |
  | `priority1`, `priority2` | int 0-255 | `128` | BMCA's priorities (lower wins) |
  | `slave-only` | bool | `false` | Never be master |
  | `sync-interval` | `auto\|-7`..`4` | `auto` | log2 s between Syncs (media -3, default 0) |
  | `announce-interval` | `auto\|-3`..`4` | `auto` | log2 s between Announces (both 1) |
  | `delay-interval` | `auto\|-7`..`5` | `auto` | log2 s between Delay_Reqs, as master tells slaves (media -3, default 0) |

  `delay-interval` is new over §10's table: `ptp4l` tests want it.
- **`ptp`** (clock identities as linuxptp prints them):

  ```
  PTP: slave on eth0 (udp, media, domain 0) for 2 min 13 s
  Master: 001b21.fffe.aabbcc port 1; grandmaster the same, class 6, accuracy 100 ns, GPS
  Offset +0.123 us, path delay 31.204 us, frequency +12.345 ppm, servo locked
  UTC offset 37 s (the master's); the system clock follows ptpclock0
    Foreign master            Port Class Pri1 Pri2 Steps Accuracy
  * 001b21.fffe.aabbcc           1     6  128  128     0   100 ns
    e8f60a.fffe.e2e222 (this)    1   248  128  128     0  unknown
  ```

  As master the second line is `Grandmaster: this clock, class 248, accuracy 10 ms, NTP`, and
  the offset line is left out. Off: `PTP: off (net/ptp/enabled)`. `ptp -v` adds per-type
  message counts (sent, received) and the rest of the counters below, and the flows' drops.
- **Counters** `net.ptp.*`: `state` (disabled|listening|master|passive|uncalibrated|slave),
  `offset_ns` (kept as history), `delay_ns`, `frequency_ppb`, `rx_<type>` and `tx_<type>` for
  sync, delay_req, follow_up, delay_resp and announce, `ignored`, `send_failures`,
  `announce_timeouts`, `state_changes`, `master_changes`, `steps`, `follow_ups_missed`.
- **`clock`** shows source `ptp` as it is. **`ntp`** adds `(the clock follows PTP)` to its first
  line while PTP has it.
- **A cost to measure:** while PTP runs, its flows are open, so bulk traffic is held to its
  share of the driver (§30e: 8 receive buffers and 2 transmit descriptors kept for flows). TCP's
  throughput with PTP on is measured and recorded here.

### 31d. As built: step 3, the service (2026-10-13)

- **As §31c,** with these differences:
  - **The task starts the first time PTP is enabled** (at boot if saved), and stays, idle, once
    disabled: no internal RAM until then. Its state is a `PsramObject`.
  - **A master on the arbitrary timescale:** `ptp4l` on software stamps runs on the system's UTC
    and doesn't set `ptpTimescale`. A slave of such a master takes its time as UTC (offset 0),
    as `phc2sys` does; `ptp` says `UTC offset 0 s (the master's time is UTC, not PTP's
    timescale)`. If the board then becomes master, ptpclock0 steps 37 s back to TAI.
  - **The clocks' following isn't a second linuxptp servo:** the system timer and ptpclock0
    count the same crystal (docs/platforms/esp32p4.md §4a), so the follower takes the other's
    rate and a PI (kp 0.2, ki 0.02 a second) corrects only the offset. linuxptp's gains on reads
    a microsecond apart swung the system clock's rate 1-2 ppm a second. A step for the first
    offset beyond 20 µs, any beyond 1 ms (a new master, NTP's step), and when the system clock
    is taken over.
  - **The master line** shows the grandmaster's priorities, not its time source (the port keeps
    only the dataset). Ours, as master, shows its own source.
- **Measured on the board alone:**
  - Master after the announce timeout; Sync, Follow_Up and Announce sent, every departure
    stamped, none failed. ptpclock0 followed the NTP-disciplined system clock within ±0.5 µs
    a sample (±0.4 ppm in rate) and announced "NTP, accuracy 25 ms" (NTP's offset and half its
    delay).
  - `l2` and the default profile (a Sync a second); slave-only stays listening, class 255; PTP
    starts at boot when saved.
  - TCP with sartre, 32 MB each way: 39 Mbit/s in and out without PTP, 36 with it (the flows
    hold bulk traffic to its share): about 7 %.
  - The task used 2.2 KB of its 4 KB stack through all of it, and 243 ms of CPU in 6 minutes.
    Code: `ptpservice.cpp` 11.6 KB and `ptpcommand.cpp` 4.7 KB as objects (with their text).
- **Against `ptp4l` on sartre** (UDP, software stamps there; §31 step 5's first runs):
  - **`ptp4l -S -s --free_running 1` following the board:** it chose the board as best master
    and measured a path delay of 21-23 µs (±3-4 µs). Not steering its clock, its offset was
    sartre's clock against the board's, closing at the 7 ppm between them.
  - **The board following `ptp4l -S --priority1 100`:** slave and locked after one step; path
    delay 19-34 µs; offsets mostly within ±2 µs, up to 23 µs when sartre's stamps strayed, the
    rate swinging up to ±11 ppm with them (the master's noise; a hardware-stamped grandmaster
    will show the board's own). The system clock followed ptpclock0 (source `ptp`) and NTP
    showed `(the clock follows PTP)`.
  - **`ptp4l` stopped:** the board timed it out, became master without a step of the port, NTP
    had the system clock again, and ptpclock0 followed it within a microsecond.
- **The same over Ethernet (`-2`, `net/ptp/transport l2`):** the board as master answered every
  Delay_Req (154 of 154 in the first 20 s); as slave it locked after one step with a path delay
  of 17.6 µs (UDP's 19-23: sartre's IP stack left out) and offsets within ±2 µs after the first
  half-minute; with `ptp4l` stopped it became master again and NTP had the clock back. Nothing
  dropped, failed or missed in any run.
- **Known:** `clock`'s source stays `ptp` after NTP takes the clock back, until NTP next steps
  it: NTP slews without setting the source.

### 31e. Step 4's design: /dev/ptp0, CLOCK_TAI, the leap-second file (2026-10-13, approved by the user)

- **The leap-second file:** `thirdparty/tzdata/leap-seconds.list`, from 2026e (`update-tzdata`
  keeps it now; signature checked): 5 KB, 28 lines (1972's start and 27 leap seconds), TAI-UTC
  37 s since 2017-01-01,
  updated 2026-07-06, expires 2027-06-28. Its `#h` is SHA-1 over the digits of the update time,
  the expiry, then each line's two numbers, run together (checked against this file).
  - **The ROM** gets it as `/rom/share/leap-seconds.list` (`bmds_storage`'s ROM build, as
    `zoneinfo.bin`); **the `tzdata` package** as `share/leap-seconds.list`.
  - **`clock/leapseconds`** (`path`, default `/sd0/p0/share/leap-seconds.list`), as §31: the ROM's
    while nothing is at the path; the one that expires later of the two wins; empty: none.
  - **Parsing** in `bmds_core` (`leapseconds.h`, host-tested and fuzzed), with a small SHA-1 there
    (`bmds_core` takes no ESP-IDF; 20 lines of rounds). A file that fails its hash or doesn't
    parse is ignored and logged. Loaded at boot and when the card mounts or the setting changes
    (as the zone database), into a table in the kernel (`Clock`): at most 64 entries.
- **The TAI offset** (`Clock::taiOffset()`), in this order:
  1. a PTP slave's master's, when valid (and on PTP's timescale);
  2. the file's, for now (past its expiry still, but stale);
  3. none: 0, so `CLOCK_TAI` reads UTC (Linux's behaviour without an offset).

  The PTP service uses it in place of step 3's constant 37, and a master announces
  `currentUtcOffsetValid` only from an unexpired file, with `leap61`/`leap59` through the UTC day
  that ends in a leap second the file lists (1588's meaning: the day's last minute).
- **ABI, for the user:**
  - **`BMDS_CLOCK_TAI 2u`** for `clock_get` (an older kernel answers `BMDS_EINVAL`; `linux.c`'s
    `CLOCK_TAI` then reads `CLOCK_REALTIME`, as Linux does with no offset).
  - **`/dev/ptp0`**, a device file per Ethernet device's 1588 clock, and one new slot:
    `BMDS_SYS_PTP_CTL 116u`, `int32_t ptp_ctl(int32_t fd, uint32_t op, void *arg)`:

    | Op | Arg | Does |
    |---|---|---|
    | `BMDS_PTP_TIME 0` | `int64_t *` | reads the clock: ns since 1970 on PTP's timescale (TAI) |
    | `BMDS_PTP_CAPS 1` | `bmds_ptp_caps_t *` | `size`, `max_adj_ppb` (500000), `flags` (`BMDS_PTP_CAP_PPS`: a PPS pin runs), `pps_pin` (GPIO, or -1), `index` (0 for ptp0), `reserved` |
    | `BMDS_PTP_OFFSET 2` | `bmds_ptp_offset_t *` | `size`, `clock` (`BMDS_CLOCK_*`), `count` (1-25 in; done out), `reserved`, then `samples[25][3]` ns: the system clock, the PTP clock, the system clock |
    | `BMDS_PTP_SET 3` | `const int64_t *` | sets it |
    | `BMDS_PTP_STEP 4` | `const int64_t *` | adds to it in one update |
    | `BMDS_PTP_GET_FREQ 5`, `BMDS_PTP_SET_FREQ 6` | `int64_t *` | its frequency correction, parts per trillion (the driver rounds to its 0.37 ppb) |

    Set, step and frequency answer `BMDS_EBUSY` while the PTP service runs on that device.
  - **`linux.c`:** `clock_gettime(FD_TO_CLOCKID(fd))` and `clock_getres` on it;
    `clock_settime` and `clock_adjtime` on it (`ADJ_SETOFFSET`, `ADJ_FREQUENCY`, `ADJ_NANO`; other
    modes `EINVAL`); ioctls `PTP_CLOCK_GETCAPS(2)`, `PTP_SYS_OFFSET`, `PTP_SYS_OFFSET_EXTENDED`
    (its `clockid`: realtime, monotonic or TAI); `PTP_SYS_OFFSET_PRECISE` is `EOPNOTSUPP` (no
    cross-timestamping). `CLOCK_TAI` (11).
  - **Not now: `PTP_PEROUT_REQUEST`.** §31 listed it. The PPS already runs whenever the 1588
    clock does; another period needs `PpsOut` generalised. The AES67 step brings it if a media
    clock output wants it. CAPS says no periodic outputs (`n_per_out` 0), `pps` 1 when the pin runs.
  - **`clock`** adds, after `Frequency:`:

    ```
    TAI:       UTC+37 s, from PTP's master
    TAI:       UTC+37 s, from /rom/share/leap-seconds.list (expires 2027-06-28)
    TAI:       UTC+37 s, from /sd0/p0/share/leap-seconds.list (expired 2027-06-28: stale)
    TAI:       unknown (no leap-second file: clock/leapseconds); CLOCK_TAI reads UTC
    ```

    and **`clock leapseconds`**, as `clock tzdata`:

    ```
    /sd0/p0/share/leap-seconds.list: not there: the ROM's, /rom/share/leap-seconds.list
    Updated 2026-07-06, expires 2027-06-28; 28 leap seconds, UTC+37 s since 2017-01-01
      1972-01-01  +10 s
      1972-07-01  +11 s
      ...
    ```

    A file that fails: `/sd0/p0/share/leap-seconds.list: ignored: its hash doesn't match` (or
    `not a leap-second list`), then the ROM's.
- **Tests:** host tests for the parser (the real file, its hash, a changed digit, expiry, the
  offset at dates around leaps); a fuzzer; `progtest` for `CLOCK_TAI`, `/dev/ptp0` (read,
  `PTP_SYS_OFFSET_EXTENDED`, `EBUSY` while the service runs, a step and frequency while it
  doesn't).

### 31f. As built: step 4 (2026-10-13)

- **As §31e,** with these differences:
  - **`clock/leapseconds` can't be empty:** `path` settings refuse "" (`clock/tzdata` the same),
    and accepting it for every path setting would hand all their readers a new case. There is
    always a list: the card's or the ROM's.
  - **The offset is taken at the list's update, not now:** the list is read at boot, maybe before
    the clock is set (1955); its update time is before the real now, and a list names at most
    the change still to come, so `Clock` keeps the offset then and the next change.
  - **`bmds_fs` doesn't log** (it is host-built without the log): a refused list is told through
    `Vfs::setLeapSecondsRefused()`, which the system configuration sets to a warning.
  - **`linux/ptp_clock.h`** is new in `sdk/include` (as `sys/soundcard.h`): Linux's layouts and
    numbers, written for BMDS, with `FD_TO_CLOCKID`. `clock_getres` on a PTP clock says 1 ns, as
    Linux's does.
  - **A slave's master's valid offset** goes to `Clock` (source `ptp` for `CLOCK_TAI`) only while
    the system clock follows it.
- **Host tests** (`tests/host/leapseconds.cpp`): SHA-1's FIPS vectors; the kept list (28 lines,
  37 s, expiry 2027-06-28, the offset either side of 2017-01-01); a changed offset and a moved
  expiry fail the hash; cut, empty and out-of-order lists refused; CR LF and blank lines taken; the
  kernel's choice (file, a master's over it, none). `process.cpp`'s clock id 2, the invalid one
  until now, is TAI: 3 is checked instead. **Fuzzed** (`leapseconds_fuzz`, seeded with the kept
  list): 10.5 million inputs in 5 minutes, nothing found.
- **On the board:**
  - `clock`: `TAI:       UTC+37 s, from /rom/share/leap-seconds.list (expires 2027-06-28)`; with a
    copy on the card, from it; a copy with one offset changed: `ignored: its hash doesn't match:
    the ROM's`, and a warning in the log.
  - `progtest ptp`: `CLOCK_TAI` UTC+37.000 s; `/dev/ptp0`'s caps (500000 ppb, PPS);
    `PTP_SYS_OFFSET_EXTENDED` read within 3 µs a sample (the call, two system reads and the MAC's);
    a +1 s step lands at +1.0013 s (the reads between); +100 ppm reads back +100.000;
    `PTP_SYS_OFFSET_PRECISE` `EOPNOTSUPP`; past `max_adj` `ERANGE`.
  - `progtest ptp busy` with the service on: `EBUSY` for `clock_settime` and `clock_adjtime`;
    ptp0 then UTC+36.9993 s (TAI, the service having set it, still pulling in).
  - `progtest net`, `rtnet` and `time` still pass.
- **Costs:** code 4.6 KB as objects (the parser 1.6, SHA-1 0.8, the loader 1.3, `/dev/ptp0` 1.0);
  internal RAM 0.3 KB (two paths and `Clock`'s 28 bytes); the ROM 5 KB; PSRAM only while a list is
  read: its text (5 KB) and two tables (1 KB each).

### 31g. As built: step 5, the board against `ptp4l` (2026-10-13)

The runs of §31d, again with step 4 in, then what §31 still asked: the board standing down for
`ptp4l` restarted, and both profiles. One script on sartre (the user ran it with sudo): `ptp4l -S
-s --free_running 1` following the board for 60 s with `pmc` reading it; `ptp4l -S --priority1
100` for 60 s, stopped for 20, restarted for 40; on the media profile (`ptp4l` too at 8 Syncs
and Delay_Reqs a second), then the default. The board was watched from here every 4 s.

- **The first run found three faults** (fixed; the third run below is clean):
  - **The board's clock went 37 s ahead each time it went back to slave of `ptp4l`.** As master,
    ptpclock0 moves to TAI; `ptp4l` on software stamps is on UTC. linuxptp's servo steps only the
    first offset of its life, and `Servo::reset()` (a new parent) kept that: back to slave, the
    servo locked 37 s off and slewed at its 500 ppm bound (20 hours), and the system clock,
    following a locked servo, took the 37 s. Three returns: 111 s. The board as master then took
    sartre with it (that script's `ptp4l -s` lacked `--free_running`: stepped 37 s, chrony took it
    back). Now a reset re-arms the first step: a new master's first offset beyond 20 µs is
    stepped (linuxptp's clock doesn't move while master; ours does).
  - **For 2 s the timescale was the board's own.** The Announce that chooses a parent comes before
    it is the parent; its timescale and UTC offset were taken from the next (2 s on the media
    profile), the port keeping its master's flags (PTP's timescale) until then. The servo locks
    in 0.5 s at 8 Syncs a second: the system clock followed `ptp4l`'s UTC as TAI, 37 s back, until
    the next Announce. Each foreign master's last Announce's time properties are kept now and
    taken when it is chosen.
  - **NTP kept PTP's rate.** Following sartre while chrony slewed it back, the system clock ran
    at -320 ppm; given back, NTP kept that rate (its discipline starts from the clock's) and its
    1024 s poll. Now `NtpClient::clockReleased()` (the service, on letting go) puts back the
    frequency of NTP's last update and polls at once, the discipline started again.
- **NTP's discipline takes a large rate error whole** (the user's ask): the -320 ppm survived a
  reset in the RTC record, and 6% an update (64 s) took an hour. Each interval of 32 s or more
  also estimates the frequency that would have kept the clock (the one set, plus its error);
  from 3 on, when their mean is beyond 4 standard errors from the frequency set, the mean is
  taken. An interval's error is its two ends' noise, so the mean's is the first and last offsets'
  over n intervals: its variance is theirs over n², not n (a jitter bound failed on the board: the
  filter's jitter carries the very rate error). Host tests: 253 ppm off with ±9 ms noise to within
  35 ppm in 4 updates (the weighing alone: 198); a good rate in that noise moves at most 2.5 ppm.
  On the board: -82 ppm to -14 after 3 updates, then +0.8.
- **The third run** (all three fixes):
  - **`ptp4l` following the board** (both profiles): best master the board; `pmc`: UTC offset 37,
    `currentUtcOffsetValid` 1, `ptpTimescale` 1, time source NTP (0x50), accuracy 100 ms (0x2D);
    path delay 18-21 µs. Its offsets (1.5-2.9 ms) are sartre's NTP time against the board's.
  - **The board following `ptp4l`, stopped, restarted** (both profiles): slave and locked after
    one step each time (4 slave runs, 4 steps); offsets within ±1 µs on the default profile, ±8 µs
    on the media (sartre's stamps); master again 7 s after `ptp4l` stopped, NTP's frequency back
    each time (+0.800 ppm), and slave again when it restarted. The board's clock stayed within
    ssh's latency of this PC's through the run: no step of the system clock beyond them.
  - Nothing dropped, failed, missed or lost: 2927 frames in, 13542 out, 6756 stamps.
- **Known:** `clock`'s source stays `ptp` after NTP has the clock back (as §31d), until NTP next
  steps it.
- **Waits for the grandmaster or a second board** (§31): lock against GPS time, two boards electing,
  PPS on a scope.

### 31h. Where PTP stands, and what comes next (2026-10-13)

Steps 1-5 of §31 are done and committed. Left of step 7 (roadmap): what needs a hardware-stamped
grandmaster, a second board, or a PPS input.

- **The user's next hardware** (their choice of order; GPS grandmaster recommended first):
  - **A GPS PPS + Intel NIC time server** (linuxptp with hardware stamps, `ts2phc` from the PPS).
    Ask: which NIC (i210, i225/i226: which SDP pins take PPS in), how the GPS gives time of day
    (gpsd, NMEA), and run it with clockClass 6, PTP's timescale, `currentUtcOffsetValid`. Tests:
    the board's lock without sartre's software-stamp noise (expect tens of ns), `CLOCK_TAI` from a
    master's valid offset (never yet exercised: `ptp4l` on software stamps is on UTC), holdover
    with its cable pulled, PPS against GPS on a scope.
  - **A second ESP32-P4 board:** two boards electing, hardware stamps both ends; AES67 between
    boards later.
  - **GPS PPS into the board** (the user's idea): GPIO27 (EXT1 pin 6, beside PPS out on GPIO26 at
    pin 4; EXT1 is a footprint: a header or wire to solder), 3.3 V only (a divider for a 5 V PPS),
    GND shared. NMEA later on UEXT's UART0 RX (GPIO38, pin 4) if the board is to be a grandmaster.
    Plan: first an interrupt that stamps the edge with ptpclock0, measured with a jumper GPIO26 to
    GPIO27 (the board stamping its own PPS: the spread is the capture's own error); then, from
    those numbers, a hardware capture (ETM to a timer, as PPS out before chip revision 3) if the
    interrupt's jitter is too much for sub-µs. Not yet checked: what the P4 can latch on an input
    edge. Add the pin to the board definition and `docs/HARDWARE.md`.
- **Known, open:** `clock`'s source stays `ptp` after NTP has the clock back, until NTP steps it
  (NTP slews without setting the source); `PTP_PEROUT_REQUEST` waits for AES67 (§31e).
- **How the runs against `ptp4l` were done** (to do again): sartre (`jth@sartre`, 172.20.0.9,
  `enp8s0`, mlx4, software stamps only, linuxptp 3.1.1 with `pmc`; chrony keeps its clock) can't
  reach the board by ssh, and `ptp4l` needs root, which only the user has. So: a script in
  `~/ptp5` on sartre that the user runs with sudo (`ptp4l -S -s --free_running 1` to follow the
  board, never without `--free_running`: it set sartre's clock 37 s off once; `ptp4l -S
  --priority1 100` to lead; `pmc -u -b 0 'GET TIME_PROPERTIES_DATA_SET'` etc. while it runs;
  phases marked in a file), and a watcher here sampling `ptp` and `clock` on the board over ssh
  every 4 s, switching board settings when the script waits for it (a `go` file). PTP settings on
  the board are set unsaved and reverted after.
