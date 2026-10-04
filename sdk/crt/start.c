/**
 * @file      start.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The program's entry point. The loader calls _start(sys, argc, argv, envp)
 * on the program's own stack; musl's __libc_start_main wants what Linux
 * leaves on a new process's stack: argv, its NULL, the environment, its
 * NULL, then the auxiliary vector. That array is built here, in _start's
 * frame (which lives until exit), and musl does the rest: thread pointer
 * and TLS, stdio, constructors, main, exit.
 */

#include <elf.h>
#include <stddef.h>
#include <stdint.h>
#include <bmds/sys.h>

const bmds_sys_t *bmds_sys;

int  main(int, char **, char **);
void __bmds_linux_start(void);
int __libc_start_main(int (*)(int, char **, char **), int, char **, void (*)(void), void (*)(void),
                      void (*)(void));

/* From the linker script: the TLS template, where its initialized part
 * ends, and where the segment ends. */
extern char __tls_base[], __bmds_tdata_end[], __bmds_tls_end[];

/*
 * musl tests the weak _DYNAMIC to find a dynamic program; undefined, the
 * test is a PC-relative reference to absolute 0, which the loader can't
 * relocate. Defined (and with no PT_DYNAMIC entry to go with it), the
 * program is static, as it is.
 */
__attribute__((visibility("hidden"))) const size_t _DYNAMIC[1];

static Elf32_Phdr    tls_header;
static unsigned char random_bytes[16];

__attribute__((section(".text.bmds_start"), used)) int _start(const bmds_sys_t *sys, int argc, char **argv,
                                                              char **envp) {
        bmds_sys = sys;
        __bmds_linux_start();
        int envc = 0;
        while (envp[envc] != NULL) ++envc;
        (void)bmds_random(random_bytes, sizeof(random_bytes));
        /*
         * The alignment: the template's own (the image is 4096-aligned, so its
         * address modulo 4096 is its link address's). musl puts the data at
         * the template's address modulo the alignment from tp; any larger
         * value would move it from where the code expects it.
         */
        const uintptr_t base = (uintptr_t)__tls_base;
        const uintptr_t align = (base & 4095) != 0 ? (base & -base) : 4096;
        tls_header.p_type = PT_TLS;
        tls_header.p_vaddr = (Elf32_Addr)base;
        tls_header.p_filesz = (Elf32_Word)((uintptr_t)__bmds_tdata_end - base);
        tls_header.p_memsz = (Elf32_Word)((uintptr_t)__bmds_tls_end - base);
        tls_header.p_align = (Elf32_Word)align;
        const size_t aux[] = {
                AT_PHDR,   (size_t)&tls_header, AT_PHENT, sizeof(tls_header), AT_PHNUM, 1,
                AT_PAGESZ, 4096,                AT_RANDOM, (size_t)random_bytes, AT_HWCAP, 0,
                AT_SECURE, 0,                   AT_EXECFN, (size_t)argv[0],      AT_NULL,  0,
        };
        char *vector[argc + 1 + envc + 1 + sizeof(aux) / sizeof(aux[0])];
        int   k = 0;
        for (int i = 0; i < argc; ++i) vector[k++] = argv[i];
        vector[k++] = NULL;
        for (int i = 0; i < envc; ++i) vector[k++] = envp[i];
        vector[k++] = NULL;
        for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) vector[k++] = (char *)aux[i];
        return __libc_start_main(main, argc, vector, NULL, NULL, NULL);
}
