---
theme: vga
paginate: true
footer: BMDS · Bare Metal Do Stuff
transition: wipe
duration: 25m
---

<!-- _class: title -->
# BMDS

## Bare Metal Do Stuff

A DOS-like operating system for the ESP32-P4

<!--
This deck is presented by Preso, a program on BMDS, from a Markdown file on the SD card.
Started over SSH, these notes are on the laptop and the slides on the HDMI screen.
-->

---

# The board

Olimex ESP32-P4-PC, about the price of a lunch:

- 2 RISC-V cores at 360 MHz
- 32 MB of PSRAM, 768 KB of fast SRAM
- 16 MB of flash
- HDMI, 4 USB ports, Ethernet, an SD card, audio
- no radio, no MMU worth the name

<!--
The P4 has no Wi-Fi or Bluetooth. Its MMU maps flash and PSRAM with one table for both cores
and no write protection: there is no memory protection between programs, as on DOS.
-->

---

# The idea: DOS, not Linux

* One user, a prompt, files on a card
* Type a program's name and it runs
* The program gets (nearly) the whole machine
* The OS provides services, then gets out of the way

> Plug in a monitor, a keyboard and a mouse,
> and program the machine on the machine.

---

# Goals

* **A PC-like experience:** boot to a prompt in seconds
* **Separately built programs**, loaded from the card
* **Efficient to the bone:** idle means idle
* **Don't break userspace:** a program that runs keeps running
* **Self-hosting:** edit, compile and run on the device

---

# The layers

![script w:576 h:224](anim/layers.as)

<!--
Each Next adds a layer, from the hardware up. FreeRTOS, as ESP-IDF ships it, is the kernel:
tasks and scheduling. BMDS owns the drivers and everything above them.
-->

---

# Drivers: chosen one by one

| Driver | Built on |
|---|---|
| SD card | IDF's SDMMC host |
| USB keyboards, mice, pads | TinyUSB's host |
| HDMI | the DSI bridge, our own compositor |
| Audio | the ES8311 codec over I2S |
| Ethernet | IDF's EMAC, our own TCP/IP use |

Each choice and its cost are written down in `devplan/`.

---

# Programs

A program is an ELF built with the SDK, on the card. It calls the system through one table:

```c
static inline int32_t bmds_write(int32_t fd,
        const void *buf, uint32_t size) {
        return ((bmds_sys_write_fn)
                bmds_sys->slot[BMDS_SYS_WRITE])(fd, buf, size);
}
```

musl is its C library: BusyBox, Dropbear and Doom build as they are.

---

# Don't break userspace

The table only grows. Every number is locked by the build:

```c
_Static_assert(BMDS_SURFACE_IMAGE == 6 &&
               BMDS_SURFACE_BLIT == 7,
               "ABI: surface_control ops");
```

A test that fails is never edited to pass.

---

# Efficiency is correctness

* No polling where an interrupt or a notification works
* Every allocation names its memory: SRAM or PSRAM
* No allocation in interrupts or hot paths
* Copies by the 2D-DMA, pixels by the vector unit

This deck sleeps between slides: nothing runs until a key.

---

# A desktop

* A window manager and a panel
* Terminals, a file browser, a text editor
* All of them ordinary programs, on the public GUI API
* SSH in, copy files with sftp, mount with sshfs

---

<!-- run: luckyduck -->
# Lucky Duck

A card game, written for BMDS.

<!--
A live demo: stepping onto this slide starts Lucky Duck, and Preso closes its screen to give it
the memory. Closing the game brings the deck back on the next slide.
-->

---

# Scripting for everyone

`angel`: AngelScript with the whole system, and an IDE in the spirit of QBasic.

```angel run
int f = 1;
for (int n = 1; n <= 6; n++) {
        f *= n;
        println("" + n + "! = " + f);
}
```

---

# This talk

* A Markdown file, presented by Preso
* Its diagrams are scripts:

![script w:576 h:150](anim/quicksort.as)

<!-- Next sorts the bars: each swap is the algorithm's own, played as it happens. -->

---

# Next: self-hosting

* A C compiler on the device: TinyCC, ported to 32-bit RISC-V
* An assembler and a linker for the SDK's programs
* Then BMDS builds its own programs

---

<!-- _class: lead -->
# Thank you

Questions?
