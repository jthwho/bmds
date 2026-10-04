/**
 * @file      program.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The BMDS executable format: a RISC-V ELF file the kernel recognizes as
 * a program. It is linked at address 0 and keeps its relocations; the
 * kernel loads it at any 4096-byte-aligned address and moves it there by
 * patching its absolute references, as DOS did with .EXE files.
 *
 * - ELFCLASS32, little endian, EM_RISCV, ET_EXEC, single-float ABI
 *   (`EF_RISCV_FLOAT_ABI_SINGLE`), no RVE.
 * - Exactly one PT_LOAD segment, at virtual address 0, holding the whole
 *   image (code, data, bss). No PT_INTERP, no PT_DYNAMIC.
 * - A PT_NOTE with the name "BMDS", type `BMDS_NOTE_PROGRAM` and a
 *   `bmds_program_note_t` as its descriptor. A file without it is not a
 *   BMDS program.
 * - Section headers, the symbol table (SHT_SYMTAB) and the relocation
 *   sections (SHT_RELA, linked with --emit-relocs) of every allocated
 *   section are kept. `strip --strip-unneeded` (what the SDK does to the
 *   file that goes on the card) and `--strip-debug` keep them; a full
 *   `strip` doesn't, and the file no longer loads.
 * - Relocations in allocated sections, at load address B (a multiple of
 *   4096): R_RISCV_32 adds B to the word; R_RISCV_HI20 adds B >> 12 to the
 *   instruction's upper immediate (the low 12 bits of B are zero, so the
 *   matching LO12 parts are unchanged). Both are left alone when their
 *   symbol is absolute or undefined (weak). The PC-relative, thread-local
 *   (local exec) and label-difference kinds, and the linker's hints
 *   (RELAX, ALIGN), need nothing. Any other kind, and a PC-relative upper
 *   part (PCREL_HI20) against an absolute or undefined symbol that the
 *   linker didn't turn into a `lui`, makes the file unloadable.
 * - The entry point is `_start` (see bmds/sys.h for the startup state).
 * - Code uses the medany model (PC-relative addressing); libraries built
 *   for medlow (absolute `lui` addressing, like the toolchain's picolibc)
 *   work too, through R_RISCV_HI20.
 *
 * The SDK's linker script and flags (sdk/ld/bmds.ld, BmdsProgram.cmake)
 * produce exactly this.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_PROGRAM_H
#define BMDS_PROGRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The note's name. */
#define BMDS_NOTE_NAME "BMDS"

/** @brief The note type of a program's description. */
#define BMDS_NOTE_PROGRAM 1u

/** @brief Stack the kernel gives a program whose note asks for 0. */
#define BMDS_DEFAULT_STACK_SIZE (32u * 1024u)

/** @brief The alignment of the address the kernel loads a program at. */
#define BMDS_LOAD_ALIGN 4096u

/** @brief Largest stack a program may ask for. */
#define BMDS_MAX_STACK_SIZE (1024u * 1024u)

/** @brief What a program says about itself (the note's descriptor). */
typedef struct bmds_program_note {
                uint32_t size;        /**< sizeof(bmds_program_note_t) as built. */
                uint32_t abi_version; /**< The `BMDS_ABI_VERSION` it was built against. */
                uint32_t stack_size;  /**< Bytes of stack it needs; 0 for `BMDS_DEFAULT_STACK_SIZE`. */
                uint32_t flags;       /**< None defined yet: must be 0 (unknown bits are refused). */
} bmds_program_note_t;

#ifdef __cplusplus
}
#endif

#endif /* BMDS_PROGRAM_H */
