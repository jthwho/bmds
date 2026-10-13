/**
 * @file      leapfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The leap-second list (devplan/network.md §31e): IANA's leap-seconds.list at clock/leapseconds,
 * or the ROM's, whichever good one expires later, read into the kernel's TAI offset.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <bmds/clock.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The path set ("" for none); under the storage lock.
        constinit char listPath[Vfs::MaxZoneDatabasePath + 1] = {};
        // The list in use since the last read ("" for none); under the storage lock.
        constinit char inUse[Vfs::MaxZoneDatabasePath + 1] = {};
        // Told of a list at the path that is refused (the target logs it; this layer doesn't log).
        constinit std::atomic<Vfs::LeapSecondsRefused> refused{nullptr};

        // Reads @p path into @p table: the read's error, and the verdict when it was read.
        Error readList(StringView path, LeapSeconds::Table &table, LeapSeconds::Verdict &verdict) {
                char  *text = nullptr;
                size_t size = 0;
                {
                        Mutex::Locker   lock(Fs::lock(), WaitForever);
                        Volume         *volume = nullptr;
                        Volume::Reader *reader = nullptr;
                        const Error     err = Fs::openReaderLocked(path, volume, reader);
                        if (err.isError()) return err;
                        const uint64_t bytes = reader->size();
                        Error          readErr = Error::Ok;
                        if (bytes > Vfs::MaxLeapSecondsBytes) {
                                readErr = Error::OutOfRange;
                        } else {
                                text = static_cast<char *>(Memory::alloc(MemSpace::Psram, size_t(bytes) + 1));
                                if (text == nullptr) readErr = Error::NoMem;
                        }
                        if (readErr.isOk()) {
                                auto [got, e] = reader->readAt(
                                        0, WritableByteSpan(reinterpret_cast<uint8_t *>(text), size_t(bytes)));
                                readErr = e.isError() ? e : got != bytes ? Error(Error::IoError) : Error();
                                size = got;
                        }
                        volume->closeReader(reader);
                        if (readErr.isError()) {
                                Memory::free(text);
                                return readErr;
                        }
                }
                verdict = LeapSeconds::parse(StringView(text, size), table);
                Memory::free(text);
                return Error::Ok;
        }

} // namespace

void Vfs::examineLeapSeconds(LeapSecondsReport &out) {
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                std::memcpy(out.path, listPath, sizeof(out.path));
        }
        out.any = false;
        out.fromRom = false;
        const StringView path(out.path);
        bool             pathGood = false;
        if (!path.isEmpty()) {
                out.pathError = readList(path, out.table, out.pathVerdict);
                pathGood = out.pathError.isOk() && out.pathVerdict == LeapSeconds::Verdict::Ok;
                if (pathGood) out.pathExpires = out.table.expires;
        } else {
                out.pathError = Error::NotExist;
        }
        if (path.isEmpty()) return; // none: not the ROM's either
        if (path == RomLeapSeconds) {
                out.romError = out.pathError;
                out.romVerdict = out.pathVerdict;
                out.any = pathGood;
                out.fromRom = pathGood;
                return;
        }
        // The ROM's: in use unless the path's is good and expires as late or later.
        auto *rom = static_cast<LeapSeconds::Table *>(Memory::alloc(MemSpace::Psram, sizeof(LeapSeconds::Table)));
        if (rom == nullptr) {
                out.romError = Error::NoMem;
                out.any = pathGood;
                return;
        }
        new (rom) LeapSeconds::Table();
        out.romError = readList(RomLeapSeconds, *rom, out.romVerdict);
        const bool romGood = out.romError.isOk() && out.romVerdict == LeapSeconds::Verdict::Ok;
        if (romGood && (!pathGood || rom->expires > out.table.expires)) {
                out.table = *rom;
                out.fromRom = true;
        }
        out.any = pathGood || romGood;
        Memory::free(rom);
        return;
}

void Vfs::reloadLeapSeconds() {
        auto *report = static_cast<LeapSecondsReport *>(Memory::alloc(MemSpace::Psram, sizeof(LeapSecondsReport)));
        if (report == nullptr) return; // the table loaded before stays
        new (report) LeapSecondsReport();
        examineLeapSeconds(*report);
        const Vfs::LeapSecondsRefused tell = refused.load(std::memory_order_acquire);
        if (tell != nullptr && report->pathError.isOk() && report->pathVerdict != LeapSeconds::Verdict::Ok)
                tell(StringView(report->path), report->pathVerdict);
        Clock::setLeapSeconds(report->any ? &report->table : nullptr);
        {
                Mutex::Locker    lock(Fs::lock(), WaitForever);
                const StringView used = !report->any      ? StringView()
                                        : report->fromRom ? RomLeapSeconds
                                                          : StringView(report->path);
                std::memcpy(inUse, used.data(), used.size());
                inUse[used.size()] = '\0';
        }
        report->~LeapSecondsReport();
        Memory::free(report);
        return;
}

void Vfs::setLeapSecondsRefused(LeapSecondsRefused handler) {
        refused.store(handler, std::memory_order_release);
        return;
}

Result<String> Vfs::leapSecondsInUse() {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        path;
        const Error   err = path.append(StringView(inUse));
        if (err.isError()) return makeError<String>(err);
        return makeResult(std::move(path));
}

Error Vfs::setLeapSecondsFile(StringView path) {
        if (!path.isEmpty() && (path[0] != '/' || path.size() > MaxZoneDatabasePath)) return Error::Invalid;
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                std::memcpy(listPath, path.data(), path.size());
                listPath[path.size()] = '\0';
        }
        reloadLeapSeconds();
        return Error::Ok;
}

BMDS_NAMESPACE_END
