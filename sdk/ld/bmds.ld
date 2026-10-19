/*
 * bmds.ld: the BMDS program layout (see bmds/program.h).
 *
 * Copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One PT_LOAD segment linked at 0 holds everything; the relocations are
 * kept (--emit-relocs) for the kernel, which loads the image at any
 * 4096-byte-aligned address in PSRAM. Link with: -static -nostdlib
 * -Wl,--emit-relocs,--no-relax -T bmds.ld (as BmdsProgram.cmake does).
 *
 * Set the stack a program needs with -Wl,--defsym=__bmds_stack_size=N
 * (bytes; 0, the default, means BMDS_DEFAULT_STACK_SIZE), and its flags
 * (BMDS_PROGRAM_*) with -Wl,--defsym=__bmds_program_flags=N.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

PROVIDE(__bmds_stack_size = 0);
PROVIDE(__bmds_program_flags = 0);

PHDRS
{
        image PT_LOAD FLAGS(7);  /* read, write, execute: one image */
        note PT_NOTE;
        tls PT_TLS;
}

SECTIONS
{
        . = 0;

        .text : {
                KEEP(*(.text.bmds_start))
                *(.text .text.* .gnu.linkonce.t.*)
        } :image

        /* The program note: Elf32_Nhdr, "BMDS", bmds_program_note_t. */
        .note.bmds : ALIGN(4) {
                LONG(5)                 /* namesz: "BMDS" and its NUL */
                LONG(16)                /* descsz */
                LONG(1)                 /* type: BMDS_NOTE_PROGRAM */
                BYTE(0x42) BYTE(0x4d) BYTE(0x44) BYTE(0x53) BYTE(0) BYTE(0) BYTE(0) BYTE(0)
                LONG(16)                /* size */
                LONG(1)                 /* abi_version: BMDS_ABI_VERSION */
                LONG(__bmds_stack_size) /* stack_size */
                LONG(__bmds_program_flags) /* flags: BMDS_PROGRAM_* */
        } :image :note

        .rodata : {
                *(.rodata .rodata.* .gnu.linkonce.r.* .srodata .srodata.*)
        } :image

        .preinit_array : {
                PROVIDE_HIDDEN(__preinit_array_start = .);
                KEEP(*(.preinit_array))
                PROVIDE_HIDDEN(__preinit_array_end = .);
        } :image
        .init_array : {
                PROVIDE_HIDDEN(__init_array_start = .);
                KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
                KEEP(*(.init_array))
                PROVIDE_HIDDEN(__init_array_end = .);
        } :image
        .fini_array : {
                PROVIDE_HIDDEN(__fini_array_start = .);
                KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
                KEEP(*(.fini_array))
                PROVIDE_HIDDEN(__fini_array_end = .);
        } :image

        /* No GOT is expected (no position-independent code); kept visible. */
        .got : { *(.got.plt) *(.got) } :image

        .data : {
                *(.data .data.* .gnu.linkonce.d.* .sdata .sdata.*)
        } :image

        /*
         * Thread-local storage: the template musl copies into each thread's
         * block (the startup code describes it as a PT_TLS entry).
         * .tbss takes no room by itself; .tbss_space reserves it.
         * __tls_base is where the TLS segment starts: .tdata's address, or
         * .tbss's when there is no .tdata (the linker drops an empty .tdata
         * and .tbss doesn't start where .tdata's alignment would have).
         */
        .tdata : ALIGN(16) {
                *(.tdata .tdata.* .gnu.linkonce.td.*)
        } :image :tls
        .tbss : {
                *(.tbss .tbss.* .gnu.linkonce.tb.*)
                *(.tcommon)
        } :image :tls
        PROVIDE_HIDDEN(__tls_base = SIZEOF(.tdata) > 0 ? ADDR(.tdata) : ADDR(.tbss));
        /*
         * Where the initialized part and the whole segment end, for the
         * startup code (musl wants them as a PT_TLS entry). Defined here,
         * after both sections: naming them earlier in the script reorders
         * the output sections, and the linker computes thread-local
         * offsets from the first TLS section it has.
         */
        PROVIDE_HIDDEN(__bmds_tdata_end = SIZEOF(.tdata) > 0 ? ADDR(.tdata) + SIZEOF(.tdata) : ADDR(.tbss));
        PROVIDE_HIDDEN(__bmds_tls_end = ADDR(.tbss) + SIZEOF(.tbss));
        .tbss_space (NOLOAD) : {
                . = ADDR(.tbss) + SIZEOF(.tbss);
        } :image

        .bss (NOLOAD) : ALIGN(16) {
                PROVIDE_HIDDEN(__bss_start = .);
                *(.sbss .sbss.* .bss .bss.* .gnu.linkonce.b.* COMMON)
                . = ALIGN(16);
                PROVIDE_HIDDEN(__bss_end = .);
        } :image

        PROVIDE_HIDDEN(__bmds_image_end = .);

        /* No unwinding in C programs (C++ is built without exceptions). */
        /DISCARD/ : {
                *(.eh_frame) *(.eh_frame_hdr)
                *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash)
                *(.note.GNU-stack)
                *(.gnu_debuglink)
        }
}
