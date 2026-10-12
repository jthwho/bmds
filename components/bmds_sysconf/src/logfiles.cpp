/**
 * @file      logfiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system log in files, and /dev/log (devplan/logging.md §3.3, §3.5).
 */

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/filehandle.h>
#include <bmds/log.h>
#include <bmds/logfiles.h>
#include <bmds/logstream.h>
#include <bmds/logtext.h>
#include <bmds/memory.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace LogFiles {

        namespace {

                constexpr const char  *FileName = "system.log";
                constexpr unsigned int FlushMs = 1000;  // info lines wait this long for company
                constexpr size_t       InBytes = 4096;  // read from the history at a time
                constexpr size_t       OutBytes = 6144; // converted lines, written at a time
                constexpr size_t       StampBytes = 24; // "2026-10-11 17:07:04.123 "
                constexpr unsigned int WriteTimeoutMs = 5000;

                BMDS_SYSCONF(logDir, "log/dir", Config::Type::Path, "/sd0/p0/logs",
                             "Where the system log's files go (system.log, then system.log.1 and up); empty: no files",
                             0, 255, nullptr, nullptr)
                BMDS_SYSCONF(logFileBytes, "log/file-bytes", Config::Type::Int, "1048576",
                             "The size at which system.log becomes system.log.1", 16384, 1073741824, nullptr, nullptr)
                BMDS_SYSCONF(logFiles, "log/files", Config::Type::Int, "4",
                             "How many of the system log's files are kept, the current one counted", 1, 16, nullptr,
                             nullptr)

                struct State {
                                Semaphore         wake{1, 0}; // a line, a mount, or a setting changed
                                SysConf::Watcher  watcher;
                                std::atomic<bool> changed{true};
                                Thread            thread;
                                char             *in = nullptr;  // InBytes, PSRAM
                                char             *out = nullptr; // OutBytes, PSRAM
                                // The thread's own:
                                String          dir{MemSpace::Psram};
                                uint64_t        maxBytes = 0;
                                uint32_t        keep = 1;
                                FileHandle::Ptr file;
                                uint64_t        size = 0;       // the open file's
                                uint64_t        position = 0;   // the log's, written up to here
                                bool            booted = false; // the boot's marker written
                                bool            failed = false; // a write failed: wait for a mount or a setting
                };

                constinit StaticObject<State> state;

                Result<FileHandle::Ptr> openDevLog(void *, uint8_t flags) {
                        return FileHandle::open(SyslogStream::instance(),
                                                static_cast<uint8_t>(((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                                                     ((flags & Vfs::Write) ? FileHandle::Write : 0)));
                }

                void settingsChanged(void *) {
                        SysConf::Id id;
                        while (state->watcher.next(id)) {}
                        (void)state->watcher.overflowed();
                        state->changed.store(true);
                        state->wake.release();
                        return;
                }

                void loadSettings(State &s) {
                        if (SysConf::get(StringView("log/dir"), s.dir).isError()) s.dir.clear();
                        const Result<int32_t> bytes = SysConf::getInt(StringView("log/file-bytes"));
                        const Result<int32_t> files = SysConf::getInt(StringView("log/files"));
                        s.maxBytes = bytes.isOk() ? uint64_t(bytes.value()) : 1048576;
                        s.keep = files.isOk() ? uint32_t(files.value()) : 4;
                        return;
                }

                // @p dir/system.log, or with `.N` for @p index > 0.
                Error filePath(const State &s, uint32_t index, String &out) {
                        out.clear();
                        return index == 0 ? out.appendf("%s/%s", s.dir.cStr(), FileName)
                                          : out.appendf("%s/%s.%" PRIu32, s.dir.cStr(), FileName, index);
                }

                // Makes @p path's directories that are missing.
                Error makeDirs(StringView path) {
                        const Vfs::Context root{"/", nullptr};
                        for (size_t at = 1; at <= path.size(); ++at) {
                                if (at < path.size() && path[at] != '/') continue;
                                const Error err = Vfs::mkdir(path.left(at), root);
                                if (err.isError() && err != Error::Exists) return err;
                        }
                        return Error::Ok;
                }

                Error openFile(State &s) {
                        const Vfs::Context root{"/", nullptr};
                        String             path(MemSpace::Psram);
                        Error err = Vfs::stat(s.dir.view(), root).isOk() ? Error() : makeDirs(s.dir.view());
                        if (err.isOk()) err = filePath(s, 0, path);
                        if (err.isError()) return err;
                        auto [file, oerr] = Vfs::open(path.view(), Vfs::Write | Vfs::Create | Vfs::Append, root);
                        if (oerr.isError()) return oerr;
                        const Result<FileInfo> info = Vfs::stat(path.view(), root);
                        s.size = info.isOk() ? info.value().size : 0;
                        s.file = std::move(file);
                        return Error::Ok;
                }

                // system.log becomes system.log.1, and so on; the oldest beyond log/files goes.
                void rotate(State &s) {
                        s.file.reset();
                        const Vfs::Context root{"/", nullptr};
                        String             from(MemSpace::Psram), to(MemSpace::Psram);
                        for (uint32_t k = s.keep; k-- > 0;) {
                                if (filePath(s, k, from).isError()) return;
                                if (k + 1 >= s.keep) {
                                        (void)Vfs::remove(from.view(), root);
                                        continue;
                                }
                                if (filePath(s, k + 1, to).isError()) return;
                                (void)Vfs::rename(from.view(), to.view(), Vfs::RenameMode::Replace, root);
                        }
                        return;
                }

                Error writeOut(State &s, size_t len) {
                        if (len == 0) return Error::Ok;
                        const Result<size_t> n =
                                s.file->write(ByteSpan(reinterpret_cast<const uint8_t *>(s.out), len), WriteTimeoutMs);
                        if (n.isError()) return n.error();
                        s.size += n.value();
                        return n.value() == len ? Error::Ok : Error::IoError;
                }

                // `2026-10-11 17:07:04.123 ` for a line stamped @p uptimeMs, from @p wallMinusUptimeNs.
                size_t wallStamp(char *out, uint64_t uptimeMs, int64_t wallMinusUptimeNs) {
                        const int64_t   ns = wallMinusUptimeNs + int64_t(uptimeMs) * 1'000'000;
                        const LocalTime t = Clock::localTime(Timestamp{ns});
                        const unsigned  ms =
                                unsigned(((ns % 1'000'000'000) + 1'000'000'000) % 1'000'000'000 / 1'000'000);
                        const int n = std::snprintf(out, StampBytes + 1, "%04ld-%02u-%02u %02u:%02u:%02u.%03u ",
                                                    long(t.local.year), unsigned(t.local.month), unsigned(t.local.day),
                                                    unsigned(t.local.hour), unsigned(t.local.minute),
                                                    unsigned(t.local.second), ms);
                        return n > 0 && size_t(n) <= StampBytes ? size_t(n) : 0;
                }

                // The lines in @p len bytes of `in`, converted into `out` and written (as `out` fills).
                Error writeLines(State &s, size_t len, uint64_t lostBytes) {
                        const bool    wall = Clock::isSet();
                        const int64_t offset = Clock::now().ns - Clock::monotonicNs();
                        size_t        used = 0;
                        if (!s.booted) {
                                s.booted = true;
                                used += size_t(std::snprintf(s.out, OutBytes, "--- boot ---\n"));
                        }
                        if (lostBytes > 0)
                                used += size_t(std::snprintf(s.out + used, OutBytes - used,
                                                             "--- %" PRIu64 " bytes of the log lost ---\n", lostBytes));
                        StringView rest(s.in, len);
                        while (!rest.isEmpty()) {
                                const size_t nl = rest.indexOf('\n');
                                StringView   line = nl == StringView::NPos ? rest : rest.left(nl + 1);
                                rest = rest.mid(line.size());
                                if (used + StampBytes + line.size() > OutBytes) {
                                        const Error err = writeOut(s, used);
                                        if (err.isError()) return err;
                                        used = 0;
                                }
                                uint64_t uptimeMs = 0;
                                size_t   stamp = 0;
                                if (wall && LogText::parseStamp(line, uptimeMs, stamp)) {
                                        const size_t n = wallStamp(s.out + used, uptimeMs, offset);
                                        if (n > 0) {
                                                used += n;
                                                line = line.mid(stamp);
                                        }
                                }
                                const size_t n = line.size() < OutBytes - used ? line.size() : OutBytes - used;
                                std::memcpy(s.out + used, line.data(), n);
                                used += n;
                        }
                        return writeOut(s, used);
                }

                // A warning or an error among the lines: written at once.
                bool hasUrgent(StringView text) {
                        while (!text.isEmpty()) {
                                const StringView line = LogText::nextLine(text);
                                uint64_t         ms = 0;
                                size_t           stamp = 0;
                                if (LogText::parseStamp(line, ms, stamp) && stamp < line.size() &&
                                    (line[stamp] == 'W' || line[stamp] == 'E'))
                                        return true;
                        }
                        return false;
                }

                void run(void *) {
                        State &s = *state;
                        // Mounts wake the thread always; new lines only while it can write them.
                        WaitList::Entry mounts(Vfs::mountWaiters(), s.wake);
                        size_t          linesSlot = WaitList::NoSlot;
                        int64_t         pendingSinceMs = -1;
                        for (;;) {
                                if (s.changed.exchange(false)) {
                                        loadSettings(s);
                                        s.failed = false;
                                }
                                // After a failure only a mount or a setting wakes the thread: an error
                                // that logs a line must not make it try again at once.
                                const bool enabled = !s.dir.isEmpty() && !s.failed;
                                if (enabled && linesSlot == WaitList::NoSlot) linesSlot = Log::waiters().add(s.wake);
                                if (!enabled && linesSlot != WaitList::NoSlot) {
                                        Log::waiters().remove(linesSlot);
                                        linesSlot = WaitList::NoSlot;
                                }
                                if (!enabled || s.position == Log::position()) {
                                        (void)s.wake.acquire(WaitForever);
                                        s.failed = false;
                                        continue;
                                }
                                uint64_t      next = s.position, lost = 0;
                                const size_t  len = Log::read(next, Span<char>(s.in, InBytes), lost);
                                const int64_t nowMs = Clock::monotonicNs() / 1'000'000;
                                if (pendingSinceMs < 0) pendingSinceMs = nowMs;
                                const bool now = hasUrgent(StringView(s.in, len)) || next < Log::position() ||
                                                 nowMs - pendingSinceMs >= FlushMs;
                                if (!now) {
                                        (void)s.wake.acquire(unsigned(FlushMs - (nowMs - pendingSinceMs)));
                                        continue;
                                }
                                // Open only to write: FatFs refuses readers of a file open for writing.
                                const Error err = openFile(s);
                                const bool  ok = err.isOk() && writeLines(s, len, lost).isOk();
                                s.file.reset();
                                if (!ok) {
                                        s.failed = true;
                                        continue;
                                }
                                s.position = next;
                                pendingSinceMs = -1;
                                if (s.size >= s.maxBytes) rotate(s);
                        }
                }

        } // namespace

        Error init() {
                if (state.isCreated()) return Error::Ok;
                State &s = state.create();
                s.in = static_cast<char *>(Memory::alloc(MemSpace::Psram, InBytes));
                s.out = static_cast<char *>(Memory::alloc(MemSpace::Psram, OutBytes));
                if (s.in == nullptr || s.out == nullptr) return Error::NoMem;
                (void)SyslogStream::instance();
                static constexpr Vfs::DeviceFile DevLog{"log", openDevLog, nullptr, nullptr, nullptr, true};
                Error                            err = Vfs::addDeviceFile(DevLog);
                if (err.isOk()) err = s.watcher.start(StringView("log"), &settingsChanged, nullptr);
                if (err.isError()) return err;
                Thread::Config config;
                config.name = "logd";
                config.stackBytes = 5120;               // 2.9 KB used after rotations on exFAT
                config.stackSpace = MemSpace::Psram;    // /boot too: flash0's task does the flash work
                config.priority = 1;
                config.kernel = true;
                return s.thread.start(config, &run, nullptr);
        }

} // namespace LogFiles

BMDS_NAMESPACE_END
