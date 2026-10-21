/**
 * @file      process.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fdtable.h>
#include <bmds/memspace.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/signals.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/sys.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

class ProgramControls;

class Console;

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
 * reading it gets whole lines, editable: echoed through the line editor
 * the shell's `read` uses, or unseen without `BMDS_TERM_ECHO`. Else it
 * gets the bytes as they arrive, CR as LF with `BMDS_TERM_ICRNL`, echoed
 * with `BMDS_TERM_ECHO`. These modes are the process's (a vfork child
 * starts with its parent's); the rest are the console's. `poll()` sees it
 * as reading does: readable once a read would be served (a whole line in
 * line mode), editing what was typed on the way.
 *
 * The methods are the system calls behind the SDK's table (bmds/sys.h):
 * same arguments, results >= 0 or a negated `BMDS_E...` number.
 *
 * @par Thread Safety
 * The system calls are thread-safe: a program's threads share its Process
 * (devplan/processes.md §3.10). Its descriptor table, memory blocks and
 * directories are under one lock (a call holds a reference to the handle
 * it uses), the working directory under another while a path uses it, and
 * console input is read in turns. The rest (creating, `prepareExec()`,
 * `release()`, the accessors a built-in program uses) is for one thread:
 * the loader's, before the program's threads run or after they have gone.
 */
class Process {
        public:
                /** @brief Largest alignment `memAlloc()` accepts. */
                static constexpr uint32_t MaxAlign = 4096;

                /** @brief How many directories a program may have open at once. */
                static constexpr int MaxDirectories = 8;

                /** @brief The most entries one `poll()` takes. */
                static constexpr uint32_t MaxPollEntries = BMDS_POLL_MAX;

                /** @brief How often `poll()` checks a stream whose wait list was full. */
                static constexpr unsigned int UnwatchedPollMs = 10;

                /**
                 * @brief Constructs a process with its own copy of @p fds, in `/`.
                 * @param console The terminal (`/dev/console`, the terminal calls); may be null.
                 */
                explicit Process(const FdTable &fds, Console *console = nullptr);
                ~Process();
                Process(const Process &) = delete;
                Process &operator=(const Process &) = delete;

                /**
                 * @brief Creates a vfork child's Process (`Psram`): a copy of this
                 *        one's fds (with their flags), directory, console and
                 *        signal actions, and the mask of @p thread (the one that
                 *        vforks); no memory, directories, arguments or pending signals.
                 * @return The child (free it with `destroy()`), or null (no memory).
                 */
                Process *createChild(const Signals::Thread &thread);

                /** @brief Releases and frees a Process from `createChild()` or `create()`. */
                static void destroy(Process *process);

                /** @brief Creates a Process in `Psram` (see the constructor); null if no memory. */
                static Process *create(const FdTable &fds, Console *console);

                /**
                 * @brief Prepares for a new program (an exec): the argument block
                 *        is rebuilt from @p args and @p environment (which may point
                 *        into the old one or into this process's memory), then the
                 *        memory, directories and line input of the old program are
                 *        freed, the close-on-exec fds closed and caught signals
                 *        reset to their default. Its fds, directory, terminal,
                 *        ignored signals, mask and pending signals stay.
                 * @return `Ok`, or as `setArguments()` (nothing changed then).
                 */
                Error prepareExec(Span<const StringView> args, Span<const StringView> environment);

                /** @brief Released when one of its children ends (its waits). */
                Semaphore &childEvent() { return _childEvent; }

                /** @brief Returns its console: its controlling terminal (may be null). */
                Console *console() const { return _console.load(std::memory_order_acquire); }

                /**
                 * @brief Makes @p console (may be null) its terminal (`TIOCSCTTY`,
                 *        `TIOCNOTTY`): the console counts it as a user, the old one
                 *        no longer does, and input the old one's line editor held is
                 *        dropped. Its fds are unchanged.
                 */
                void setConsole(Console *console);

                /** @brief Returns its signals (whose interrupt its blocking calls wake on). */
                Signals &signals() { return _signals; }

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

                /**
                 * @brief Returns the program's actions (`devplan/controls.md` §7c), made on first
                 *        use (null when PSRAM is out); dropped when the program ends or is replaced.
                 *        Not referenced for the caller: valid while the program runs.
                 */
                ProgramControls *controls();

                /** @brief Returns the program's actions if made (`controls()`), else null. */
                ProgramControls *existingControls() const { return _controls.load(std::memory_order_acquire); }

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
                /**
                 * @brief Returns true when @p bytes from @p ptr lie inside one allocation of
                 *        `memAlloc()` (a walk of them: for a check made once, `BMDS_SURFACE_IMAGE`).
                 */
                bool    owns(const void *ptr, size_t bytes) const;
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
                /** @brief A clock's time: no lock, no wait, nothing of the process's (a system call without `KernelCall`). */
                static int32_t clockGet(uint32_t clock, int64_t *ns);
                int32_t sleep(int64_t ns);
                /** @brief `sleep()` for a real-time thread: woken by `WakeTimer`, not the tick. */
                int32_t sleepPrecise(int64_t ns);
                int32_t interrupted();
                int32_t termGet(int32_t fd, bmds_term_t *term);
                int32_t termSet(int32_t fd, uint32_t flags);

                /** @brief The terminal @p fd is (this process's console): 0, or `-BMDS_EBADF`, `-BMDS_ENOTTY`. */
                int32_t terminalOf(int32_t fd, Console *&out) const;
                int32_t memInfo(uint32_t flags, bmds_mem_info_t *info);
                int32_t setMtime(const char *path, int64_t ns);
                int32_t fsInfo(const char *path, bmds_fs_info_t *info);
                int32_t ftruncate(int32_t fd, int64_t size);
                int32_t symlink(const char *target, const char *path);
                int32_t readlink(const char *path, char *buf, uint32_t size);

                /** @brief `file_type()`: what @p path is (`Vfs::identify()`), its type into @p out. */
                int32_t fileType(const char *path, uint32_t flags, char *out, uint32_t size);

                /** @name The system configuration (`sysconf_*`, `devplan/sysconf.md` §12.2) */
                ///@{
                int32_t sysconfFind(const char *key);
                int32_t sysconfGet(int32_t id, uint32_t field, char *out, uint32_t size);
                int32_t sysconfInfo(int32_t id, bmds_sysconf_info_t *info);
                int32_t sysconfSet(int32_t id, uint32_t op, const char *value);
                int32_t sysconfNext(const char *prefix, int32_t after);
                int32_t sysconfDeclare(const bmds_sysconf_param_t *param);
                int32_t sysconfWatch(const char *prefix, uint32_t flags);

                /** @brief `sync()`: `Vfs::sync()`, its error as an errno. */
                int32_t sync();
                ///@}
                int32_t lstat(const char *path, bmds_stat_t *info);
                int32_t pipe(int32_t *fds, uint32_t flags);
                int32_t fdFlags(int32_t fd, int32_t flags);
                int32_t poll(bmds_pollfd_t *fds, uint32_t count, int32_t timeoutMs);

                /** @brief Returns the negated `BMDS_E...` number for @p err. */
                static int32_t errnoFor(Error err);

                /**
                 * @brief Returns the error a program's negated `BMDS_E...` number stands for
                 *        (`errnoFor()`'s inverse; `IoError` for one with none).
                 */
                static Error errorFor(int32_t error);

                /** @brief Returns a reference to @p fd's handle, or none (for calls on a kind of fd). */
                FileHandle::Ptr handle(int32_t fd) const { return handleOf(fd); }

                /**
                 * @brief Gives @p handle the lowest free fd, close-on-exec if @p flags
                 *        has `BMDS_O_CLOEXEC` (for calls that make a kind of fd).
                 * @return The fd, or `-BMDS_EMFILE`.
                 */
                int32_t addHandle(FileHandle::Ptr handle, uint32_t flags);

                /**
                 * @brief Makes the program's @p path absolute (against its current
                 *        directory, normalized) into @p out, for calls that keep a path.
                 * @return 0, or `-BMDS_EFAULT`, `-BMDS_ENAMETOOLONG`, `-BMDS_ENOENT`
                 *         (empty), `-BMDS_ENOMEM`.
                 */
                int32_t absolutePath(const char *path, String &out);

        private:
                struct Block;     // an allocation's header, just before what the program gets
                struct LineInput; // the line editor and its line, once the program reads the console

                void         releaseProgram();
                Vfs::Context vfs() const;
                bool         isConsole(const FileHandle *h) const;
                int32_t      readConsole(void *buf, uint32_t size);
                int32_t      pumpConsole(bool wait);
                uint8_t      readiness(FileHandle &h);
                int32_t      rawRead(uint8_t *out, int32_t n);
                int32_t      path(const char *path, StringView &out) const;
                FileHandle::Ptr handleOf(int32_t fd) const; // a reference to fd's handle, or none

                mutable Mutex _lock;              // the fds, the memory blocks, the directories
                Mutex         _pathLock;          // _cwd while a path call uses it
                Semaphore     _consoleTurn{1, 1}; // the console's input (_line and the modes' use of it)

                FdTable    _fds;
                std::atomic<Console *> _console;
                std::atomic<ProgramControls *> _controls{nullptr};
                Signals    _signals;
                String     _cwd;
                LineInput *_line = nullptr;
                bool       _lineInput = true;
                bool       _echo = true;
                bool       _crToNl = true;
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
