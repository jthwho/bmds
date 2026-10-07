/**
 * @file      clipfiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /dev/clip0 and /dev/clip0p: a clipboard's text as a file (devplan/clipboard.md C9, §4c).
 */

#include <cstring>
#include <new>
#include <bmds/clipboard.h>
#include <bmds/clipboardreader.h>
#include <bmds/memory.h>
#include <bmds/stream.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // How long a file being opened waits for an owner's bytes.
        constexpr unsigned int OwnerTimeoutMs = 5000;

        // A file of the clipboard's text: read, it is the text when it was opened; written, it
        // is a file whose contents become the text when its last handle closes (if changed).
        class ClipFile final : public Stream {
                public:
                        ClipFile(Clipboard &clipboard, Clipboard::Selection selection, const char *path, bool append)
                            : _clipboard(clipboard), _selection(selection), _path(path), _append(append),
                              _text(MemSpace::Psram) {}

                        // Takes the clipboard's text (converted; nothing if it has none).
                        Error load() {
                                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipboardReader));
                                if (mem == nullptr) return Error::NoMem;
                                auto *reader = ::new (mem) ClipboardReader(_clipboard);
                                Error err = reader->open(_selection, ClipboardType::Text);
                                if (err == Error::Empty || err == Error::NotFound || err == Error::NotSupported)
                                        err = Error::Ok; // no text: an empty file
                                else if (err.isOk())
                                        err = readAll(*reader);
                                reader->~ClipboardReader();
                                Memory::free(reader);
                                return err;
                        }

                        Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override {
                                (void)timeoutMs;
                                if (_position >= _text.size()) return makeError<size_t>(Error::EndOfFile);
                                size_t n = _text.size() - static_cast<size_t>(_position);
                                if (n > buf.size()) n = buf.size();
                                std::memcpy(buf.data(), _text.data() + _position, n);
                                _position += n;
                                return makeResult(n);
                        }

                        Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override {
                                (void)timeoutMs;
                                if (_append) _position = _text.size();
                                const uint64_t end = _position + data.size();
                                if (end > _clipboard.limit()) return makeError<size_t>(Error::NoSpace);
                                Error err = grow(static_cast<size_t>(end));
                                if (err.isError()) return makeError<size_t>(err);
                                std::memcpy(_text.data() + _position, data.data(), data.size());
                                _position = end;
                                _changed = true;
                                return makeResult(data.size());
                        }

                        using Stream::write;

                        Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                int64_t base = 0;
                                if (from == SeekFrom::Current) base = static_cast<int64_t>(_position);
                                if (from == SeekFrom::End) base = static_cast<int64_t>(_text.size());
                                const int64_t to = base + offset;
                                if (to < 0) return makeError<uint64_t>(Error::Invalid);
                                _position = static_cast<uint64_t>(to);
                                return makeResult(_position);
                        }

                        Error truncate(uint64_t size) override {
                                if (size > _clipboard.limit()) return Error::NoSpace;
                                if (size == _text.size()) return Error::Ok;
                                if (size < _text.size()) {
                                        _text.truncate(static_cast<size_t>(size));
                                } else {
                                        const Error err = grow(static_cast<size_t>(size));
                                        if (err.isError()) return err;
                                }
                                _changed = true;
                                return Error::Ok;
                        }

                        // Set by the opener: an emptied file (O_TRUNC) is a change even unwritten.
                        void setChanged() {
                                _changed = true;
                                return;
                        }

                        void handleClosed(uint8_t mode) override {
                                if ((mode & FileHandle::Write) != 0 && _changed) {
                                        const StringView types[] = {ClipboardType::Text};
                                        if (_text.isEmpty())
                                                (void)_clipboard.clear(_selection, 0);
                                        else
                                                (void)_clipboard.copy(
                                                        _selection, Span<const StringView>(types),
                                                        ByteSpan(reinterpret_cast<const uint8_t *>(_text.data()),
                                                                 _text.size()),
                                                        _path, 0);
                                }
                                this->~ClipFile();
                                Memory::free(this);
                                return;
                        }

                private:
                        ~ClipFile() = default;

                        Error readAll(ClipboardReader &reader) {
                                uint8_t buf[512];
                                for (;;) {
                                        auto [n, err] = reader.read(WritableByteSpan(buf, sizeof(buf)), OwnerTimeoutMs);
                                        if (err == Error::EndOfFile) return Error::Ok;
                                        if (err.isError()) return err;
                                        if (_text.size() + n > _clipboard.limit()) return Error::NoSpace;
                                        err = _text.append(StringView(reinterpret_cast<const char *>(buf), n));
                                        if (err.isError()) return err;
                                }
                        }

                        // Makes the text @p size bytes at least, zeros after the old end.
                        Error grow(size_t size) {
                                if (size <= _text.size()) return Error::Ok;
                                const size_t old = _text.size();
                                Error        err = _text.reserve(size);
                                for (size_t i = old; i < size && err.isOk(); ++i) err = _text.append('\0');
                                return err;
                        }

                        Clipboard           &_clipboard;
                        Clipboard::Selection _selection;
                        const char          *_path;
                        bool                 _append;
                        bool                 _changed = false;
                        uint64_t             _position = 0;
                        String               _text;
        };

        struct FileArg {
                        size_t               number;
                        Clipboard::Selection selection;
                        const char          *path; // the owner's name when written
        };

        constexpr FileArg Args[] = {{0, Clipboard::Selection::Clipboard, "/dev/clip0"},
                                    {0, Clipboard::Selection::Primary, "/dev/clip0p"}};

        Result<FileHandle::Ptr> openClip(void *arg, uint8_t flags) {
                const auto *a = static_cast<const FileArg *>(arg);
                auto [clipboard, cerr] = Clipboard::clipboard(a->number);
                if (cerr.isError()) return makeError<FileHandle::Ptr>(cerr);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipFile));
                if (mem == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
                auto *file = ::new (mem) ClipFile(*clipboard, a->selection, a->path, (flags & Vfs::Append) != 0);
                const uint8_t mode =
                        ((flags & Vfs::Read) ? FileHandle::Read : 0) | ((flags & Vfs::Write) ? FileHandle::Write : 0);
                // Emptied when opened so (`>`); else the text first (vi writes over it, then cuts it).
                Error err = Error::Ok;
                if ((flags & Vfs::Truncate) != 0)
                        file->setChanged();
                else
                        err = file->load();
                Result<FileHandle::Ptr> handle =
                        err.isOk() ? FileHandle::open(*file, mode) : makeError<FileHandle::Ptr>(err);
                if (handle.error().isError()) file->handleClosed(0); // frees it, unchanged
                return handle;
        }

        uint64_t clipSize(void *arg) {
                const auto *a = static_cast<const FileArg *>(arg);
                auto [clipboard, err] = Clipboard::clipboard(a->number);
                return err.isOk() ? clipboard->storedTextSize(a->selection) : 0;
        }

} // namespace

Error Clipboard::addFiles(size_t number) {
        if (number != 0) return Error::NotSupported; // the names below are clipboard 0's
        if (clipboard(number).error().isError()) return Error::NotExist;
        Vfs::DeviceFile file;
        file.name = "clip0";
        file.open = openClip;
        file.size = clipSize;
        file.arg = const_cast<FileArg *>(&Args[0]);
        Error err = Vfs::addDeviceFile(file);
        if (err.isError()) return err;
        file.name = "clip0p";
        file.arg = const_cast<FileArg *>(&Args[1]);
        return Vfs::addDeviceFile(file);
}

BMDS_NAMESPACE_END
