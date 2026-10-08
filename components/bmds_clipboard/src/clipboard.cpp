/**
 * @file      clipboard.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The clipboard service (devplan/clipboard.md §4a).
 */

#include <atomic>
#include <cstring>
#include <new>
#include <bmds/clipboard.h>
#include <bmds/inireader.h>
#include <bmds/memory.h>
#include <bmds/pipe.h>
#include <bmds/staticobject.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<Clipboard> clipboards[Clipboard::MaxClipboards];

        // Stored bytes: one PSRAM block, shared by an offer's types and the readers reading it.
        struct ClipBlock {
                        std::atomic<uint32_t> refs{1};
                        uint32_t              size = 0;

                        uint8_t *bytes() { return reinterpret_cast<uint8_t *>(this + 1); }

                        static ClipBlock *make(ByteSpan data) {
                                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipBlock) + data.size());
                                if (mem == nullptr) return nullptr;
                                auto *block = ::new (mem) ClipBlock();
                                block->size = static_cast<uint32_t>(data.size());
                                if (!data.isEmpty()) std::memcpy(block->bytes(), data.data(), data.size());
                                return block;
                        }

                        void ref() {
                                refs.fetch_add(1, std::memory_order_relaxed);
                                return;
                        }

                        void unref() {
                                if (refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
                                this->~ClipBlock();
                                Memory::free(this);
                                return;
                        }
        };

        // A reader of a stored block: a file of its bytes (it seeks, so fstat() gives its size).
        class BlockStream final : public Stream {
                public:
                        explicit BlockStream(ClipBlock *block) : _block(block) {}

                        Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override {
                                (void)timeoutMs;
                                if (_position >= _block->size) return makeError<size_t>(Error::EndOfFile);
                                size_t n = _block->size - _position;
                                if (n > buf.size()) n = buf.size();
                                std::memcpy(buf.data(), _block->bytes() + _position, n);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override {
                                (void)data;
                                (void)timeoutMs;
                                return makeError<size_t>(Error::NotSupported);
                        }

                        using Stream::write;

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                int64_t base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(_position);
                                if (from == SeekFrom::End) base = _block->size;
                                const int64_t to = base + offset;
                                if (to < 0) return makeError<uint64_t>(Error::Invalid);
                                _position = static_cast<uint64_t>(to);
                                return makeResult(_position);
                        }

                        void handleClosed(uint8_t mode) override {
                                (void)mode;
                                _block->unref();
                                this->~BlockStream();
                                Memory::free(this);
                                return;
                        }

                private:
                        ~BlockStream() = default;

                        ClipBlock *_block;
                        uint64_t   _position = 0;
        };

        // Opens a reader of @p block (its reference taken over, dropped on failure).
        Result<FileHandle::Ptr> openBlock(ClipBlock *block) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(BlockStream));
                if (mem == nullptr) {
                        block->unref();
                        return makeError<FileHandle::Ptr>(Error::NoMem);
                }
                auto                   *stream = ::new (mem) BlockStream(block);
                Result<FileHandle::Ptr> handle = FileHandle::open(*stream, FileHandle::Read);
                if (handle.error().isError()) stream->handleClosed(FileHandle::Read); // frees it and the reference
                return handle;
        }

        // Copies @p name into @p out (`MaxOwnerName` bytes at most, NUL-ended).
        void copyName(StringView name, char *out) {
                size_t n = name.size() < Clipboard::MaxOwnerName ? name.size() : Clipboard::MaxOwnerName;
                std::memcpy(out, name.data(), n);
                out[n] = '\0';
                return;
        }

} // namespace

// An offer: its types (normal forms) and, for each, a stored block or none (on request).
struct Clipboard::Offer {
                uint32_t   serial = 0;
                Owner      owner;
                bool       live = false; // the owner may be called
                uint8_t    count = 0;
                size_t     held = 0; // its blocks' bytes, each counted once
                char       name[MaxOwnerName + 1] = {};
                char       types[MaxTypes][ClipboardType::BufferBytes] = {};
                ClipBlock *data[MaxTypes] = {};

                StringView type(size_t i) const { return StringView(types[i]); }

                int find(StringView normal) const {
                        for (size_t i = 0; i < count; ++i)
                                if (type(i) == normal) return static_cast<int>(i);
                        return -1;
                }

                size_t heldBytes() const {
                        size_t total = 0;
                        for (size_t i = 0; i < count; ++i) {
                                if (data[i] == nullptr) continue;
                                bool seen = false;
                                for (size_t j = 0; j < i && !seen; ++j) seen = data[j] == data[i];
                                if (!seen) total += data[i]->size;
                        }
                        return total;
                }
};

Clipboard::Clipboard() : _config(MemSpace::Psram) {
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipboardConversions));
        if (mem != nullptr) {
                _conversions = ::new (mem) ClipboardConversions();
                (void)_conversions->addBuiltins();
        }
}

Clipboard::~Clipboard() {
        for (Offer *&offer : _offers) release(std::exchange(offer, nullptr));
        if (_conversions != nullptr) {
                _conversions->~ClipboardConversions();
                Memory::free(_conversions);
        }
}

Result<Clipboard *> Clipboard::create(size_t number) {
        if (number >= MaxClipboards) return makeError<Clipboard *>(Error::NotSupported);
        if (!clipboards[number].isCreated()) clipboards[number].create();
        return makeResult(&*clipboards[number]);
}

Result<Clipboard *> Clipboard::clipboard(size_t number) {
        if (number >= MaxClipboards || !clipboards[number].isCreated()) return makeError<Clipboard *>(Error::NotExist);
        return makeResult(&*clipboards[number]);
}

void Clipboard::setLimit(size_t bytes) {
        Mutex::Locker lock(_lock, WaitForever);
        _limit = bytes;
        return;
}

Error Clipboard::configure(StringView text, Report report, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        Error         err = _config.assign(text);
        if (err.isError()) return err;
        // [Limits] is the system configuration's clipboard/limit now: its lines are reported.
        IniReader ini(_config.view());
        for (IniReader::Item item = ini.next(); item.kind != IniReader::Kind::End; item = ini.next())
                if (item.kind == IniReader::Kind::Entry && item.name.equalsIgnoreCase("Limits") && report != nullptr)
                        report(arg, item.line, item.text);
        if (_conversions != nullptr) {
                *_conversions = ClipboardConversions();
                (void)_conversions->read(_config.view(), report, arg);
                (void)_conversions->addBuiltins();
        }
        return Error::Ok;
}

void Clipboard::release(Offer *offer) {
        if (offer == nullptr) return;
        for (size_t i = 0; i < offer->count; ++i)
                if (offer->data[i] != nullptr) offer->data[i]->unref();
        offer->~Offer();
        Memory::free(offer);
        return;
}

void Clipboard::tellLost(Offer *offer) {
        if (offer == nullptr || !offer->live || offer->owner.lost == nullptr) return;
        Mutex::Locker lock(_callLock, WaitForever);
        offer->owner.lost(offer->owner.arg, offer->serial);
        return;
}

Clipboard::Offer *Clipboard::take(Selection selection, uint32_t serial, Error &err) {
        Offer *&slot = _offers[static_cast<size_t>(selection)];
        if (slot == nullptr || (serial != 0 && slot->serial != serial)) {
                err = slot == nullptr && serial == 0 ? Error::Ok : Error::InvalidState;
                return nullptr;
        }
        err = Error::Ok;
        _stored -= slot->held;
        return std::exchange(slot, nullptr);
}

Result<uint32_t> Clipboard::offer(Selection selection, Span<const StringView> types, const Owner &owner) {
        if (static_cast<size_t>(selection) >= Selections || types.isEmpty() || types.size() > MaxTypes)
                return makeError<uint32_t>(Error::Invalid);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Offer));
        if (mem == nullptr) return makeError<uint32_t>(Error::NoMem);
        auto *made = ::new (mem) Offer();
        for (const StringView type : types) {
                auto [normal, err] = ClipboardType::normalize(
                        type, Span<char>(made->types[made->count], ClipboardType::BufferBytes));
                if (err.isError() || made->find(normal) >= 0) {
                        release(made);
                        return makeError<uint32_t>(Error::Invalid);
                }
                ++made->count;
        }
        made->owner = owner;
        copyName(owner.name, made->name);
        made->owner.name = StringView(made->name);
        made->live = owner.requested != nullptr || owner.lost != nullptr;
        Offer *old = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (++_serial == 0) _serial = 1;
                made->serial = _serial;
                Error err;
                old = take(selection, 0, err);
                _offers[static_cast<size_t>(selection)] = made;
        }
        const uint32_t serial = made->serial;
        tellLost(old);
        release(old);
        return makeResult(serial);
}

Error Clipboard::store(Selection selection, uint32_t serial, StringView type, ByteSpan bytes) {
        if (static_cast<size_t>(selection) >= Selections) return Error::Invalid;
        char normal[ClipboardType::BufferBytes];
        auto [n, nerr] = ClipboardType::normalize(type, Span<char>(normal, sizeof(normal)));
        if (nerr.isError()) return Error::NotFound;
        if (bytes.size() > _limit) return Error::NoSpace;
        ClipBlock *block = ClipBlock::make(bytes);
        if (block == nullptr) return Error::NoMem;
        ClipBlock *old = nullptr;
        Error      err = Error::Ok;
        {
                Mutex::Locker lock(_lock, WaitForever);
                Offer        *o = _offers[static_cast<size_t>(selection)];
                const int     i = o != nullptr && o->serial == serial ? o->find(n) : -2;
                if (i == -2) {
                        err = Error::InvalidState;
                } else if (i < 0) {
                        err = Error::NotFound;
                } else {
                        old = o->data[i];
                        o->data[i] = block;
                        const size_t held = o->heldBytes();
                        if (_stored - o->held + held > _limit) {
                                o->data[i] = old;
                                old = nullptr;
                                err = Error::NoSpace;
                        } else {
                                _stored = _stored - o->held + held;
                                o->held = held;
                                block = nullptr;
                        }
                }
        }
        if (block != nullptr) block->unref();
        if (old != nullptr) old->unref();
        return err;
}

Result<uint32_t> Clipboard::copy(Selection selection, Span<const StringView> types, ByteSpan bytes, StringView name,
                                 int32_t process) {
        if (static_cast<size_t>(selection) >= Selections || types.isEmpty() || types.size() > MaxTypes)
                return makeError<uint32_t>(Error::Invalid);
        if (bytes.size() > _limit) return makeError<uint32_t>(Error::NoSpace);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Offer));
        if (mem == nullptr) return makeError<uint32_t>(Error::NoMem);
        auto *made = ::new (mem) Offer();
        for (const StringView type : types) {
                auto [normal, err] = ClipboardType::normalize(
                        type, Span<char>(made->types[made->count], ClipboardType::BufferBytes));
                if (err.isError() || made->find(normal) >= 0) {
                        release(made);
                        return makeError<uint32_t>(Error::Invalid);
                }
                ++made->count;
        }
        ClipBlock *block = ClipBlock::make(bytes);
        if (block == nullptr) {
                release(made);
                return makeError<uint32_t>(Error::NoMem);
        }
        for (size_t i = 0; i < made->count; ++i) {
                if (i > 0) block->ref();
                made->data[i] = block;
        }
        made->held = bytes.size();
        copyName(name, made->name);
        made->owner.name = StringView(made->name);
        made->owner.process = process;
        Offer *old = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                const Offer  *there = _offers[static_cast<size_t>(selection)];
                if (_stored - (there != nullptr ? there->held : 0) + made->held > _limit) {
                        release(made);
                        return makeError<uint32_t>(Error::NoSpace);
                }
                if (++_serial == 0) _serial = 1;
                made->serial = _serial;
                Error err;
                old = take(selection, 0, err);
                _offers[static_cast<size_t>(selection)] = made;
                _stored += made->held;
        }
        const uint32_t serial = made->serial;
        tellLost(old);
        release(old);
        return makeResult(serial);
}

Error Clipboard::clear(Selection selection, uint32_t serial) {
        if (static_cast<size_t>(selection) >= Selections) return Error::Invalid;
        Offer *old = nullptr;
        Error  err;
        {
                Mutex::Locker lock(_lock, WaitForever);
                old = take(selection, serial, err);
        }
        if (serial == 0) tellLost(old); // someone else cleared it
        release(old);
        return err;
}

Error Clipboard::withdraw(Selection selection, uint32_t serial) {
        if (static_cast<size_t>(selection) >= Selections || serial == 0) return Error::Invalid;
        Offer *gone = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                Offer       *&slot = _offers[static_cast<size_t>(selection)];
                if (slot == nullptr || slot->serial != serial) return Error::InvalidState;
                Offer *o = slot;
                o->live = false;
                // The stored types stay; those on request go.
                size_t kept = 0;
                for (size_t i = 0; i < o->count; ++i) {
                        if (o->data[i] == nullptr) continue;
                        if (kept != i) {
                                std::memcpy(o->types[kept], o->types[i], sizeof(o->types[i]));
                                o->data[kept] = o->data[i];
                                o->data[i] = nullptr;
                        }
                        ++kept;
                }
                o->count = static_cast<uint8_t>(kept);
                if (kept == 0) {
                        Error err;
                        gone = take(selection, serial, err);
                }
        }
        release(gone);
        // A call that began before is over once the call lock is free.
        { Mutex::Locker wait(_callLock, WaitForever); }
        return Error::Ok;
}

Result<FileHandle::Ptr> Clipboard::request(Selection selection, StringView type, char *taken) {
        if (static_cast<size_t>(selection) >= Selections) return makeError<FileHandle::Ptr>(Error::Invalid);
        char normal[ClipboardType::BufferBytes];
        auto [n, nerr] = ClipboardType::normalize(type, Span<char>(normal, sizeof(normal)));
        if (nerr.isError()) return makeError<FileHandle::Ptr>(Error::Invalid);
        if (taken != nullptr) std::memcpy(taken, normal, n.size() + 1);
        uint32_t serial = 0;
        Owner    owner;
        {
                Mutex::Locker lock(_lock, WaitForever);
                const Offer  *o = _offers[static_cast<size_t>(selection)];
                if (o == nullptr) return makeError<FileHandle::Ptr>(Error::Empty);
                const int i = o->find(n);
                if (i < 0) return makeError<FileHandle::Ptr>(Error::NotFound);
                if (ClipBlock *block = o->data[i]) {
                        block->ref();
                        return openBlock(block);
                }
                if (!o->live || o->owner.requested == nullptr) return makeError<FileHandle::Ptr>(Error::NotExist);
                serial = o->serial;
                owner = o->owner;
        }
        // On request: the owner writes into a pipe the caller reads.
        auto [ends, perr] = Pipe::create();
        if (perr.isError()) return makeError<FileHandle::Ptr>(perr);
        Mutex::Locker calls(_callLock, WaitForever);
        {
                // Withdrawn or replaced while the pipe was made: nothing to ask.
                Mutex::Locker lock(_lock, WaitForever);
                const Offer  *o = _offers[static_cast<size_t>(selection)];
                if (o == nullptr || o->serial != serial || !o->live) return makeError<FileHandle::Ptr>(Error::NotExist);
        }
        owner.requested(owner.arg, serial, n, std::move(ends.write));
        return makeResult(std::move(ends.read));
}

void Clipboard::describe(Selection selection, Description &out) const {
        out = Description();
        if (static_cast<size_t>(selection) >= Selections) return;
        Mutex::Locker lock(_lock, WaitForever);
        const Offer  *o = _offers[static_cast<size_t>(selection)];
        if (o == nullptr) return;
        out.serial = o->serial;
        std::memcpy(out.owner, o->name, sizeof(out.owner));
        out.process = o->owner.process;
        out.window = o->owner.window;
        out.count = o->count;
        for (size_t i = 0; i < o->count; ++i) {
                std::memcpy(out.types[i].type, o->types[i], sizeof(out.types[i].type));
                out.types[i].size = o->data[i] != nullptr ? int64_t(o->data[i]->size) : -1;
        }
        return;
}

uint64_t Clipboard::storedTextSize(Selection selection) const {
        if (static_cast<size_t>(selection) >= Selections) return 0;
        Mutex::Locker lock(_lock, WaitForever);
        const Offer  *o = _offers[static_cast<size_t>(selection)];
        const int     i = o != nullptr ? o->find(ClipboardType::Text) : -1;
        return i >= 0 && o->data[i] != nullptr ? o->data[i]->size : 0;
}

size_t Clipboard::storedBytes() const {
        Mutex::Locker lock(_lock, WaitForever);
        return _stored;
}

BMDS_NAMESPACE_END
