/**
 * @file      tmpvolume.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A tmpfs behind the Volume interface (devplan/storage.md §15).
 */

#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/stream.h>

#include "tmpvolume.h"

BMDS_NAMESPACE_BEGIN

/** @brief A file or directory. One `Psram` block; a long name and the data are others. */
struct TmpVolume::Node {
                static constexpr size_t InlineName = 24;

                Node     *parent = nullptr; // null: the root, or removed
                Node     *next = nullptr;   // the next entry of its directory, or of the removed list
                Node     *child = nullptr;  // a directory's first entry
                uint8_t  *bytes = nullptr;  // a small file's data (capacity bytes)
                uint8_t **pages = nullptr;  // a large file's pages (capacity slots; null: a hole)
                uint64_t  size = 0;
                int64_t   modified = 0;
                uint32_t  capacity = 0;
                uint32_t  refs = 0; // open files, readers and directories
                char     *name = inlineName;
                uint16_t  nameBytes = 0;
                FileType  type = FileType::File;
                uint8_t   attributes = 0;
                bool      paged = false;
                bool      removed = false;
                char      inlineName[InlineName] = {};

                StringView nameView() const { return StringView(name, nameBytes); }
};

/** @brief An open directory: its next entry, moved on when that is removed. */
class TmpVolume::DirCursor final : public Volume::Cursor {
        public:
                DirCursor(TmpVolume &owner, Node &directory)
                    : volume(owner), dir(directory), entry(directory.child), generation(owner.generation()) {}

                bool stale() const { return generation != volume.generation(); }

                Error next(FileInfo &info) override {
                        if (stale()) return Error::NoMedia;
                        if (entry == nullptr) return Error::EndOfFile;
                        const Node &n = *entry;
                        entry = n.next;
                        info = FileInfo();
                        const Error err = info.name.assign(n.nameView());
                        if (err.isError()) return err;
                        info.type = n.type;
                        info.size = n.type == FileType::File ? n.size : 0;
                        info.modified = Timestamp{n.modified};
                        info.attributes = n.attributes;
                        return Error::Ok;
                }

                TmpVolume     &volume;
                Node          &dir;
                Node          *entry;
                DirCursor     *nextCursor = nullptr;
                const uint32_t generation;
};

namespace {

        constinit TmpVolume volumes[TmpVolume::MaxVolumes];

        int64_t nowNs() {
                return Clock::now().ns;
        }

        constexpr uint64_t pageCount(uint64_t bytes) {
                return (bytes + TmpVolume::PageBytes - 1) / TmpVolume::PageBytes;
        }

        // A small file's block: its size rounded up to 64 bytes.
        constexpr uint32_t smallCapacity(uint64_t bytes) {
                return static_cast<uint32_t>((bytes + 63) & ~uint64_t(63));
        }

        /**
         * An open file as a Stream (Psram), until its FileHandle closes. Its
         * node stays while it is open, even removed. Files don't block, so
         * the stream timeouts don't apply.
         */
        class TmpFile final : public Stream {
                public:
                        TmpFile(TmpVolume &volume, TmpVolume::Node &node, uint8_t flags, const FileIdentity &identity)
                            : _volume(volume), _node(node), _identity(identity), _generation(volume.generation()),
                              _readable((flags & Vfs::Read) != 0), _writable((flags & Vfs::Write) != 0),
                              _append((flags & Vfs::Append) != 0) {}

                        Result<size_t> read(WritableByteSpan buf, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                if (!_readable) return makeError<size_t>(Error::BadDescriptor);
                                const size_t n = _volume.readAt(_node, _position, buf);
                                if (n == 0 && !buf.isEmpty()) return makeError<size_t>(Error::EndOfFile);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan data, unsigned int) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                if (!_writable) return makeError<size_t>(Error::BadDescriptor);
                                if (_append) _position = TmpVolume::sizeOf(_node);
                                const Result<size_t> n = _volume.writeAt(_node, _position, data);
                                if (n.isOk()) _position += n.value();
                                return n;
                        }

                        using Stream::write;

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return makeError<uint64_t>(Error::NoMedia);
                                const int64_t base = from == SeekFrom::Start     ? 0
                                                     : from == SeekFrom::Current ? int64_t(_position)
                                                                                 : int64_t(TmpVolume::sizeOf(_node));
                                if (offset < -base) return makeError<uint64_t>(Error::Invalid);
                                _position = uint64_t(base + offset);
                                return makeResult(_position);
                        }

                        Error truncate(uint64_t size) override {
                                Mutex::Locker lock(Fs::lock(), WaitForever);
                                if (stale()) return Error::NoMedia;
                                if (!_writable) return Error::BadDescriptor;
                                return _volume.resize(_node, size);
                        }

                        void handleClosed(uint8_t) override {
                                {
                                        Mutex::Locker lock(Fs::lock(), WaitForever);
                                        // A stale file (unmounted) is only freed.
                                        if (!stale()) {
                                                _volume.unref(_node);
                                                if (_writable) Fs::writtenLocked(_identity);
                                        }
                                }
                                this->~TmpFile();
                                Memory::free(this);
                                return;
                        }

                private:
                        bool stale() const { return _generation != _volume.generation(); }

                        TmpVolume         &_volume;
                        TmpVolume::Node   &_node;
                        const FileIdentity _identity; // told at close after writing
                        const uint32_t     _generation;
                        uint64_t           _position = 0;
                        const bool         _readable;
                        const bool         _writable;
                        const bool         _append;
        };

        // A file open for plain reads (Psram).
        class TmpReader final : public Volume::Reader {
                public:
                        TmpReader(TmpVolume &owner, TmpVolume::Node &file)
                            : volume(owner), node(file), generation(owner.generation()) {}

                        bool stale() const { return generation != volume.generation(); }

                        uint64_t size() const override { return stale() ? 0 : TmpVolume::sizeOf(node); }

                        Result<size_t> readAt(uint64_t offset, WritableByteSpan buf) override {
                                if (stale()) return makeError<size_t>(Error::NoMedia);
                                return makeResult(volume.readAt(node, offset, buf));
                        }

                        TmpVolume       &volume;
                        TmpVolume::Node &node;
                        const uint32_t   generation;
        };

} // namespace

TmpVolume *TmpVolume::acquire() {
        for (TmpVolume &v : volumes) {
                if (v._inUse) continue;
                v._inUse = true;
                return &v;
        }
        return nullptr;
}

Error TmpVolume::mount(uint64_t limitBytes) {
        if (_root != nullptr) return Error::InvalidState;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Node));
        if (mem == nullptr) return Error::NoMem;
        _root = ::new (mem) Node();
        _root->type = FileType::Directory;
        _root->modified = nowNs();
        _used = sizeof(Node);
        _limit = limitBytes;
        ++_generation;
        return Error::Ok;
}

void TmpVolume::release() {
        destroyAll();
        _inUse = false;
        return;
}

void *TmpVolume::take(size_t bytes) {
        if (_used + bytes > _limit) return nullptr;
        void *p = Memory::alloc(MemSpace::Psram, bytes);
        if (p != nullptr) _used += bytes;
        return p;
}

void TmpVolume::give(void *p, size_t bytes) {
        if (p == nullptr) return;
        Memory::free(p);
        _used -= bytes;
        return;
}

uint64_t TmpVolume::sizeOf(const Node &node) {
        return node.size;
}

void TmpVolume::destroy(Node *node) {
        if (node->paged) {
                for (uint32_t i = 0; i < node->capacity; ++i) give(node->pages[i], PageBytes);
                give(node->pages, node->capacity * sizeof(uint8_t *));
        } else {
                give(node->bytes, node->capacity);
        }
        if (node->name != node->inlineName) give(node->name, node->nameBytes);
        node->~Node();
        give(node, sizeof(Node));
        return;
}

void TmpVolume::destroyAll() {
        // Every file and directory still open is stale from now on.
        ++_generation;
        _cursors = nullptr;
        // The tree, leaves first, without recursion.
        Node *n = _root;
        while (n != nullptr) {
                if (n->child != nullptr) {
                        n = n->child;
                        continue;
                }
                Node *up = n->parent;
                if (up != nullptr) up->child = n->next;
                destroy(n);
                n = up;
        }
        _root = nullptr;
        while (_removed != nullptr) {
                Node *next = _removed->next;
                destroy(_removed);
                _removed = next;
        }
        _used = 0;
        return;
}

Result<TmpVolume::Node *> TmpVolume::find(StringView path, bool &noPath) const {
        noPath = false;
        Node *n = _root;
        if (n == nullptr) return makeError<Node *>(Error::NoMedia);
        size_t at = 0;
        while (at < path.size()) {
                // "/name" next.
                ++at;
                size_t end = path.indexOf('/', at);
                if (end == StringView::NPos) end = path.size();
                const StringView name = path.mid(at, end - at);
                if (n->type != FileType::Directory) {
                        noPath = true;
                        return makeError<Node *>(Error::NotExist);
                }
                Node *c = n->child;
                while (c != nullptr && c->nameView() != name) c = c->next;
                if (c == nullptr) {
                        noPath = end != path.size();
                        return makeError<Node *>(Error::NotExist);
                }
                n = c;
                at = end;
        }
        return makeResult(n);
}

Error TmpVolume::findParent(StringView path, Node *&parent, StringView &name, bool &noPath) const {
        noPath = false;
        const size_t slash = path.lastIndexOf('/');
        if (slash == StringView::NPos || slash + 1 == path.size()) return Error::Invalid;
        auto [p, err] = find(path.left(slash), noPath);
        if (err.isError()) {
                noPath = true;
                return err;
        }
        if (p->type != FileType::Directory) {
                noPath = true;
                return Error::NotExist;
        }
        parent = p;
        name = path.mid(slash + 1);
        return Error::Ok;
}

Result<TmpVolume::Node *> TmpVolume::create(Node &parent, StringView name, FileType type) {
        if (name.isEmpty() || name.size() > MaxName || name.contains('\0') || name == "." || name == "..")
                return makeError<Node *>(Error::Invalid);
        void *mem = take(sizeof(Node));
        if (mem == nullptr) return makeError<Node *>(Error::NoSpace);
        Node *n = ::new (mem) Node();
        if (name.size() > Node::InlineName) {
                n->name = static_cast<char *>(take(name.size()));
                if (n->name == nullptr) {
                        n->name = n->inlineName;
                        destroy(n);
                        return makeError<Node *>(Error::NoSpace);
                }
        }
        std::memcpy(n->name, name.data(), name.size());
        n->nameBytes = static_cast<uint16_t>(name.size());
        n->type = type;
        n->attributes = type == FileType::File ? FileInfo::Archive : 0;
        n->modified = nowNs();
        n->parent = &parent;
        // At the end: listings come in the order of creation.
        Node **link = &parent.child;
        while (*link != nullptr) link = &(*link)->next;
        *link = n;
        parent.modified = n->modified;
        return makeResult(n);
}

void TmpVolume::unlink(Node &node) {
        Node *parent = node.parent;
        for (DirCursor *c = _cursors; c != nullptr; c = c->nextCursor)
                if (c->entry == &node) c->entry = node.next;
        for (Node **link = &parent->child; *link != nullptr; link = &(*link)->next) {
                if (*link == &node) {
                        *link = node.next;
                        break;
                }
        }
        parent->modified = nowNs();
        node.parent = nullptr;
        node.next = nullptr;
        if (node.refs == 0) {
                destroy(&node);
                return;
        }
        node.removed = true;
        node.next = _removed;
        _removed = &node;
        return;
}

void TmpVolume::unref(Node &node) {
        if (node.refs != 0) --node.refs;
        if (node.refs != 0 || !node.removed) return;
        for (Node **link = &_removed; *link != nullptr; link = &(*link)->next) {
                if (*link == &node) {
                        *link = node.next;
                        break;
                }
        }
        destroy(&node);
        return;
}

size_t TmpVolume::readAt(const Node &node, uint64_t offset, WritableByteSpan buf) const {
        if (offset >= node.size) return 0;
        const uint64_t left = node.size - offset;
        const size_t   n = buf.size() < left ? buf.size() : static_cast<size_t>(left);
        uint8_t       *out = buf.data();
        if (!node.paged) {
                std::memcpy(out, node.bytes + offset, n);
                return n;
        }
        size_t done = 0;
        while (done < n) {
                const uint64_t at = offset + done;
                const uint64_t page = at / PageBytes;
                const uint32_t within = static_cast<uint32_t>(at % PageBytes);
                const size_t   chunk = PageBytes - within < n - done ? PageBytes - within : n - done;
                const uint8_t *p = page < node.capacity ? node.pages[page] : nullptr;
                if (p != nullptr) {
                        std::memcpy(out + done, p + within, chunk);
                } else {
                        std::memset(out + done, 0, chunk); // a hole
                }
                done += chunk;
        }
        return n;
}

Error TmpVolume::makePaged(Node &node) {
        // The block becomes the first page (none for an empty file).
        auto **pages = static_cast<uint8_t **>(take(sizeof(uint8_t *)));
        if (pages == nullptr) return Error::NoSpace;
        pages[0] = nullptr;
        if (node.size != 0) {
                pages[0] = static_cast<uint8_t *>(take(PageBytes));
                if (pages[0] == nullptr) {
                        give(pages, sizeof(uint8_t *));
                        return Error::NoSpace;
                }
                std::memcpy(pages[0], node.bytes, node.size);
                std::memset(pages[0] + node.size, 0, PageBytes - node.size);
        }
        give(node.bytes, node.capacity);
        node.bytes = nullptr;
        node.pages = pages;
        node.capacity = 1;
        node.paged = true;
        return Error::Ok;
}

Result<size_t> TmpVolume::writeAt(Node &node, uint64_t offset, ByteSpan data) {
        if (data.isEmpty()) return makeResult(size_t(0));
        const uint64_t end = offset + data.size();
        // Past the end: what the storage held there is zeroed first (left from a truncate).
        if (offset > node.size) {
                const Error err = resize(node, offset);
                if (err.isError()) return makeError<size_t>(err);
        }
        if (!node.paged && end <= PageBytes) {
                if (end > node.capacity) {
                        // Doubling, so a file written a little at a time isn't copied each time.
                        uint32_t capacity = smallCapacity(end);
                        if (node.capacity * 2u > capacity) capacity = node.capacity * 2u;
                        if (capacity > PageBytes) capacity = PageBytes;
                        auto *bytes = static_cast<uint8_t *>(take(capacity));
                        if (bytes == nullptr) return makeError<size_t>(Error::NoSpace);
                        std::memcpy(bytes, node.bytes, node.size);
                        give(node.bytes, node.capacity);
                        node.bytes = bytes;
                        node.capacity = capacity;
                }
                std::memcpy(node.bytes + offset, data.data(), data.size());
                if (end > node.size) node.size = end;
                node.modified = nowNs();
                return makeResult(data.size());
        }
        if (!node.paged) {
                const Error err = makePaged(node);
                if (err.isError()) return makeError<size_t>(err);
        }
        const uint64_t needed = pageCount(end);
        if (needed > UINT32_MAX) return makeError<size_t>(Error::NoSpace);
        if (needed > node.capacity) {
                uint64_t slots = node.capacity * 2u;
                if (slots < needed) slots = needed;
                auto **pages = static_cast<uint8_t **>(take(slots * sizeof(uint8_t *)));
                if (pages == nullptr && slots > needed) {
                        slots = needed;
                        pages = static_cast<uint8_t **>(take(slots * sizeof(uint8_t *)));
                }
                if (pages == nullptr) return makeError<size_t>(Error::NoSpace);
                std::memcpy(pages, node.pages, node.capacity * sizeof(uint8_t *));
                std::memset(pages + node.capacity, 0, (slots - node.capacity) * sizeof(uint8_t *));
                give(node.pages, node.capacity * sizeof(uint8_t *));
                node.pages = pages;
                node.capacity = static_cast<uint32_t>(slots);
        }
        size_t done = 0;
        while (done < data.size()) {
                const uint64_t at = offset + done;
                const uint64_t page = at / PageBytes;
                const uint32_t within = static_cast<uint32_t>(at % PageBytes);
                const size_t chunk = PageBytes - within < data.size() - done ? PageBytes - within : data.size() - done;
                uint8_t    *&p = node.pages[page];
                if (p == nullptr) {
                        p = static_cast<uint8_t *>(take(PageBytes));
                        if (p == nullptr) break;
                        // The parts this write doesn't cover read as zeros.
                        if (within != 0) std::memset(p, 0, within);
                        if (within + chunk != PageBytes) std::memset(p + within + chunk, 0, PageBytes - within - chunk);
                }
                std::memcpy(p + within, data.data() + done, chunk);
                done += chunk;
        }
        if (done == 0) return makeError<size_t>(Error::NoSpace);
        if (offset + done > node.size) node.size = offset + done;
        node.modified = nowNs();
        return makeResult(done);
}

Error TmpVolume::resize(Node &node, uint64_t size) {
        if (size < node.size) {
                if (!node.paged) {
                        if (size == 0) {
                                give(node.bytes, node.capacity);
                                node.bytes = nullptr;
                                node.capacity = 0;
                        }
                } else {
                        // Whole pages past the end go; the rest of the last is zeroed when it grows again.
                        for (uint64_t i = pageCount(size); i < node.capacity; ++i) {
                                give(node.pages[i], PageBytes);
                                node.pages[i] = nullptr;
                        }
                }
        } else if (size > node.size) {
                // Zero what the storage still holds between the end and the new end.
                if (!node.paged && size > PageBytes) {
                        // Pages from now on; what follows is a hole.
                        const Error err = makePaged(node);
                        if (err.isError()) return err;
                } else if (!node.paged) {
                        const uint64_t keep = node.capacity < size ? node.capacity : size;
                        if (keep > node.size) std::memset(node.bytes + node.size, 0, keep - node.size);
                        if (size > node.capacity) {
                                const uint32_t capacity = smallCapacity(size);
                                auto          *bytes = static_cast<uint8_t *>(take(capacity));
                                if (bytes == nullptr) return Error::NoSpace;
                                if (node.size != 0) std::memcpy(bytes, node.bytes, node.size);
                                std::memset(bytes + node.size, 0, capacity - node.size);
                                give(node.bytes, node.capacity);
                                node.bytes = bytes;
                                node.capacity = capacity;
                        }
                }
                if (node.paged) {
                        const uint64_t page = node.size / PageBytes;
                        const uint32_t within = static_cast<uint32_t>(node.size % PageBytes);
                        if (within != 0 && page < node.capacity && node.pages[page] != nullptr)
                                std::memset(node.pages[page] + within, 0, PageBytes - within);
                }
        }
        node.size = size;
        node.modified = nowNs();
        return Error::Ok;
}

Result<FsInfo> TmpVolume::info() {
        if (_root == nullptr) return makeError<FsInfo>(Error::NoMedia);
        FsInfo info;
        info.type = "tmpfs";
        info.clusterBytes = PageBytes;
        info.totalBytes = _limit;
        info.freeBytes = _used < _limit ? _limit - _used : 0;
        return makeResult(info);
}

Error TmpVolume::stat(StringView path, FileInfo &info, bool &noPath) {
        auto [n, err] = find(path, noPath);
        if (err.isError()) return err;
        info = FileInfo();
        err = info.name.assign(n->nameView());
        if (err.isError()) return err;
        info.type = n->type;
        info.size = n->type == FileType::File ? n->size : 0;
        info.modified = Timestamp{n->modified};
        info.attributes = n->attributes;
        return Error::Ok;
}

Result<FileHandle::Ptr> TmpVolume::open(StringView path, uint8_t flags, const FileIdentity &identity, bool &noPath,
                                        String *link) {
        auto [n, err] = find(path, noPath);
        if (err == Error::NotExist && !noPath && (flags & Vfs::Create) != 0) {
                Node      *parent = nullptr;
                StringView name;
                err = findParent(path, parent, name, noPath);
                if (err.isOk()) {
                        auto [made, cerr] = create(*parent, name, FileType::File);
                        n = made;
                        err = cerr;
                }
        } else if (err.isOk() && (flags & Vfs::Create) != 0 && (flags & Vfs::Exclusive) != 0) {
                err = Error::Exists;
        }
        if (err.isError()) return makeError<FileHandle::Ptr>(err);
        if (n->type == FileType::Directory) return makeError<FileHandle::Ptr>(Error::IsDir);
        const bool writing = (flags & Vfs::Write) != 0;
        // A System file may be an emulated link: read it here.
        if (link != nullptr) {
                link->clear();
                if ((flags & Vfs::Truncate) == 0 && (flags & Vfs::Read) != 0 &&
                    Fs::isLinkCandidate(n->attributes, FileType::File, n->size)) {
                        uint8_t      buf[Fs::MaxLinkFile];
                        const size_t got = readAt(*n, 0, WritableByteSpan(buf, sizeof(buf)));
                        if (Fs::parseLink(buf, got, *link).isOk()) return makeError<FileHandle::Ptr>(Error::Invalid);
                        link->clear();
                }
        }
        if (writing && (flags & Vfs::Truncate) != 0) (void)resize(*n, 0);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(TmpFile));
        if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *file = ::new (mem) TmpFile(*this, *n, flags, identity);
        ++n->refs;
        const uint8_t           mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) | (writing ? FileHandle::Write : 0);
        Result<FileHandle::Ptr> handle = FileHandle::open(*file, mode);
        if (handle.error().isError()) {
                file->~TmpFile();
                Memory::free(mem);
                unref(*n);
        }
        return handle;
}

Result<Volume::Reader *> TmpVolume::openReader(StringView path) {
        bool noPath = false;
        auto [n, err] = find(path, noPath);
        if (err.isError()) return makeError<Reader *>(err);
        if (n->type == FileType::Directory) return makeError<Reader *>(Error::IsDir);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(TmpReader));
        if (mem == nullptr) return makeError<Reader *>(Error::NoMem);
        ++n->refs;
        return makeResult<Reader *>(::new (mem) TmpReader(*this, *n));
}

void TmpVolume::closeReader(Reader *reader) {
        if (reader == nullptr) return;
        auto *r = static_cast<TmpReader *>(reader);
        if (!r->stale()) unref(r->node);
        r->~TmpReader();
        Memory::free(r);
        return;
}

Error TmpVolume::mkdir(StringView path, bool &noPath) {
        auto [existing, ferr] = find(path, noPath);
        if (ferr.isOk()) return Error::Exists;
        if (noPath || ferr != Error::NotExist) return ferr;
        Node      *parent = nullptr;
        StringView name;
        Error      err = findParent(path, parent, name, noPath);
        if (err.isOk()) err = create(*parent, name, FileType::Directory).error();
        return err;
}

Error TmpVolume::rmdir(StringView path) {
        bool noPath = false;
        auto [n, err] = find(path, noPath);
        if (err.isError()) return err;
        if (n == _root) return Error::Busy;
        if (n->type != FileType::Directory) return Error::NotDir;
        if (n->child != nullptr) return Error::NotEmpty;
        unlink(*n);
        return Error::Ok;
}

Error TmpVolume::remove(StringView path) {
        bool noPath = false;
        auto [n, err] = find(path, noPath);
        if (err.isError()) return err;
        if (n->type == FileType::Directory) return Error::IsDir;
        unlink(*n);
        return Error::Ok;
}

Error TmpVolume::rename(StringView from, StringView to, Vfs::RenameMode mode, bool &noPath) {
        auto [n, err] = find(from, noPath);
        if (err.isError()) return err;
        if (n == _root) return Error::Busy;
        Node      *parent = nullptr;
        StringView name;
        err = findParent(to, parent, name, noPath);
        if (err.isError()) return err;
        // Not into itself.
        for (const Node *up = parent; up != nullptr; up = up->parent)
                if (up == n) return Error::Invalid;
        Node *taken = parent->child;
        while (taken != nullptr && taken->nameView() != name) taken = taken->next;
        if (taken == n) return Error::Ok;
        if (taken != nullptr) {
                if (mode == Vfs::RenameMode::NoReplace) return Error::Exists;
                if (n->type == FileType::Directory && taken->type != FileType::Directory) return Error::NotDir;
                if (n->type != FileType::Directory && taken->type == FileType::Directory) return Error::IsDir;
                if (taken->child != nullptr) return Error::NotEmpty;
        }
        if (name.isEmpty() || name.size() > MaxName || name.contains('\0') || name == "." || name == "..")
                return Error::Invalid;
        // The new name first: nothing has moved if there is no room for it.
        char *newName = n->inlineName;
        if (name.size() > Node::InlineName) {
                newName = static_cast<char *>(take(name.size()));
                if (newName == nullptr) return Error::NoSpace;
        }
        // What had the name goes (kept while open, as a remove).
        if (taken != nullptr) unlink(*taken);
        // Out of its directory (cursors moved past it), into the new one at the end.
        Node *old = n->parent;
        for (DirCursor *c = _cursors; c != nullptr; c = c->nextCursor)
                if (c->entry == n) c->entry = n->next;
        for (Node **link = &old->child; *link != nullptr; link = &(*link)->next) {
                if (*link == n) {
                        *link = n->next;
                        break;
                }
        }
        n->next = nullptr;
        Node **link = &parent->child;
        while (*link != nullptr) link = &(*link)->next;
        *link = n;
        n->parent = parent;
        if (n->name != n->inlineName) give(n->name, n->nameBytes);
        n->name = newName;
        std::memcpy(n->name, name.data(), name.size());
        n->nameBytes = static_cast<uint16_t>(name.size());
        old->modified = parent->modified = nowNs();
        return Error::Ok;
}

Error TmpVolume::setModified(StringView path, Timestamp time) {
        bool noPath = false;
        auto [n, err] = find(path, noPath);
        if (err.isError()) return err;
        n->modified = time.ns;
        return Error::Ok;
}

Error TmpVolume::writeLink(StringView path, ByteSpan contents, bool &noPath) {
        auto [existing, ferr] = find(path, noPath);
        if (ferr.isOk()) return Error::Exists;
        if (noPath || ferr != Error::NotExist) return ferr;
        Node      *parent = nullptr;
        StringView name;
        Error      err = findParent(path, parent, name, noPath);
        if (err.isError()) return err;
        auto [n, cerr] = create(*parent, name, FileType::File);
        if (cerr.isError()) return cerr;
        n->attributes = FileInfo::System | FileInfo::Archive;
        const Result<size_t> written = writeAt(*n, 0, contents);
        if (written.isError() || written.value() != contents.size()) {
                unlink(*n);
                return Error::NoSpace;
        }
        return Error::Ok;
}

Result<Volume::Cursor *> TmpVolume::openDirectory(StringView path, bool &noPath) {
        auto [n, err] = find(path, noPath);
        if (err.isError()) return makeError<Cursor *>(err);
        if (n->type != FileType::Directory) {
                // The directory itself is a file (perhaps a link).
                noPath = true;
                return makeError<Cursor *>(Error::NotExist);
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(DirCursor));
        if (mem == nullptr) return makeError<Cursor *>(Error::NoMem);
        auto *cursor = ::new (mem) DirCursor(*this, *n);
        ++n->refs;
        cursor->nextCursor = _cursors;
        _cursors = cursor;
        return makeResult<Cursor *>(cursor);
}

void TmpVolume::closeDirectory(Cursor *cursor) {
        if (cursor == nullptr) return;
        auto *c = static_cast<DirCursor *>(cursor);
        if (!c->stale()) {
                for (DirCursor **link = &_cursors; *link != nullptr; link = &(*link)->nextCursor) {
                        if (*link == c) {
                                *link = c->nextCursor;
                                break;
                        }
                }
                unref(c->dir);
        }
        c->~DirCursor();
        Memory::free(c);
        return;
}

BMDS_NAMESPACE_END
