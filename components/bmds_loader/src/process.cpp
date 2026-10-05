/**
 * @file      process.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/errno.h>
#include <bmds/interrupt.h>
#include <bmds/lineeditor.h>
#include <bmds/memory.h>
#include <bmds/pipe.h>
#include <bmds/process.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

// Before every allocation the program gets: links for release(), and what
// is needed to free and resize it.
struct Process::Block {
                Block   *prev;
                Block   *next;
                uint32_t size;   // bytes the program asked for
                uint32_t offset; // from the raw allocation to the program's pointer (just past this)
};

// Console input with BMDS_TERM_LINE: the editor, the line it gave (with
// its '\n') until the program has read it all, and what was typed after
// the line in the same read.
struct Process::LineInput {
                explicit LineInput(Console &console) : editor(console) {}

                LineEditor editor;
                char       line[LineEditor::LineMax + 1];
                uint16_t   linePos = 0;
                uint16_t   lineLength = 0;
                uint8_t    ahead[32];
                uint8_t    aheadPos = 0;
                uint8_t    aheadLength = 0;
};

namespace {

        constexpr uint32_t KnownMemFlags = BMDS_MEM_FAST | BMDS_MEM_DMA;
        constexpr uint32_t KnownOpenFlags = BMDS_O_READ | BMDS_O_WRITE | BMDS_O_CREATE | BMDS_O_TRUNC | BMDS_O_APPEND |
                                            BMDS_O_EXCL | BMDS_O_CLOEXEC;
        constexpr uint32_t KnownTermFlags =
                BMDS_TERM_SIGNALS | BMDS_TERM_ONLCR | BMDS_TERM_COLORS | BMDS_TERM_LOGS | BMDS_TERM_LINE;

        constexpr size_t roundUp(size_t value, size_t to) {
                return (value + to - 1) / to * to;
        }

        MemSpace spaceFor(uint32_t flags) {
                const bool fast = (flags & BMDS_MEM_FAST) != 0;
                const bool dma = (flags & BMDS_MEM_DMA) != 0;
                if (fast) return dma ? MemSpace::InternalDma : MemSpace::Internal;
                return dma ? MemSpace::PsramDma : MemSpace::Psram;
        }

        // Copies the fields of @p full that fit in the caller's struct (its
        // first member, `size`, says how big it is), leaving `size` alone.
        template <typename T> int32_t copyOut(const T &full, T *out) {
                if (out == nullptr) return -BMDS_EFAULT;
                const uint32_t size = out->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                const size_t n = size < sizeof(T) ? size : sizeof(T);
                std::memcpy(reinterpret_cast<uint8_t *>(out) + sizeof(uint32_t),
                            reinterpret_cast<const uint8_t *>(&full) + sizeof(uint32_t), n - sizeof(uint32_t));
                return 0;
        }

        bmds_stat_t statFor(const FileInfo &info) {
                bmds_stat_t st = {};
                switch (info.type) {
                        case FileType::File: st.type = BMDS_TYPE_FILE; break;
                        case FileType::Directory: st.type = BMDS_TYPE_DIRECTORY; break;
                        case FileType::Device: st.type = BMDS_TYPE_DEVICE; break;
                        case FileType::Link: st.type = BMDS_TYPE_LINK; break;
                }
                st.attributes = info.attributes;
                st.file_size = info.size;
                st.modified_ns = info.modified.ns;
                st.file_id = info.id;
                st.device = info.device;
                return st;
        }

        // A filesystem call's error: a path too long once joined to the
        // current directory is ENAMETOOLONG, not ERANGE.
        int32_t fsErrno(Error err) {
                return err == Error::OutOfRange ? -BMDS_ENAMETOOLONG : Process::errnoFor(err);
        }

        uint32_t colorCount(ColorDepth depth) {
                switch (depth) {
                        case ColorDepth::None: return 0;
                        case ColorDepth::Ansi16: return 16;
                        case ColorDepth::Ansi256: return 256;
                        case ColorDepth::TrueColor: return 1u << 24;
                }
                return 0;
        }

} // namespace

Process::Process(const FdTable &fds, Console *console) : _fds(fds), _console(console) {}

Process::~Process() {
        release();
}

Process *Process::create(const FdTable &fds, Console *console) {
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(Process));
        if (memory == nullptr) return nullptr;
        return new (memory) Process(fds, console);
}

Process *Process::createChild() const {
        Process *child = create(_fds, _console);
        if (child == nullptr) return nullptr;
        if (child->setCwd(_cwd.view()).isError()) {
                destroy(child);
                return nullptr;
        }
        child->_lineInput = _lineInput;
        child->_signals.inherit(_signals);
        return child;
}

void Process::destroy(Process *process) {
        if (process == nullptr) return;
        process->~Process();
        Memory::free(process);
        return;
}

// What a program leaves behind but its fds and arguments.
void Process::releaseProgram() {
        for (Directory &d : _dirs) d.close();
        if (_line != nullptr) {
                _line->~LineInput();
                Memory::free(_line);
                _line = nullptr;
        }
        _lineInput = true;
        while (_blocks != nullptr) {
                Block *b = _blocks;
                _blocks = b->next;
                Memory::free(reinterpret_cast<uint8_t *>(b + 1) - b->offset);
        }
        _allocatedBytes = 0;
        _allocations = 0;
        return;
}

Error Process::prepareExec(Span<const StringView> args, Span<const StringView> environment) {
        const Error err = setArguments(args, environment);
        if (err.isError()) return err;
        releaseProgram();
        _fds.closeOnExec();
        _signals.resetForExec();
        return Error::Ok;
}

void Process::release() {
        _fds.closeAll();
        releaseProgram();
        Memory::free(_argBlock);
        _argBlock = nullptr;
        _argv = nullptr;
        _envp = nullptr;
        _argc = 0;
        return;
}

Error Process::setCwd(StringView cwd) {
        String copy;
        Error  err = copy.append(cwd);
        if (err.isError()) return err;
        _cwd = std::move(copy);
        return Error::Ok;
}

int Process::openDirectories() const {
        int n = 0;
        for (const Directory &d : _dirs) n += d.isOpen() ? 1 : 0;
        return n;
}

Vfs::Context Process::vfs() const {
        return Vfs::Context{cwd(), _console};
}

bool Process::isConsole(const FileHandle *h) const {
        return _console != nullptr && h != nullptr && &h->stream() == static_cast<Stream *>(_console);
}

// A path from the program: NUL-terminated, at most BMDS_PATH_MAX bytes.
int32_t Process::path(const char *path, StringView &out) const {
        if (path == nullptr) return -BMDS_EFAULT;
        const size_t n = strnlen(path, BMDS_PATH_MAX + 1);
        if (n > BMDS_PATH_MAX) return -BMDS_ENAMETOOLONG;
        if (n == 0) return -BMDS_ENOENT;
        out = StringView(path, n);
        return 0;
}

Error Process::setArguments(Span<const StringView> args, Span<const StringView> environment) {
        constexpr size_t MaxCount = 4096;
        constexpr size_t MaxBytes = 256 * 1024;
        if (args.size() > MaxCount || environment.size() > MaxCount) return Error::Invalid;
        size_t text = 0;
        for (StringView a : args) text += a.size() + 1;
        for (StringView e : environment) text += e.size() + 1;
        if (text > MaxBytes) return Error::Invalid;
        const size_t pointers = (args.size() + 1 + environment.size() + 1) * sizeof(char *);
        auto        *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, pointers + text));
        if (block == nullptr) return Error::NoMem;
        // The old block goes last: an exec's arguments may point into it.
        void *old = _argBlock;
        _argBlock = block;
        char **slots = reinterpret_cast<char **>(block);
        char  *out = reinterpret_cast<char *>(block + pointers);
        auto   copy = [&](StringView s) {
                char *start = out;
                std::memcpy(out, s.data(), s.size());
                out[s.size()] = '\0';
                out += s.size() + 1;
                return start;
        };
        _argv = slots;
        for (size_t i = 0; i < args.size(); ++i) _argv[i] = copy(args[i]);
        _argv[args.size()] = nullptr;
        _envp = slots + args.size() + 1;
        for (size_t i = 0; i < environment.size(); ++i) _envp[i] = copy(environment[i]);
        _envp[environment.size()] = nullptr;
        _argc = static_cast<int32_t>(args.size());
        Memory::free(old);
        return Error::Ok;
}

int32_t Process::errnoFor(Error err) {
        switch (err.code()) {
                case Error::Ok: return 0;
                case Error::Invalid: return -BMDS_EINVAL;
                case Error::NoMem: return -BMDS_ENOMEM;
                case Error::Timeout: return -BMDS_ETIMEDOUT;
                case Error::WouldBlock: return -BMDS_EAGAIN;
                case Error::Busy: return -BMDS_EBUSY;
                case Error::NotExist:
                case Error::NotFound: return -BMDS_ENOENT;
                case Error::Exists: return -BMDS_EEXIST;
                case Error::NotSupported:
                case Error::NotImplemented: return -BMDS_ENOTSUP;
                case Error::NoSpace:
                case Error::Full: return -BMDS_ENOSPC;
                case Error::OutOfRange: return -BMDS_ERANGE;
                case Error::PermissionDenied: return -BMDS_EACCES;
                case Error::Cancelled: return -BMDS_ECANCELED;
                case Error::BadDescriptor: return -BMDS_EBADF;
                case Error::TooManyOpen: return -BMDS_EMFILE;
                case Error::TooManyLinks: return -BMDS_ELOOP;
                case Error::IsDir: return -BMDS_EISDIR;
                case Error::NotDir: return -BMDS_ENOTDIR;
                case Error::NotEmpty: return -BMDS_ENOTEMPTY;
                case Error::ReadOnly: return -BMDS_EROFS;
                case Error::Interrupted: return -BMDS_EINTR;
                case Error::BrokenPipe: return -BMDS_EPIPE;
                case Error::NoMedia: return -BMDS_ENOMEDIUM;
                default: return -BMDS_EIO;
        }
}

int32_t Process::read(int32_t fd, void *buf, uint32_t size) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        if (size == 0) return 0;
        if (size > INT32_MAX) size = INT32_MAX;
        if (h->canRead() && isConsole(h)) return readConsole(buf, size);
        auto [n, err] = h->read(WritableByteSpan(static_cast<uint8_t *>(buf), size), WaitForever);
        if (err == Error::EndOfFile) return 0;
        if (err.isError()) return errnoFor(err);
        return static_cast<int32_t>(n);
}

// The console: lines from the editor with BMDS_TERM_LINE, else bytes as
// they come (after anything left over from line mode).
int32_t Process::readConsole(void *buf, uint32_t size) {
        auto *out = static_cast<uint8_t *>(buf);
        if (_line != nullptr && _line->linePos < _line->lineLength) {
                LineInput     &l = *_line;
                const uint32_t left = static_cast<uint32_t>(l.lineLength - l.linePos);
                const uint32_t n = left < size ? left : size;
                std::memcpy(out, l.line + l.linePos, n);
                l.linePos = static_cast<uint16_t>(l.linePos + n);
                return static_cast<int32_t>(n);
        }
        if (!_lineInput) {
                if (_line != nullptr && _line->aheadPos < _line->aheadLength) {
                        LineInput     &l = *_line;
                        const uint32_t left = static_cast<uint32_t>(l.aheadLength - l.aheadPos);
                        const uint32_t n = left < size ? left : size;
                        std::memcpy(out, l.ahead + l.aheadPos, n);
                        l.aheadPos = static_cast<uint8_t>(l.aheadPos + n);
                        return static_cast<int32_t>(n);
                }
                auto [n, err] = _console->read(WritableByteSpan(out, size), WaitForever);
                if (err == Error::EndOfFile) return 0;
                if (err.isError()) return errnoFor(err);
                return static_cast<int32_t>(n);
        }
        if (_line == nullptr) {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(LineInput));
                if (memory == nullptr) return -BMDS_ENOMEM;
                _line = new (memory) LineInput(*_console);
        }
        LineInput &l = *_line;
        l.editor.start(StringView());
        for (;;) {
                if (l.aheadPos == l.aheadLength) {
                        auto [n, err] = _console->read(WritableByteSpan(l.ahead, sizeof(l.ahead)), WaitForever);
                        if (err == Error::EndOfFile) return 0;
                        if (err.isError()) return errnoFor(err);
                        l.aheadPos = 0;
                        l.aheadLength = static_cast<uint8_t>(n);
                }
                switch (l.editor.feed(static_cast<char>(l.ahead[l.aheadPos++]))) {
                        case LineEditor::Event::Line: {
                                const StringView line = l.editor.line();
                                if (!line.isEmpty()) l.editor.addHistory(line); // up-arrow recalls it
                                std::memcpy(l.line, line.data(), line.size());
                                l.line[line.size()] = '\n';
                                l.linePos = 0;
                                l.lineLength = static_cast<uint16_t>(line.size() + 1);
                                return readConsole(buf, size);
                        }
                        case LineEditor::Event::Eof: return 0;
                        case LineEditor::Event::Cancel: l.editor.start(StringView()); break;
                        case LineEditor::Event::None: break;
                }
        }
}

int32_t Process::write(int32_t fd, const void *buf, uint32_t size) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        if (size > INT32_MAX) size = INT32_MAX;
        auto [n, err] = h->write(ByteSpan(static_cast<const uint8_t *>(buf), size), WaitForever);
        // Part of it written, then an error: report what was written.
        if (err.isError() && n == 0) {
                // Nobody will read it: SIGPIPE, which ends the program unless
                // it is caught or ignored.
                if (err == Error::BrokenPipe) (void)_signals.post(BMDS_SIGPIPE, BMDS_SI_KERNEL);
                return errnoFor(err);
        }
        return static_cast<int32_t>(n);
}

int32_t Process::close(int32_t fd) {
        if (!_fds.isOpen(fd)) return -BMDS_EBADF;
        return errnoFor(_fds.close(fd));
}

int32_t Process::seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        Stream::SeekFrom from = Stream::SeekFrom::Start;
        switch (whence) {
                case BMDS_SEEK_SET: break;
                case BMDS_SEEK_CUR: from = Stream::SeekFrom::Current; break;
                case BMDS_SEEK_END: from = Stream::SeekFrom::End; break;
                default: return -BMDS_EINVAL;
        }
        auto [pos, err] = h->stream().seek(offset, from);
        if (err == Error::NotSupported) return -BMDS_ESPIPE;
        if (err.isError()) return errnoFor(err);
        if (position != nullptr) *position = static_cast<int64_t>(pos);
        return 0;
}

int32_t Process::isatty(int32_t fd) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        return h->stream().isTerminal() ? 1 : 0;
}

void *Process::memAlloc(uint32_t size, uint32_t align, uint32_t flags) {
        if ((flags & ~KnownMemFlags) != 0) return nullptr;
        if (align == 0) align = BMDS_MEM_ALIGN;
        if ((align & (align - 1)) != 0 || align > MaxAlign) return nullptr;
        if (align < BMDS_MEM_ALIGN) align = BMDS_MEM_ALIGN;
        // The header goes in front, in padding that keeps the program's
        // pointer aligned.
        const size_t pad = roundUp(sizeof(Block), align);
        if (size > SIZE_MAX - pad) return nullptr;
        auto *raw = static_cast<uint8_t *>(Memory::allocAligned(spaceFor(flags), size + pad, align));
        if (raw == nullptr) return nullptr;
        uint8_t *user = raw + pad;
        Block   *b = reinterpret_cast<Block *>(user) - 1;
        b->size = size;
        b->offset = static_cast<uint32_t>(pad); // user - raw
        b->prev = nullptr;
        b->next = _blocks;
        if (_blocks != nullptr) _blocks->prev = b;
        _blocks = b;
        _allocatedBytes += size;
        ++_allocations;
        return user;
}

void Process::memFree(void *ptr) {
        if (ptr == nullptr) return;
        Block *b = static_cast<Block *>(ptr) - 1;
        if (b->prev != nullptr) {
                b->prev->next = b->next;
        } else {
                _blocks = b->next;
        }
        if (b->next != nullptr) b->next->prev = b->prev;
        _allocatedBytes -= b->size;
        --_allocations;
        Memory::free(reinterpret_cast<uint8_t *>(b + 1) - b->offset);
        return;
}

void *Process::memRealloc(void *ptr, uint32_t size, uint32_t flags) {
        if (ptr == nullptr) return memAlloc(size, 0, flags);
        if ((flags & ~KnownMemFlags) != 0) return nullptr;
        const Block *old = static_cast<Block *>(ptr) - 1;
        void        *p = memAlloc(size, 0, flags);
        if (p == nullptr) return nullptr;
        std::memcpy(p, ptr, old->size < size ? old->size : size);
        memFree(ptr);
        return p;
}

int32_t Process::open(const char *path, uint32_t flags) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        if ((flags & ~KnownOpenFlags) != 0 || (flags & (BMDS_O_READ | BMDS_O_WRITE)) == 0) return -BMDS_EINVAL;
        uint8_t vfsFlags = 0;
        if (flags & BMDS_O_READ) vfsFlags |= Vfs::Read;
        if (flags & BMDS_O_WRITE) vfsFlags |= Vfs::Write;
        if (flags & BMDS_O_CREATE) vfsFlags |= Vfs::Create;
        if (flags & BMDS_O_TRUNC) vfsFlags |= Vfs::Truncate;
        if (flags & BMDS_O_APPEND) vfsFlags |= Vfs::Append;
        if (flags & BMDS_O_EXCL) vfsFlags |= Vfs::Exclusive;
        auto [file, err] = Vfs::open(p, vfsFlags, vfs());
        if (err.isError()) return fsErrno(err);
        auto [fd, aerr] = _fds.add(std::move(file));
        if (aerr.isError()) return -BMDS_EMFILE;
        if (flags & BMDS_O_CLOEXEC) (void)_fds.setCloseOnExec(fd, true);
        return fd;
}

int32_t Process::pipe(int32_t *fds, uint32_t flags) {
        if (fds == nullptr) return -BMDS_EFAULT;
        if ((flags & ~BMDS_O_CLOEXEC) != 0) return -BMDS_EINVAL;
        auto [ends, err] = Pipe::create();
        if (err.isError()) return errnoFor(err);
        auto [readFd, rerr] = _fds.add(std::move(ends.read));
        if (rerr.isError()) return -BMDS_EMFILE;
        auto [writeFd, werr] = _fds.add(std::move(ends.write));
        if (werr.isError()) {
                (void)_fds.close(readFd);
                return -BMDS_EMFILE;
        }
        if (flags & BMDS_O_CLOEXEC) {
                (void)_fds.setCloseOnExec(readFd, true);
                (void)_fds.setCloseOnExec(writeFd, true);
        }
        fds[0] = readFd;
        fds[1] = writeFd;
        return 0;
}

int32_t Process::fdFlags(int32_t fd, int32_t flags) {
        if (!_fds.isOpen(fd)) return -BMDS_EBADF;
        if (flags != -1) {
                if ((flags & ~static_cast<int32_t>(BMDS_FD_CLOEXEC)) != 0) return -BMDS_EINVAL;
                (void)_fds.setCloseOnExec(fd, (flags & BMDS_FD_CLOEXEC) != 0);
        }
        return _fds.isCloseOnExec(fd) ? static_cast<int32_t>(BMDS_FD_CLOEXEC) : 0;
}

int32_t Process::lstat(const char *path, bmds_stat_t *info) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        if (info == nullptr) return -BMDS_EFAULT;
        auto [fi, err] = Vfs::lstat(p, vfs());
        if (err.isError()) return fsErrno(err);
        return copyOut(statFor(fi), info);
}

int32_t Process::symlink(const char *target, const char *path) {
        StringView t;
        StringView p;
        if (int32_t r = this->path(target, t); r < 0) return r;
        if (int32_t r = this->path(path, p); r < 0) return r;
        const Error err = Vfs::symlink(t, p, vfs());
        return err == Error::Invalid ? -BMDS_EINVAL : fsErrno(err);
}

int32_t Process::readlink(const char *path, char *buf, uint32_t size) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        if (buf == nullptr && size != 0) return -BMDS_EFAULT;
        auto [target, err] = Vfs::readLink(p, vfs());
        if (err == Error::Invalid) return -BMDS_EINVAL;
        if (err.isError()) return fsErrno(err);
        const StringView t = target.view();
        if (size != 0) {
                // Cut at a character boundary, as readdir's names.
                size_t len = t.size();
                if (len > size - 1) {
                        len = size - 1;
                        while (len > 0 && (static_cast<uint8_t>(t[len]) & 0xC0) == 0x80) --len;
                }
                std::memcpy(buf, t.data(), len);
                buf[len] = '\0';
        }
        return static_cast<int32_t>(t.size());
}

int32_t Process::stat(const char *path, bmds_stat_t *info) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        if (info == nullptr) return -BMDS_EFAULT;
        auto [fi, err] = Vfs::stat(p, vfs());
        if (err.isError()) return fsErrno(err);
        return copyOut(statFor(fi), info);
}

int32_t Process::fstat(int32_t fd, bmds_stat_t *info) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        bmds_stat_t st = {};
        st.type = BMDS_TYPE_DEVICE;
        st.open_flags = (h->canRead() ? BMDS_O_READ : 0) | (h->canWrite() ? BMDS_O_WRITE : 0);
        st.file_id = h->id();
        st.device = h->device();
        // A stream that seeks is a file: its size is where its end is.
        Stream &stream = h->stream();
        auto [here, err] = stream.seek(0, Stream::SeekFrom::Current);
        if (err.isOk()) {
                auto [end, eerr] = stream.seek(0, Stream::SeekFrom::End);
                if (eerr.isOk()) {
                        st.type = BMDS_TYPE_FILE;
                        st.file_size = end;
                }
                (void)stream.seek(static_cast<int64_t>(here), Stream::SeekFrom::Start);
        }
        return copyOut(st, info);
}

int32_t Process::mkdir(const char *path) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        return fsErrno(Vfs::mkdir(p, vfs()));
}

int32_t Process::rmdir(const char *path) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        return fsErrno(Vfs::rmdir(p, vfs()));
}

int32_t Process::unlink(const char *path) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        return fsErrno(Vfs::remove(p, vfs()));
}

int32_t Process::rename(const char *from, const char *to) {
        StringView f, t;
        if (int32_t r = path(from, f); r < 0) return r;
        if (int32_t r = path(to, t); r < 0) return r;
        // The VFS's NotSupported: not within one filesystem. EXDEV, so a
        // `mv` knows to copy instead.
        const Error err = Vfs::rename(f, t, vfs());
        return err == Error::NotSupported ? -BMDS_EXDEV : fsErrno(err);
}

int32_t Process::opendir(const char *path) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        int slot = 0;
        while (slot < MaxDirectories && _dirs[slot].isOpen()) ++slot;
        if (slot == MaxDirectories) return -BMDS_EMFILE;
        auto [dir, err] = Vfs::openDirectory(p, vfs());
        if (err.isError()) return fsErrno(err);
        _dirs[slot] = std::move(dir);
        return slot;
}

int32_t Process::readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size) {
        if (dir < 0 || dir >= MaxDirectories || !_dirs[dir].isOpen()) return -BMDS_EBADF;
        if (name == nullptr && size != 0) return -BMDS_EFAULT;
        if (info != nullptr && info->size < sizeof(uint32_t)) return -BMDS_EINVAL;
        auto [fi, err] = _dirs[dir].next();
        if (err == Error::EndOfFile) return 0;
        if (err.isError()) return errnoFor(err);
        if (info != nullptr) (void)copyOut(statFor(fi), info);
        const StringView n = fi.name.view();
        if (size != 0) {
                // Cut a name that doesn't fit at a character boundary.
                size_t len = n.size();
                if (len > size - 1) {
                        len = size - 1;
                        while (len > 0 && (static_cast<uint8_t>(n[len]) & 0xC0) == 0x80) --len;
                }
                std::memcpy(name, n.data(), len);
                name[len] = '\0';
        }
        return static_cast<int32_t>(n.size());
}

int32_t Process::closedir(int32_t dir) {
        if (dir < 0 || dir >= MaxDirectories || !_dirs[dir].isOpen()) return -BMDS_EBADF;
        _dirs[dir].close();
        return 0;
}

int32_t Process::getcwd(char *buf, uint32_t size) {
        const StringView c = cwd();
        if (buf == nullptr) return -BMDS_EFAULT;
        if (size < c.size() + 1) return -BMDS_ERANGE;
        std::memcpy(buf, c.data(), c.size());
        buf[c.size()] = '\0';
        return static_cast<int32_t>(c.size());
}

int32_t Process::chdir(const char *path) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        // The physical path: links along it followed once, here.
        auto [real, err] = Vfs::realPath(p, vfs());
        if (err.isError()) return fsErrno(err);
        auto [fi, serr] = Vfs::stat(real.view(), vfs());
        if (serr.isError()) return fsErrno(serr);
        if (!fi.isDirectory()) return -BMDS_ENOTDIR;
        _cwd = std::move(real);
        return 0;
}

int32_t Process::dup(int32_t fd, int32_t minFd) {
        if (!_fds.isOpen(fd)) return -BMDS_EBADF;
        if (minFd < 0 || minFd >= FdTable::MaxFds) return -BMDS_EINVAL;
        auto [n, err] = _fds.dup(fd, minFd);
        if (err.isError()) return -BMDS_EMFILE;
        return n;
}

int32_t Process::dup2(int32_t fd, int32_t to) {
        if (!_fds.isOpen(fd) || to < 0 || to >= FdTable::MaxFds) return -BMDS_EBADF;
        (void)_fds.dup2(fd, to);
        return to;
}

int32_t Process::clockGet(uint32_t clock, int64_t *ns) {
        if (ns == nullptr) return -BMDS_EFAULT;
        switch (clock) {
                case BMDS_CLOCK_REALTIME: *ns = Clock::now().ns; return 0;
                case BMDS_CLOCK_MONOTONIC: *ns = Clock::monotonicNs(); return 0;
                default: return -BMDS_EINVAL;
        }
}

int32_t Process::sleep(int64_t ns) {
        // Ticks count from wherever the current one is: sleep to a deadline
        // on the clock, so the program gets at least what it asked for.
        constexpr int64_t NsPerMs = 1'000'000;
        constexpr int64_t MaxMs = UINT32_MAX - 1;
        Interrupt        &interrupt = _signals.interrupt();
        if (ns <= 0) return interrupt.isRaised() ? -BMDS_EINTR : 0;
        const int64_t end = Clock::monotonicNs() + ns;
        for (;;) {
                const int64_t left = end - Clock::monotonicNs();
                if (left <= 0) return 0;
                const int64_t ms = left / NsPerMs + (left % NsPerMs != 0 ? 1 : 0);
                const Error   err = interrupt.sleep(static_cast<unsigned>(ms < MaxMs ? ms : MaxMs));
                if (err.isError()) return errnoFor(err);
        }
}

int32_t Process::interrupted() {
        return (_signals.pending() & Signals::bit(BMDS_SIGINT)) != 0 ? 1 : 0;
}

int32_t Process::termGet(int32_t fd, bmds_term_t *term) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        if (!isConsole(h)) return -BMDS_ENOTTY;
        const Console::TermMode mode = _console->mode();
        const Console::Size     cells = _console->size();
        bmds_term_t             t = {};
        t.flags = (mode.signals ? BMDS_TERM_SIGNALS : 0) | (mode.nlToCrNl ? BMDS_TERM_ONLCR : 0) |
                  (mode.colors ? BMDS_TERM_COLORS : 0) | (mode.logs ? BMDS_TERM_LOGS : 0) |
                  (_lineInput ? BMDS_TERM_LINE : 0);
        t.columns = cells.columns;
        t.rows = cells.rows;
        t.colors = colorCount(_console->colorDepth());
        return copyOut(t, term);
}

int32_t Process::termSet(int32_t fd, uint32_t flags) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        if (!isConsole(h)) return -BMDS_ENOTTY;
        if ((flags & ~KnownTermFlags) != 0) return -BMDS_EINVAL;
        Console::TermMode mode;
        mode.signals = (flags & BMDS_TERM_SIGNALS) != 0;
        mode.nlToCrNl = (flags & BMDS_TERM_ONLCR) != 0;
        mode.colors = (flags & BMDS_TERM_COLORS) != 0;
        mode.logs = (flags & BMDS_TERM_LOGS) != 0;
        _console->setMode(mode);
        _lineInput = (flags & BMDS_TERM_LINE) != 0;
        return 0;
}

int32_t Process::memInfo(uint32_t flags, bmds_mem_info_t *info) {
        if ((flags & ~KnownMemFlags) != 0) return -BMDS_EINVAL;
        if (info == nullptr) return -BMDS_EFAULT;
        const MemStats  stats = Memory::stats(spaceFor(flags));
        bmds_mem_info_t m = {};
        auto            clamp = [](size_t v) {
                return static_cast<uint32_t>(v < UINT32_MAX ? v : UINT32_MAX);
        };
        m.total_bytes = clamp(stats.totalBytes);
        m.free_bytes = clamp(stats.freeBytes);
        m.largest_free = clamp(stats.largestFreeBlock);
        m.program_bytes = clamp(_allocatedBytes);
        m.program_blocks = clamp(_allocations);
        return copyOut(m, info);
}

int32_t Process::setMtime(const char *path, int64_t ns) {
        StringView p;
        if (int32_t r = this->path(path, p); r < 0) return r;
        return fsErrno(Vfs::setModified(p, Timestamp{ns}, vfs()));
}

int32_t Process::ftruncate(int32_t fd, int64_t size) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr || !h->canWrite()) return -BMDS_EBADF;
        if (size < 0) return -BMDS_EINVAL;
        const Error err = h->stream().truncate(static_cast<uint64_t>(size));
        if (err == Error::NotSupported) return -BMDS_EINVAL; // a pipe or a terminal
        return fsErrno(err);
}

BMDS_NAMESPACE_END
