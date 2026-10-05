/**
 * @file      process.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fdtable.h>
#include <bmds/memspace.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/sys.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

class Console;
class Interrupt;

/**
 * @brief A running program's resources and its system calls.
 *
 * Everything a program gets from the kernel belongs to its Process: its
 * file descriptors (a copy of the shell's, so closing one doesn't close
 * the shell's), the memory it allocates, its directory handles, its
 * current directory, its argument and environment block. When the program
 * ends, however it ends, `release()` returns all of it, so a program can't
 * leak past its exit.
 *
 * The console (the shell's terminal) is where `/dev/console` leads and
 * what the terminal calls work on. With `BMDS_TERM_LINE` (the default),
 * reading it gets whole lines, echoed and editable (the line editor the
 * shell's `read` uses); else the bytes as they arrive.
 *
 * The methods are the system calls behind the SDK's table (bmds/sys.h):
 * same arguments, results >= 0 or a negated `BMDS_E...` number.
 *
 * @par Thread Safety
 * A program has one thread, so a Process is used by its program's task
 * only (and by the loader before it starts and after it ends).
 */
class Process {
        public:
                /** @brief Largest alignment `memAlloc()` accepts. */
                static constexpr uint32_t MaxAlign = 4096;

                /** @brief How many directories a program may have open at once. */
                static constexpr int MaxDirectories = 8;

                /**
                 * @brief Constructs a process with its own copy of @p fds, in `/`.
                 * @param console   The terminal (`/dev/console`, the terminal calls); may be null.
                 * @param interrupt Ctrl-C (`interrupted()`, `sleep()`); may be null.
                 */
                explicit Process(const FdTable &fds, Console *console = nullptr, Interrupt *interrupt = nullptr);
                ~Process();
                Process(const Process &) = delete;
                Process &operator=(const Process &) = delete;

                /**
                 * @brief Creates a vfork child's Process (`Psram`): a copy of this
                 *        one's fds (with their flags), directory, console and
                 *        interrupt; no memory, directories or arguments.
                 * @return The child (free it with `destroy()`), or null (no memory).
                 */
                Process *createChild() const;

                /** @brief Releases and frees a Process from `createChild()` or `create()`. */
                static void destroy(Process *process);

                /** @brief Creates a Process in `Psram` (see the constructor); null if no memory. */
                static Process *create(const FdTable &fds, Console *console, Interrupt *interrupt);

                /**
                 * @brief Prepares for a new program (an exec): the argument block
                 *        is rebuilt from @p args and @p environment (which may point
                 *        into the old one or into this process's memory), then the
                 *        memory, directories and line input of the old program are
                 *        freed and the close-on-exec fds closed. Its fds, directory
                 *        and terminal stay.
                 * @return `Ok`, or as `setArguments()` (nothing changed then).
                 */
                Error prepareExec(Span<const StringView> args, Span<const StringView> environment);

                /** @brief Released when one of its children ends (its waits). */
                Semaphore &childEvent() { return _childEvent; }

                /** @brief Returns its console (may be null). */
                Console *console() const { return _console; }

                /** @brief Returns its interrupt (may be null). */
                Interrupt *interrupt() const { return _interrupt; }

                /**
                 * @brief Builds the argument block: `argv` (with its NULL) and `envp`
                 *        ("NAME=value" strings, NULL-terminated), in one allocation.
                 * @return `Ok`, `NoMem`, or `Invalid` (too many or too long).
                 */
                Error setArguments(Span<const StringView> args, Span<const StringView> environment);

                /**
                 * @brief Sets the current directory (absolute, normalized: the shell's).
                 * @return `Ok` or `NoMem`.
                 */
                Error setCwd(StringView cwd);

                /** @brief Returns the current directory. */
                StringView cwd() const { return _cwd.isEmpty() ? StringView("/") : _cwd.view(); }

                /** @brief Returns the program's `argc`. */
                int32_t argc() const { return _argc; }

                /** @brief Returns its `argv`. */
                char **argv() const { return _argv; }

                /** @brief Returns its `envp`. */
                char **envp() const { return _envp; }

                /** @brief Returns its file descriptors. */
                FdTable &fds() { return _fds; }

                /** @brief Returns the bytes it has allocated and not freed. */
                size_t allocatedBytes() const { return _allocatedBytes; }

                /** @brief Returns how many allocations it holds. */
                size_t allocations() const { return _allocations; }

                /** @brief Returns how many directories it has open. */
                int openDirectories() const;

                /** @brief Closes its fds and directories and frees everything it allocated. Idempotent. */
                void release();

                // --- System calls (bmds/sys.h) ------------------------------------

                int32_t read(int32_t fd, void *buf, uint32_t size);
                int32_t write(int32_t fd, const void *buf, uint32_t size);
                int32_t close(int32_t fd);
                int32_t seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position);
                int32_t isatty(int32_t fd);
                void   *memAlloc(uint32_t size, uint32_t align, uint32_t flags);
                void    memFree(void *ptr);
                void   *memRealloc(void *ptr, uint32_t size, uint32_t flags);
                int32_t open(const char *path, uint32_t flags);
                int32_t stat(const char *path, bmds_stat_t *info);
                int32_t fstat(int32_t fd, bmds_stat_t *info);
                int32_t mkdir(const char *path);
                int32_t rmdir(const char *path);
                int32_t unlink(const char *path);
                int32_t rename(const char *from, const char *to);
                int32_t opendir(const char *path);
                int32_t readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size);
                int32_t closedir(int32_t dir);
                int32_t getcwd(char *buf, uint32_t size);
                int32_t chdir(const char *path);
                int32_t dup(int32_t fd, int32_t minFd);
                int32_t dup2(int32_t fd, int32_t to);
                int32_t clockGet(uint32_t clock, int64_t *ns);
                int32_t sleep(int64_t ns);
                int32_t interrupted();
                int32_t termGet(int32_t fd, bmds_term_t *term);
                int32_t termSet(int32_t fd, uint32_t flags);
                int32_t memInfo(uint32_t flags, bmds_mem_info_t *info);
                int32_t setMtime(const char *path, int64_t ns);
                int32_t ftruncate(int32_t fd, int64_t size);
                int32_t symlink(const char *target, const char *path);
                int32_t readlink(const char *path, char *buf, uint32_t size);
                int32_t lstat(const char *path, bmds_stat_t *info);
                int32_t pipe(int32_t *fds, uint32_t flags);
                int32_t fdFlags(int32_t fd, int32_t flags);

                /** @brief Returns the negated `BMDS_E...` number for @p err. */
                static int32_t errnoFor(Error err);

        private:
                struct Block;     // an allocation's header, just before what the program gets
                struct LineInput; // the line editor and its line, once the program reads the console

                void         releaseProgram();
                Vfs::Context vfs() const;
                bool         isConsole(const FileHandle *h) const;
                int32_t      readConsole(void *buf, uint32_t size);
                int32_t      path(const char *path, StringView &out) const;

                FdTable    _fds;
                Console   *_console;
                Interrupt *_interrupt;
                String     _cwd;
                LineInput *_line = nullptr;
                bool       _lineInput = true;
                Directory  _dirs[MaxDirectories];
                Semaphore  _childEvent{1, 0};
                Block     *_blocks = nullptr;
                size_t     _allocatedBytes = 0;
                size_t     _allocations = 0;
                void      *_argBlock = nullptr;
                char     **_argv = nullptr;
                char     **_envp = nullptr;
                int32_t    _argc = 0;
};

BMDS_NAMESPACE_END
