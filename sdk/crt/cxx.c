/**
 * @file      cxx.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What C++ programs need of a C++ runtime, without libstdc++ (sdk/README.md, "C++"): the
 * operators new and delete, std::nothrow, pure virtual calls, guards for static locals and
 * __dso_handle. Written in C under the operators' mangled names (ilp32: size_t is unsigned
 * int, "j"), so that libbmds stays a C library; an archive member, so only programs that use
 * them get them. Without exceptions, a failing operator new ends the program with a message.
 */

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* For a static object's destructor (registered with __cxa_atexit, musl's). */
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;

/* std::nothrow (an empty object). */
const char _ZSt7nothrow = 0;

static void out_of_memory(size_t size) {
        static const char message[] = "out of memory (operator new)\n";
        (void)size;
        (void)write(2, message, sizeof(message) - 1);
        abort();
}

static void *allocate(size_t size) {
        void *p = malloc(size != 0 ? size : 1);
        if (p == NULL) out_of_memory(size);
        return p;
}

static void *allocate_aligned(size_t size, size_t align) {
        if (align < sizeof(void *)) align = sizeof(void *);
        void *p = NULL;
        if (posix_memalign(&p, align, size != 0 ? size : 1) != 0) out_of_memory(size);
        return p;
}

/* operator new(size_t), new[](size_t) */
void *_Znwj(size_t size) {
        return allocate(size);
}
void *_Znaj(size_t size) {
        return allocate(size);
}
/* operator new(size_t, const std::nothrow_t &), new[] */
void *_ZnwjRKSt9nothrow_t(size_t size, const void *tag) {
        (void)tag;
        return malloc(size != 0 ? size : 1);
}
void *_ZnajRKSt9nothrow_t(size_t size, const void *tag) {
        (void)tag;
        return malloc(size != 0 ? size : 1);
}
/* operator new(size_t, std::align_val_t), new[] */
void *_ZnwjSt11align_val_t(size_t size, size_t align) {
        return allocate_aligned(size, align);
}
void *_ZnajSt11align_val_t(size_t size, size_t align) {
        return allocate_aligned(size, align);
}
/* operator delete(void *), delete[], with a size, with std::nothrow, aligned */
void _ZdlPv(void *p) {
        free(p);
}
void _ZdaPv(void *p) {
        free(p);
}
void _ZdlPvj(void *p, size_t size) {
        (void)size;
        free(p);
}
void _ZdaPvj(void *p, size_t size) {
        (void)size;
        free(p);
}
void _ZdlPvRKSt9nothrow_t(void *p, const void *tag) {
        (void)tag;
        free(p);
}
void _ZdaPvRKSt9nothrow_t(void *p, const void *tag) {
        (void)tag;
        free(p);
}
void _ZdlPvSt11align_val_t(void *p, size_t align) {
        (void)align;
        free(p);
}
void _ZdaPvSt11align_val_t(void *p, size_t align) {
        (void)align;
        free(p);
}
void _ZdlPvjSt11align_val_t(void *p, size_t size, size_t align) {
        (void)size;
        (void)align;
        free(p);
}
void _ZdaPvjSt11align_val_t(void *p, size_t size, size_t align) {
        (void)size;
        (void)align;
        free(p);
}

void __cxa_pure_virtual(void) {
        static const char message[] = "pure virtual function called\n";
        (void)write(2, message, sizeof(message) - 1);
        abort();
}

void __cxa_deleted_virtual(void) {
        static const char message[] = "deleted virtual function called\n";
        (void)write(2, message, sizeof(message) - 1);
        abort();
}

/*
 * Guards for static locals (the Itanium C++ ABI's): the guard's first byte is set once the
 * object is made (the compiler's inline check reads it); the first word's bit 8 while a
 * thread makes it. Another thread arriving meanwhile yields until it is done (rare, short).
 */
#define GUARD_DONE 1u
#define GUARD_BUSY 0x100u

int __cxa_guard_acquire(uint32_t *guard) {
        for (;;) {
                uint32_t state = __atomic_load_n(guard, __ATOMIC_ACQUIRE);
                if ((state & GUARD_DONE) != 0) return 0;
                if ((state & GUARD_BUSY) == 0 && __atomic_compare_exchange_n(guard, &state, state | GUARD_BUSY, 0,
                                                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                        return 1;
                sched_yield();
        }
}

void __cxa_guard_release(uint32_t *guard) {
        __atomic_store_n(guard, GUARD_DONE, __ATOMIC_RELEASE);
        return;
}

void __cxa_guard_abort(uint32_t *guard) {
        __atomic_store_n(guard, 0, __ATOMIC_RELEASE);
        return;
}
