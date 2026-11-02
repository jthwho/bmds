/**
 * @file      packagesdetail.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What the engine (packages.cpp) and the repositories (repositories.cpp) share: private to
 * bmds_pkg.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/memory.h>
#include <bmds/packages.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgsolver.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace Packages::Detail {

        // The most a manifest, a description and an icons section may be: far more than any
        // package has, little enough to hold.
        constexpr size_t MaxManifestBytes = 4 * 1024 * 1024;
        constexpr size_t MaxDescriptionBytes = 64 * 1024;
        constexpr size_t MaxIconsBytes = 256 * 1024;
        constexpr size_t ChunkBytes = 16 * 1024;

        // Whether an operation is running (packages.cpp).
        extern std::atomic<bool> busy;

        // One operation at a time.
        class Lock {
                public:
                        Lock() : _held(!busy.exchange(true, std::memory_order_acquire)) {}
                        ~Lock() {
                                if (_held) busy.store(false, std::memory_order_release);
                        }
                        Lock(const Lock &) = delete;
                        Lock &operator=(const Lock &) = delete;

                        bool held() const { return _held; }

                private:
                        bool _held;
        };

        inline uint32_t hashPath(StringView path) {
                uint32_t h = 2166136261u;
                for (char c : path) {
                        h ^= uint8_t(StringView::toLower(c));
                        h *= 16777619u;
                }
                return h;
        }

        struct PathEntry {
                        StringView        path;
                        PkgManifest::Kind kind = PkgManifest::Kind::File;
        };

        class PathSet {
                public:
                        PathSet() : _entries(MemSpace::Psram), _slots(MemSpace::Psram) {}

                        // Adds every entry of @p manifest (fields skipped). Its text must outlive the set.
                        Error build(StringView manifest) {
                                PkgManifest::Reader r(manifest);
                                PkgManifest::Entry  e;
                                Error               err;
                                while ((err = r.next(e)).isOk()) {
                                        if (e.kind == PkgManifest::Kind::Field) continue;
                                        err = _entries.append(PathEntry{e.path, e.kind});
                                        if (err.isError()) return err;
                                }
                                if (err != Error::EndOfFile) return Error::CorruptData;
                                size_t slots = 16;
                                while (slots < 2 * _entries.size()) slots *= 2;
                                err = _slots.reserve(slots);
                                if (err.isError()) return err;
                                for (size_t i = 0; i < slots; ++i) (void)_slots.append(0);
                                for (size_t i = 0; i < _entries.size(); ++i) {
                                        if (find(_entries[i].path) != nullptr) return Error::Exists; // twice
                                        size_t s = hashPath(_entries[i].path) & (slots - 1);
                                        while (_slots[s] != 0) s = (s + 1) & (slots - 1);
                                        _slots[s] = uint32_t(i + 1);
                                }
                                return Error::Ok;
                        }

                        const PathEntry *find(StringView path) const {
                                if (_slots.isEmpty()) return nullptr;
                                const size_t mask = _slots.size() - 1;
                                for (size_t s = hashPath(path) & mask; _slots[s] != 0; s = (s + 1) & mask) {
                                        const PathEntry &e = _entries[_slots[s] - 1];
                                        if (e.path.equalsIgnoreCase(path)) return &e;
                                }
                                return nullptr;
                        }

                        Span<const PathEntry> entries() const { return _entries.span(); }

                private:
                        List<PathEntry> _entries;
                        List<uint32_t>  _slots;
        };

        struct Loaded {
                        FileHandle::Ptr         file;
                        PkgFile::Package        head;
                        String                  manifest;
                        String                  description;
                        String                  icons;
                        Info                    info;
                        PathSet                 paths;
                        const PkgFile::Section *data = nullptr;
        };

        class Buffer {
                public:
                        explicit Buffer(size_t bytes)
                            : _p(static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes))) {}
                        ~Buffer() { Memory::free(_p); }
                        Buffer(const Buffer &) = delete;
                        Buffer  &operator=(const Buffer &) = delete;
                        uint8_t *data() const { return _p; }

                private:
                        uint8_t *_p;
        };

        /** @brief How much of a package `load()` reads and checks. */
        enum class LoadMode : uint8_t {
                Peek,    ///< The head, the signature, the manifest: for planning.
                Listing, ///< Also the description and the icons: for an index.
                Check,   ///< Full but the data's hash: for a dry run (installing checks every file's).
                Full,    ///< Every section, the data's hash and the paths too: for installing.
        };

        const Vfs::Context &vfs();

        /** @brief Sets @p why (and returns @p err): what went wrong, in words. */
        Error fail(String &why, Error err, const char *fmt, ...) BMDS_PRINTF_FORMAT(3, 4);

        Result<String> join(StringView dir, StringView rel);
        Result<String> withSuffix(StringView path, StringView suffix);
        Result<String> dbFile(const Config &config, StringView name, StringView suffix);
        void           progress(const Listener &l, StringView package, Step step, uint64_t done, uint64_t total);
        void           note(const Listener &l, StringView package, StringView text);
        Error          readNext(FileHandle &file, uint8_t *to, size_t bytes);
        Error          readWhole(StringView path, size_t limit, String &out);
        Error          writeWhole(StringView path, ByteSpan data);
        Error          writeWhole(StringView path, StringView text);
        Error          removeIfThere(StringView path);
        Error          makeDirectories(StringView path);
        void           hex(const uint8_t *bytes, size_t n, char *out);
        Error          settle(Info &info);
        Error          load(StringView path, const Config &config, Loaded &out, String &why, LoadMode mode);
        Error          readStatus(const Config &config, StringView name, Info &out);
        Error          listInstalled(const Config &config, List<String> &out);
        Error          recoverLocked(const Config &config, const Listener &listener, String &why);

        const char *signerName(Packages::Signer signer);

        // keys.cpp
        /** @brief Verifies @p parsed's signature against the keyring; who signed to @p signer. */
        Error verifyHead(const SignedContainer::Head &parsed, Span<const uint8_t> head, const Config &config,
                         Packages::Signer &signer);
        /** @brief Verifies against the keyring's keys @p ids names (any when empty). */
        Error verifyWith(const SignedContainer::Head &parsed, Span<const uint8_t> head, const Config &config,
                         Span<const StringView> ids);

        // repositories.cpp
        Error installedForWalk(const Config &config, List<Info> &infos, List<PkgSolver::Package> &out);
        Error pickIcon(ByteSpan section, bool named, StringView name, uint16_t side, List<uint8_t> &out);

} // namespace Packages::Detail

BMDS_NAMESPACE_END
