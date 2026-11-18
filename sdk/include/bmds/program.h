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
 * - Optionally, icons (appended 2026-10-06): notes with the name "BMDS",
 *   type `BMDS_NOTE_ICON` and a `bmds_icon_note_t` followed by the image as
 *   their descriptor, one note a size, in a section `.note.bmds.icon` that
 *   isn't allocated (not loaded: the kernel reads it from the file when
 *   something shows the program, a launcher or a window's title bar).
 *   `bmds_add_program(... ICONS a.png b.png)` adds them.
 * - Optionally, a GNU build-id (appended 2026-11-15): the note the linker's
 *   `--build-id` makes (name "GNU", type 3), in the PT_NOTE segment. It
 *   changes nothing about loading; a fault's report names the build by it,
 *   and its debug information is found by it (devplan/debuginfo.md).
 *   `bmds_add_program` and `bmds-cc` add it.
 * - Optionally, a patch list in place of the relocations (appended
 *   2026-11-18, devplan/programs.md §20): a program whose note has
 *   `BMDS_PROGRAM_PATCH_LIST` holds, in a section that isn't allocated
 *   (`.note.bmds.patches`), one note with the name "BMDS", type
 *   `BMDS_NOTE_PATCHES` and a `bmds_patches_note_t` followed by `count`
 *   words: each the image offset of a word the kernel patches, with
 *   `BMDS_PATCH_UPPER` set for an upper immediate (`R_RISCV_HI20`'s patch),
 *   in increasing order. The kernel applies exactly these, as it would the
 *   relocations they came from, and reads no relocation or symbol table:
 *   such a file needs neither (`sdk/tools/bmds-pack` makes one from a
 *   program, checking every relocation as the kernel would). The section
 *   headers stay.
 * - The entry point is `_start` (see bmds/sys.h for the startup state).
 * - Code uses the medany model (PC-relative addressing); libraries built
 *   for medlow (absolute `lui` addressing, like the toolchain's prebuilt libraries)
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

/** @brief The note type of an icon (in `.note.bmds.icon`, not loaded). */
#define BMDS_NOTE_ICON 2u

/** @brief An icon's image is a PNG file (8-bit, not interlaced; RGBA recommended). */
#define BMDS_ICON_FORMAT_PNG 1u

/** @brief Stack the kernel gives a program whose note asks for 0. */
#define BMDS_DEFAULT_STACK_SIZE (32u * 1024u)

/** @brief The alignment of the address the kernel loads a program at. */
#define BMDS_LOAD_ALIGN 4096u

/** @brief Largest stack a program may ask for. */
#define BMDS_MAX_STACK_SIZE (1024u * 1024u)

/**
 * @brief A program flag (appended 2026-10-18): it draws to the screen continuously (a game, a
 *        demo), so the kernel starts it on the core the display's own work isn't on.
 *        `bmds_add_program(... DISPLAY)`, or `BMDS_DISPLAY=1` for bmds-cc, sets it.
 */
#define BMDS_PROGRAM_DISPLAY 0x1u

/**
 * @brief A program flag (appended 2026-11-18): its patches are a list (`BMDS_NOTE_PATCHES`),
 *        not relocations. `sdk/tools/bmds-pack` sets it.
 */
#define BMDS_PROGRAM_PATCH_LIST 0x2u

/** @brief The note type of a program's patch list (in `.note.bmds.patches`, not loaded). */
#define BMDS_NOTE_PATCHES 3u

/** @brief In a patch list's word: the patch is an upper immediate (`lui`), not a data word. */
#define BMDS_PATCH_UPPER 0x80000000u

/** @brief What a program says about itself (the note's descriptor). */
typedef struct bmds_program_note {
                uint32_t size;        /**< sizeof(bmds_program_note_t) as built. */
                uint32_t abi_version; /**< The `BMDS_ABI_VERSION` it was built against. */
                uint32_t stack_size;  /**< Stack it needs, its system calls' too; 0: `BMDS_DEFAULT_STACK_SIZE`. */
                uint32_t flags;       /**< `BMDS_PROGRAM_*` (unknown bits are refused). */
} bmds_program_note_t;

/**
 * @brief An icon (a `BMDS_NOTE_ICON` note's descriptor): this header, then
 *        `length` bytes of the image. 16 x 16 and 32 x 32 are the sizes shown.
 */
typedef struct bmds_icon_note {
                uint32_t size;   /**< sizeof(bmds_icon_note_t) as built: where the image starts. */
                uint16_t width;  /**< Pixels. */
                uint16_t height; /**< Pixels. */
                uint32_t format; /**< `BMDS_ICON_FORMAT_PNG`; another is skipped. */
                uint32_t length; /**< Bytes of the image after the header. */
} bmds_icon_note_t;

/**
 * @brief A patch list (a `BMDS_NOTE_PATCHES` note's descriptor): this header,
 *        then `count` 32-bit words, from `size` bytes in.
 */
typedef struct bmds_patches_note {
                uint32_t size;  /**< sizeof(bmds_patches_note_t) as built: where the words start. */
                uint32_t count; /**< Words that follow. */
} bmds_patches_note_t;

#ifdef __cplusplus
}
#endif

#endif /* BMDS_PROGRAM_H */
