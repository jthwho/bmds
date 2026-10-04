/**
 * @file      process.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/errno.h>
#include <bmds/memory.h>
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

namespace {

        constexpr uint32_t KnownMemFlags = BMDS_MEM_FAST | BMDS_MEM_DMA;

        constexpr size_t roundUp(size_t value, size_t to) {
                return (value + to - 1) / to * to;
        }

        MemSpace spaceFor(uint32_t flags) {
                const bool fast = (flags & BMDS_MEM_FAST) != 0;
                const bool dma = (flags & BMDS_MEM_DMA) != 0;
                if (fast) return dma ? MemSpace::InternalDma : MemSpace::Internal;
                return dma ? MemSpace::PsramDma : MemSpace::Psram;
        }

} // namespace

Process::Process(const FdTable &fds) : _fds(fds) {}

Process::~Process() {
        release();
}

void Process::release() {
        _fds.closeAll();
        while (_blocks != nullptr) {
                Block *b = _blocks;
                _blocks = b->next;
                Memory::free(reinterpret_cast<uint8_t *>(b + 1) - b->offset);
        }
        _allocatedBytes = 0;
        _allocations = 0;
        Memory::free(_argBlock);
        _argBlock = nullptr;
        _argv = nullptr;
        _envp = nullptr;
        _argc = 0;
        return;
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
        Memory::free(_argBlock);
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
        auto [n, err] = h->read(WritableByteSpan(static_cast<uint8_t *>(buf), size), WaitForever);
        if (err == Error::EndOfFile) return 0;
        if (err.isError()) return errnoFor(err);
        return static_cast<int32_t>(n);
}

int32_t Process::write(int32_t fd, const void *buf, uint32_t size) {
        FileHandle *h = _fds.handle(fd);
        if (h == nullptr) return -BMDS_EBADF;
        if (size > INT32_MAX) size = INT32_MAX;
        auto [n, err] = h->write(ByteSpan(static_cast<const uint8_t *>(buf), size), WaitForever);
        // Part of it written, then an error: report what was written.
        if (err.isError() && n == 0) return errnoFor(err);
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

BMDS_NAMESPACE_END
