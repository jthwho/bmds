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
| 2 | **lwIP.** Vendored; `Netif` glue; DHCP or static from sysconf (live); ARP, ICMP, IGMP and the MAC filters; `lo`; the shell's `ping`. Host tests: lwIP builds on the host against a loopback netif. **Done** (§16). | Ping both ways, DHCP lease, `net` shows addresses and groups; flood ping without leaks |
| 3 | **Sockets.** `Socket` as a `Stream`; the slots; `linux.c` (sockets, ioctls); the `/etc` pseudo filesystem; DNS (§18). | BusyBox `nc`, `wget`, `telnet` and `ping` work; `poll` over sockets; throughput measured (TCP both ways) |
| 3a | **The tray's network icon** (§17). **Done** (§20). `NetStatus` (public kernel API, change-driven), the SDK's calls for it; `NetworkStatus` in the panel's tray; `panel/tray/network`. | Unplugging and replugging the cable, DHCP and static changes and `enabled` off show in the icon and the tooltip at once, with no timer running |
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
      pool server showed 2-5 ms of jitter, which makes that too noisy.)
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
    (`LWIP_MDNS_SEARCH` 0).
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
