/**
 * @file      vfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <bmds/builtinprograms.h>
#include <bmds/directorywatch.h>
#include <bmds/clock.h>
#include <bmds/flashdevice.h>
#include <bmds/memory.h>
#include <bmds/memorystream.h>
#include <bmds/partitions.h>
#include <bmds/romimage.h>
#include <bmds/trash.h>

#include "etc.h"
#include "fatlayer.h"
#include "fslayer.h"
#include "lfsvolume.h"
#include "romvolume.h"
#include "tmpvolume.h"
#include "trashindex.h"
#include "zoneinfo.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Vfs::MaxMountPath;
        using Vfs::MaxMounts;

        struct Mount {
                        char         path[MaxMountPath + 1] = {};
                        Volume      *volume = nullptr;
                        BlockDevice *device = nullptr; // null for a filesystem in memory (tmpfs)
                        bool         pinned = false;   // never unmounted (/flash)
        };

        // The mount table, in mount order; under the storage lock.
        constinit Mount  mounts[MaxMounts];
        constinit size_t mountCount = 0;
        constinit WaitListOf<4> mountChanges; // released after each mount and unmount
        constinit std::atomic<uint32_t> settling{0};  // mounts on their way (beginSettling())

        constinit NullStream   devNull;
        constinit ZeroStream   devZero;
        constinit RandomStream devRandom;
        constinit RandomStream devUrandom; // the same generator; its own node identity

        constinit Vfs::ChangeHook changeHook = nullptr; // under the storage lock

        // The files other components added to /dev: written under the storage lock before the
        // count that publishes them, so open() reads them without it.
        constinit Vfs::DeviceFile     deviceFiles[Vfs::MaxDeviceFiles];
        constinit std::atomic<size_t> deviceFileCount{0};

        bool isPresent(const Vfs::DeviceFile &file) {
                return file.present == nullptr || file.present(file.arg);
        }

        // The added file named @p name (while it exists, unless @p evenGone), or null.
        const Vfs::DeviceFile *findDeviceFile(StringView name, bool evenGone = false) {
                const size_t n = deviceFileCount.load(std::memory_order_acquire);
                for (size_t i = 0; i < n; ++i)
                        if (name == deviceFiles[i].name)
                                return evenGone || isPresent(deviceFiles[i]) ? &deviceFiles[i] : nullptr;
                return nullptr;
        }

        // Directories of numbered devices other components added (Vfs::addDeviceDirectory).
        constinit Vfs::DeviceDirectory deviceDirs[Vfs::MaxDeviceDirectories];
        constinit std::atomic<size_t>  deviceDirCount{0};

        const Vfs::DeviceDirectory *findDeviceDir(StringView name) {
                const size_t n = deviceDirCount.load(std::memory_order_acquire);
                for (size_t i = 0; i < n; ++i)
                        if (name == deviceDirs[i].name) return &deviceDirs[i];
                return nullptr;
        }

        // @p text as an entry's number (decimal, no leading zeros) below @p dir's count.
        bool deviceIndex(const Vfs::DeviceDirectory &dir, StringView text, size_t &index) {
                if (text.isEmpty() || text.size() > 4 || (text.size() > 1 && text[0] == '0')) return false;
                size_t v = 0;
                for (char c : text) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + static_cast<size_t>(c - '0');
                }
                if (v >= dir.count) return false;
                index = v;
                return true;
        }

        // /dev/NAME/N as an added directory's present entry: true with @p dir and @p index set.
        bool findDeviceEntry(StringView path, const Vfs::DeviceDirectory *&dir, size_t &index) {
                if (!path.startsWith("/dev/")) return false;
                const StringView rest = path.mid(5);
                const size_t     slash = rest.indexOf('/');
                if (slash == StringView::NPos) return false;
                dir = findDeviceDir(rest.left(slash));
                return dir != nullptr && deviceIndex(*dir, rest.mid(slash + 1), index) &&
                       (dir->present == nullptr || dir->present(dir->arg, index));
        }

        // The fixed device nodes in /dev, listed before the block devices.
        // All but the console are streams without state, shared by every caller.
        struct DevNode {
                        const char *name;
                        Stream     *stream; // null: the caller's console
        };
        constexpr DevNode DevNodes[] = {
                {"console", nullptr},     {"null", &devNull}, {"random", &devRandom},
                {"urandom", &devUrandom}, {"zero", &devZero}, {"tty", nullptr},
        };

        // The fixed node named @p name, or null.
        const DevNode *findDevNode(StringView name) {
                for (const DevNode &n : DevNodes) {
                        if (name == n.name) return &n;
                }
                return nullptr;
        }

        // What a normalized path names.
        enum class Kind : uint8_t {
                Virtual,
                DevDir,
                DevStream, // a fixed node's stream: /dev/null, /dev/zero, ...
                DevConsole,
                DevBlock,
                Mounted,    // in a mounted filesystem
                BinDir,     // /bin: the programs built in (devplan/programs.md §17)
                BinProgram, // /bin/NAME
                ZoneDir,    // /share/zoneinfo and its directories (devplan/time.md §9)
                ZoneFile,   // /share/zoneinfo/NAME
                DevFile,    // a file another component added (Vfs::addDeviceFile)
                RootLink,   // /home or /cfg, a link setHome() or setCfg() made (devplan/sysconf.md §7, cfg.md)
                EtcDir,     // /etc (devplan/network.md §8)
                EtcFile,    // /etc/NAME, built when opened (Vfs::addEtcFile)
                DevSubDir,  // /dev/NAME, a directory of numbered devices (Vfs::addDeviceDirectory)
                DevSubFile, // /dev/NAME/N, one of them
                AppsDir,    // /apps (devplan/apps.md §5)
                AppLink,    // /apps/NAME, a link setAppLinks() made
                TrashDir,   // /trash, every filesystem's trash (devplan/filebrowser.md §4k)
        };

        // Where the programs built into the firmware are listed (read-only).
        constexpr StringView BinPath("/bin");

        // The link to the user's home (setHome()): its target, and where it leads while the
        // target's filesystem isn't mounted; Psram, under the storage lock. Null: no /home.
        using Vfs::HomePath;
        constinit char           *homeTarget = nullptr;
        constinit char           *homeFallback = nullptr;
        constinit uint8_t         homeShown = 0; // what was last reported: 0 nothing, 1 the target, 2 the fallback
        constinit Vfs::HomeReport homeReport = nullptr;

        // The link to the configuration (setCfg(), devplan/cfg.md): its target, Psram, under the
        // storage lock. Null: no /cfg.
        using Vfs::CfgPath;
        constinit char *cfgTarget = nullptr;

        // /apps's links (setAppLinks()): one Psram block, an Entry each then their text; under
        // the storage lock. Null: /apps is empty.
        using Vfs::AppsPath;
        struct AppTable {
                        struct Entry {
                                        uint32_t name; // offsets into text, each NUL-ended
                                        uint32_t target;
                        };
                        size_t count;
                        Entry *entries;
                        char  *text;
        };
        constinit AppTable *appTable = nullptr;

        size_t appCountLocked() {
                return appTable != nullptr ? appTable->count : 0;
        }

        StringView appNameLocked(size_t i) {
                return StringView(appTable->text + appTable->entries[i].name);
        }

        // Where /apps/@p name leads, or empty (none). Lock held.
        StringView appTargetLocked(StringView name) {
                for (size_t i = 0; i < appCountLocked(); ++i)
                        if (appNameLocked(i) == name) return StringView(appTable->text + appTable->entries[i].target);
                return StringView();
        }

        // The mount table as the trash index takes it, by slot. Lock held.
        Span<const TrashIndex::Mount> trashMountsLocked(TrashIndex::Mount (&out)[MaxMounts]) {
                // One that locks itself has no trash (devplan/netfs.md): its slot kept, empty.
                for (size_t i = 0; i < mountCount; ++i)
                        out[i] = TrashIndex::Mount{StringView(mounts[i].path),
                                                   mounts[i].volume->locksItself() ? nullptr : mounts[i].volume};
                return Span<const TrashIndex::Mount>(out, mountCount);
        }

        // The mounts changed: the trash index made again when next needed. Lock held.
        void trashMountsChangedLocked() {
                TrashIndex::Mount m[MaxMounts];
                TrashIndex::mountsChangedLocked(trashMountsLocked(m));
                return;
        }

        // True if @p rest (within a volume) is an entry at the top of its trash:
        // `/.bmds/trash/files/NAME`; @p name set to NAME.
        bool isTrashEntry(StringView rest, StringView &name) {
                constexpr StringView Files = TrashIndex::FilesRest;
                if (rest.size() <= Files.size() + 1 || !rest.startsWith(Files) || rest[Files.size()] != '/')
                        return false;
                name = rest.mid(Files.size() + 1);
                return name.indexOf('/') == StringView::NPos;
        }

        // Where the time zone database's directory is (read-only).
        using Vfs::ZoneInfoPath;
        constexpr StringView SharePath("/share");

        struct Node {
                        Kind                kind = Kind::Virtual;
                        const ProgramEntry *program = nullptr; // BinProgram
                        Stream             *stream = nullptr;  // DevStream
                        const Vfs::DeviceFile *file = nullptr;    // DevFile
                        const Vfs::EtcFile    *etc = nullptr;     // EtcFile
                        const Vfs::DeviceDirectory *devDir = nullptr;  // DevSubDir, DevSubFile
                        size_t                      devIndex = 0;      // DevSubFile
                        uint32_t            zoneOffset = 0;    // ZoneFile: its data in the database
                        uint32_t            zoneLength = 0;
                        Timestamp           zoneModified; // ZoneDir, ZoneFile: the database's
                        Volume                *volume = nullptr;
                        BlockDevice           *device = nullptr;
                        StringView             name; // The last component ("" for /).
                        StringView             rest; // Mounted: within the volume, "" for its root, else "/dir/file".
                        uint32_t               mount = 0; // Mounted: the mount slot + 1 (FileInfo::device).
        };

        // Calls @p fn, a call into @p volume, with the storage lock held by the caller: a volume
        // that locks itself (a network filesystem, devplan/netfs.md) is called without it,
        // retained meanwhile, so a call waiting on a network holds no other file operation.
        // The lock is taken back before returning; the mounts may have changed meanwhile.
        template <typename Fn> auto callVolume(Volume &volume, Fn &&fn) -> decltype(fn()) {
                if (!volume.locksItself()) return fn();
                volume.retain();
                Fs::lock().unlock();
                auto result = fn();
                (void)Fs::lock().lock(WaitForever);
                volume.release();
                return result;
        }

        // FileInfo::id: FNV-1a over the normalized path, ASCII letters folded
        // (FAT names are case-insensitive). The root hashes as "", so that a
        // directory's prefix and "/name" make its children's ids.
        constexpr uint64_t IdSeed = 14695981039346656037ull;

        uint64_t idAppend(uint64_t id, StringView text) {
                for (char c : text) {
                        uint8_t b = static_cast<uint8_t>(c);
                        if (b >= 'A' && b <= 'Z') b = static_cast<uint8_t>(b + ('a' - 'A'));
                        id = (id ^ b) * 1099511628211ull;
                }
                return id;
        }

        uint64_t pathId(StringView normalized) {
                return idAppend(IdSeed, normalized == "/" ? StringView() : normalized);
        }

        uint64_t objectId(const void *object) {
                return reinterpret_cast<uintptr_t>(object);
        }

        // True if @p path is @p dir itself or below it.
        bool isAtOrBelow(StringView path, StringView dir) {
                if (dir == "/") return true;
                return path.startsWith(dir) && (path.size() == dir.size() || path[dir.size()] == '/');
        }

        // True if @p path is strictly below @p dir.
        bool isBelow(StringView path, StringView dir) {
                return path.size() > dir.size() && isAtOrBelow(path, dir);
        }

        // True if normalized @p path names an entry at the top of /trash that no trash has
        // (the rewrite turns the others into their files).
        bool isNewTrashName(StringView path) {
                return isBelow(path, Vfs::TrashPath) &&
                       path.indexOf('/', Vfs::TrashPath.size() + 1) == StringView::NPos;
        }

        // True if normalized @p path is in a tree nothing can be written to:
        // the programs built in, the time zone database, /etc.
        bool isReadOnlyTree(StringView path) {
                return isAtOrBelow(path, BinPath) || isAtOrBelow(path, ZoneInfoPath) ||
                       isAtOrBelow(path, Vfs::EtcPath) || isAtOrBelow(path, AppsPath);
        }

        // Resolves a normalized path. Lock held.
        Result<Node> resolveLocked(StringView path) {
                Node node;
                if (path == "/") return makeResult(node);
                node.name = path.mid(path.lastIndexOf('/') + 1);
                if ((homeTarget != nullptr && path == HomePath) || (cfgTarget != nullptr && path == CfgPath)) {
                        node.kind = Kind::RootLink;
                        return makeResult(node);
                }
                if (isAtOrBelow(path, "/dev")) {
                        if (path == "/dev") {
                                node.kind = Kind::DevDir;
                        } else if (path.lastIndexOf('/') != 4) {
                                if (!findDeviceEntry(path, node.devDir, node.devIndex))
                                        return makeError<Node>(Error::NotExist);
                                node.kind = Kind::DevSubFile;
                        } else if (const DevNode *dev = findDevNode(node.name)) {
                                node.kind = dev->stream != nullptr ? Kind::DevStream : Kind::DevConsole;
                                node.stream = dev->stream;
                        } else if (const Vfs::DeviceFile *file = findDeviceFile(node.name)) {
                                node.kind = Kind::DevFile;
                                node.file = file;
                        } else if (const Vfs::DeviceDirectory *dir = findDeviceDir(node.name)) {
                                node.kind = Kind::DevSubDir;
                                node.devDir = dir;
                        } else {
                                const BlockDevices::Entry *e = Fs::findEntryLocked(node.name);
                                if (e == nullptr) return makeError<Node>(Error::NotExist);
                                node.kind = Kind::DevBlock;
                                node.device = e->device;
                        }
                        return makeResult(node);
                }
                if (isAtOrBelow(path, BinPath)) {
                        if (path == BinPath) {
                                node.kind = Kind::BinDir;
                                return makeResult(node);
                        }
                        node.program =
                                path.lastIndexOf('/') == BinPath.size() ? BuiltinPrograms::find(node.name) : nullptr;
                        if (node.program == nullptr) return makeError<Node>(Error::NotExist);
                        node.kind = Kind::BinProgram;
                        return makeResult(node);
                }
                if (isAtOrBelow(path, AppsPath)) {
                        if (path == AppsPath) {
                                node.kind = Kind::AppsDir;
                                return makeResult(node);
                        }
                        if (path.lastIndexOf('/') != AppsPath.size() || appTargetLocked(node.name).isEmpty())
                                return makeError<Node>(Error::NotExist);
                        node.kind = Kind::AppLink;
                        return makeResult(node);
                }
                if (isAtOrBelow(path, Vfs::EtcPath)) {
                        if (path == Vfs::EtcPath) {
                                node.kind = Kind::EtcDir;
                                return makeResult(node);
                        }
                        node.etc = path.lastIndexOf('/') == Vfs::EtcPath.size() ? Etc::find(node.name) : nullptr;
                        if (node.etc == nullptr) return makeError<Node>(Error::NotExist);
                        node.kind = Kind::EtcFile;
                        return makeResult(node);
                }
                if (isAtOrBelow(path, Vfs::TrashPath)) {
                        // Its entries were rewritten to their files: what is left has no trash.
                        if (path != Vfs::TrashPath) return makeError<Node>(Error::NotExist);
                        node.kind = Kind::TrashDir;
                        return makeResult(node);
                }
                if (isAtOrBelow(path, ZoneInfoPath)) {
                        Zones::Found found;
                        const Error  err = Zones::findLocked(
                                path == ZoneInfoPath ? StringView() : path.mid(ZoneInfoPath.size() + 1), found);
                        if (err.isError()) return makeError<Node>(err);
                        node.kind = found.directory ? Kind::ZoneDir : Kind::ZoneFile;
                        node.zoneOffset = found.entry.offset;
                        node.zoneLength = found.entry.length;
                        node.zoneModified = found.modified;
                        return makeResult(node);
                }
                for (size_t i = 0; i < mountCount; ++i) {
                        const StringView mp(mounts[i].path);
                        if (isAtOrBelow(path, mp)) {
                                node.kind = Kind::Mounted;
                                node.mount = static_cast<uint32_t>(i + 1);
                                node.volume = mounts[i].volume;
                                node.device = mounts[i].device;
                                node.rest = path.mid(mp.size());
                                return makeResult(node);
                        }
                }
                // A directory leading to a mount, or to the time zone database, is virtual.
                if (path == SharePath) return makeResult(node);
                for (size_t i = 0; i < mountCount; ++i) {
                        if (isBelow(StringView(mounts[i].path), path)) return makeResult(node);
                }
                return makeError<Node>(Error::NotExist);
        }

        // --- /home (devplan/sysconf.md §7) ------------------------------------------------

        bool isMountedLocked(StringView path) {
                const Result<Node> found = resolveLocked(path);
                return found.error().isOk() && found.value().kind == Kind::Mounted;
        }

        // Where /home leads: its target, else (its filesystem not mounted) the fallback. Lock held.
        StringView homeLocked() {
                if (homeTarget == nullptr) return StringView();
                if (isMountedLocked(StringView(homeTarget))) return StringView(homeTarget);
                if (homeFallback != nullptr && isMountedLocked(StringView(homeFallback)))
                        return StringView(homeFallback);
                return StringView(homeTarget);
        }

        // Where root link @p name ("home", "cfg") leads. Lock held.
        StringView rootLinkLocked(StringView name) {
                if (name == "home") return homeLocked();
                return cfgTarget != nullptr ? StringView(cfgTarget) : StringView();
        }

        // Reports where /home leads when that changed (setHome(), a mount, an unmount). Lock held.
        void logHomeLocked() {
                if (homeTarget == nullptr) {
                        homeShown = 0;
                        return;
                }
                const StringView leads = homeLocked();
                const uint8_t    shown = leads == StringView(homeTarget) ? 1 : 2;
                if (shown == homeShown) return;
                homeShown = shown;
                if (homeReport != nullptr) homeReport(leads, shown == 1);
                return;
        }

        // Rewrites normalized @p storage at or below /home or /cfg to where it leads; the link
        // itself only with @p followHome (else the link is what is meant). Lock held.
        Error homeRewriteLocked(String &storage, bool followHome) {
                // /trash/NAME: its file in its filesystem's trash (FB20).
                if (isBelow(storage.view(), Vfs::TrashPath)) {
                        const StringView  rest = storage.view().mid(Vfs::TrashPath.size() + 1);
                        const size_t      slash = rest.indexOf('/');
                        TrashIndex::Mount m[MaxMounts];
                        size_t            slot = 0;
                        StringView        real;
                        if (TrashIndex::findLocked(trashMountsLocked(m),
                                                   slash == StringView::NPos ? rest : rest.left(slash), slot, real)) {
                                String out;
                                Error  err = out.append(StringView(mounts[slot].path));
                                if (err.isOk()) err = out.append(TrashIndex::FilesRest);
                                if (err.isOk()) err = out.append('/');
                                if (err.isOk()) err = out.append(real);
                                if (err.isOk() && slash != StringView::NPos) err = out.append(rest.mid(slash));
                                if (err.isError()) return err;
                                storage = std::move(out);
                        }
                        return Error::Ok;
                }
                // /apps/NAME first: where it leads may be below /home.
                if (isBelow(storage.view(), AppsPath)) {
                        const StringView rest = storage.view().mid(AppsPath.size() + 1);
                        const size_t     slash = rest.indexOf('/');
                        const StringView name = slash == StringView::NPos ? rest : rest.left(slash);
                        const StringView target = appTargetLocked(name);
                        if (!target.isEmpty() && (slash != StringView::NPos || followHome)) {
                                String out;
                                Error  err = out.append(target);
                                if (err.isOk() && slash != StringView::NPos) err = out.append(rest.mid(slash));
                                if (err.isError()) return err;
                                storage = std::move(out);
                        }
                }
                StringView link;
                if (homeTarget != nullptr && isAtOrBelow(storage.view(), HomePath)) link = HomePath;
                if (cfgTarget != nullptr && isAtOrBelow(storage.view(), CfgPath)) link = CfgPath;
                if (link.isEmpty()) return Error::Ok;
                if (storage.size() == link.size() && !followHome) return Error::Ok;
                String out;
                Error  err = out.append(rootLinkLocked(link.mid(1)));
                if (err.isOk()) err = out.append(storage.view().mid(link.size()));
                if (err.isOk()) storage = std::move(out);
                return err;
        }

        // Normalizes @p path (against @p cwd) into @p storage, past /home. Lock held.
        Error normalizeLocked(StringView path, StringView cwd, String &storage, bool followHome) {
                Result<String> normalized = Vfs::normalize(path, cwd);
                if (normalized.error().isError()) return normalized.error();
                storage = std::move(normalized).value();
                return homeRewriteLocked(storage, followHome);
        }

        // Normalizes @p path into @p storage and resolves it. Lock held.
        Result<Node> lookupLocked(StringView path, const Vfs::Context &context, String &storage) {
                const Error err = normalizeLocked(path, context.cwd, storage, true);
                if (err.isError()) return makeError<Node>(err);
                return resolveLocked(storage.view());
        }

        // True if normalized @p path, which doesn't exist, is in a virtual
        // directory (outside every filesystem), where nothing can be made.
        bool outsideFilesystemsLocked(StringView path) {
                if (path.isEmpty()) return false;
                const size_t       slash = path.lastIndexOf('/');
                const Result<Node> parent = resolveLocked(slash == 0 ? StringView("/") : path.left(slash));
                return parent.error().isOk() && parent.value().kind != Kind::Mounted;
        }

        FileInfo directoryInfo(StringView name) {
                FileInfo info;
                (void)info.name.assign(name);
                info.type = FileType::Directory;
                return info;
        }

        Result<FileInfo> statLocked(const Node &node, bool *noPath = nullptr) {
                FileInfo info;
                if (noPath != nullptr) *noPath = false;
                switch (node.kind) {
                        case Kind::Virtual:
                        case Kind::BinDir:
                        case Kind::EtcDir:
                        case Kind::DevSubDir:
                        case Kind::AppsDir:
                        case Kind::TrashDir:
                        case Kind::DevDir: return makeResult(directoryInfo(node.name));
                        case Kind::AppLink:
                                (void)info.name.assign(node.name);
                                info.type = FileType::Link;
                                info.size = appTargetLocked(node.name).size();
                                return makeResult(std::move(info));
                        case Kind::EtcFile:
                                (void)info.name.assign(node.name);
                                info.type = FileType::File; // size 0: built when opened
                                return makeResult(std::move(info));
                        case Kind::BinProgram:
                                (void)info.name.assign(node.name);
                                info.type = FileType::File; // empty: run, not read
                                return makeResult(std::move(info));
                        case Kind::ZoneDir:
                                info = directoryInfo(node.name);
                                info.modified = node.zoneModified;
                                return makeResult(std::move(info));
                        case Kind::ZoneFile:
                                (void)info.name.assign(node.name);
                                info.type = FileType::File;
                                info.size = node.zoneLength;
                                info.modified = node.zoneModified;
                                return makeResult(std::move(info));
                        case Kind::DevFile:
                                (void)info.name.assign(node.name);
                                info.type = node.file->isDevice ? FileType::Device : FileType::File;
                                if (node.file->size != nullptr) info.size = node.file->size(node.file->arg);
                                return makeResult(std::move(info));
                        case Kind::RootLink:
                                (void)info.name.assign(node.name);
                                info.type = FileType::Link;
                                info.size = rootLinkLocked(node.name).size();
                                return makeResult(std::move(info));
                        case Kind::DevStream:
                        case Kind::DevConsole:
                        case Kind::DevSubFile:
                        case Kind::DevBlock:
                                (void)info.name.assign(node.name);
                                info.type = FileType::Device;
                                if (node.device != nullptr)
                                        info.size = node.device->sectorCount() * node.device->sectorSize();
                                return makeResult(std::move(info));
                        default: break;
                }
                if (node.rest.isEmpty()) return makeResult(directoryInfo(node.name));
                bool        blocked = false;
                const Error err = callVolume(*node.volume, [&] { return node.volume->stat(node.rest, info, blocked); });
                if (noPath != nullptr) *noPath = blocked;
                if (err.isError()) return makeError<FileInfo>(err);
                return makeResult(std::move(info));
        }

        // --- Symbolic links (devplan/storage.md §4) ------------------------------
        //
        // Cygwin's system-file links: the System attribute, "!<symlink>", then
        // FF FE and the target in UTF-16LE with a 16-bit NUL (or, read only,
        // the target in UTF-8). Looked for only where they can be: in a file
        // with the System attribute that a lookup found, or along a path that
        // the filesystem reports as "no path" (a file where a directory should be).

        constexpr int    MaxLinks = 8;     // in a row, then TooManyLinks
        constexpr size_t MaxTarget = 1024; // bytes of UTF-8 (BMDS_PATH_MAX)
        constexpr char   Cookie[] = "!<symlink>";
        constexpr size_t CookieBytes = sizeof(Cookie) - 1;
        using Fs::MaxLinkFile;
        static_assert(MaxLinkFile == CookieBytes + 2 + 2 * MaxTarget + 2, "the link file's limit");

        bool linkCandidate(uint8_t attributes, FileType type, uint64_t size) {
                return Fs::isLinkCandidate(attributes, type, size);
        }

        // UTF-16LE (with surrogate pairs) to UTF-8.
        Error fromUtf16(const uint8_t *p, size_t units, String &out) {
                for (size_t i = 0; i < units; ++i) {
                        uint32_t c = static_cast<uint32_t>(p[2 * i] | p[2 * i + 1] << 8);
                        if (c == 0) break;
                        if (c >= 0xD800 && c < 0xDC00) {
                                if (i + 1 == units) return Error::Invalid;
                                const uint32_t low = static_cast<uint32_t>(p[2 * i + 2] | p[2 * i + 3] << 8);
                                if (low < 0xDC00 || low >= 0xE000) return Error::Invalid;
                                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                                ++i;
                        } else if (c >= 0xDC00 && c < 0xE000) {
                                return Error::Invalid;
                        }
                        char   b[4];
                        size_t n = 0;
                        if (c < 0x80) {
                                b[n++] = static_cast<char>(c);
                        } else if (c < 0x800) {
                                b[n++] = static_cast<char>(0xC0 | c >> 6);
                                b[n++] = static_cast<char>(0x80 | (c & 0x3F));
                        } else if (c < 0x10000) {
                                b[n++] = static_cast<char>(0xE0 | c >> 12);
                                b[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                                b[n++] = static_cast<char>(0x80 | (c & 0x3F));
                        } else {
                                b[n++] = static_cast<char>(0xF0 | c >> 18);
                                b[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                                b[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                                b[n++] = static_cast<char>(0x80 | (c & 0x3F));
                        }
                        const Error err = out.append(StringView(b, n));
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // UTF-8 to UTF-16LE into @p out; the number of bytes written, or 0 if invalid.
        size_t toUtf16(StringView text, uint8_t *out, size_t room) {
                size_t n = 0;
                for (size_t i = 0; i < text.size();) {
                        const uint8_t b = static_cast<uint8_t>(text[i]);
                        uint32_t      c = 0;
                        size_t        len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
                        if (len == 0 || i + len > text.size()) return 0;
                        c = len == 1 ? b : b & (0x7F >> len);
                        for (size_t k = 1; k < len; ++k) {
                                const uint8_t t = static_cast<uint8_t>(text[i + k]);
                                if ((t & 0xC0) != 0x80) return 0;
                                c = c << 6 | (t & 0x3F);
                        }
                        i += len;
                        if (c >= 0x10000) {
                                if (n + 4 > room) return 0;
                                const uint32_t v = c - 0x10000;
                                const uint32_t hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
                                out[n++] = static_cast<uint8_t>(hi);
                                out[n++] = static_cast<uint8_t>(hi >> 8);
                                out[n++] = static_cast<uint8_t>(lo);
                                out[n++] = static_cast<uint8_t>(lo >> 8);
                        } else {
                                if (n + 2 > room || c == 0) return 0;
                                out[n++] = static_cast<uint8_t>(c);
                                out[n++] = static_cast<uint8_t>(c >> 8);
                        }
                }
                return n;
        }

        // The target of the link file at @p path on @p volume; Invalid if it isn't one. Lock held.
        Error readLinkFile(Volume &volume, StringView path, String &target) {
                target.clear();
                auto [reader, err] = volume.openReader(path);
                if (err.isError()) return err;
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxLinkFile));
                if (buf == nullptr) {
                        volume.closeReader(reader);
                        return Error::NoMem;
                }
                auto [got, rerr] = reader->readAt(0, WritableByteSpan(buf, MaxLinkFile));
                volume.closeReader(reader);
                err = rerr.isOk() ? Fs::parseLink(buf, got, target) : rerr;
                Memory::free(buf);
                return err;
        }

        // An emulated link's target (its file read whole). Lock held.
        Error readLinkLocked(Volume &volume, StringView path, String &target) {
                return callVolume(volume, [&] { return readLinkFile(volume, path, target); });
        }

        // Link targets read lately (devplan/programs.md §19): a link is a small file read whole, and
        // file data skips the sector cache, so each lookup through one read the card (~0.9 ms).
        // An entry holds while its file's size, time and attributes are as when it was read; the
        // VFS drops them all on any change it reports (Fs::changedLocked()).
        struct LinkEntry {
                        const Volume *volume = nullptr;
                        uint64_t      size = 0;
                        int64_t       modified = 0;
                        uint8_t       attributes = 0;
                        uint8_t       pathBytes = 0;
                        uint8_t       targetBytes = 0;
                        char          path[96];
                        char          target[96];
        };
        constexpr size_t     LinkEntries = 32;
        constinit LinkEntry *linkCache = nullptr; // PSRAM, from the first link read; under the lock
        constinit size_t     linkNext = 0;        // the next one replaced, round the ring

        void dropLinksLocked() {
                if (linkCache != nullptr)
                        for (size_t i = 0; i < LinkEntries; ++i) linkCache[i].volume = nullptr;
                return;
        }

        // The target of link file @p path on @p volume, @p entry being what a lookup found there:
        // from the cache, else read and kept. Lock held.
        Error readLinkCachedLocked(Volume &volume, StringView path, const FileInfo &entry, String &target) {
                const auto matches = [&](const LinkEntry &e) {
                        return e.volume == &volume && e.size == entry.size && e.modified == entry.modified.ns &&
                               e.attributes == entry.attributes && StringView(e.path, e.pathBytes) == path;
                };
                if (linkCache != nullptr) {
                        for (size_t i = 0; i < LinkEntries; ++i) {
                                if (!matches(linkCache[i])) continue;
                                target.clear();
                                return target.append(StringView(linkCache[i].target, linkCache[i].targetBytes));
                        }
                }
                const Error err = readLinkLocked(volume, path, target);
                if (err.isError() || path.size() > sizeof(LinkEntry::path) || target.size() > sizeof(LinkEntry::target))
                        return err;
                if (linkCache == nullptr) {
                        linkCache = static_cast<LinkEntry *>(
                                Memory::alloc(MemSpace::Psram, sizeof(LinkEntry) * LinkEntries));
                        if (linkCache == nullptr) return err;
                        for (size_t i = 0; i < LinkEntries; ++i) ::new (&linkCache[i]) LinkEntry();
                }
                LinkEntry &e = linkCache[linkNext];
                linkNext = (linkNext + 1) % LinkEntries;
                e.volume = &volume;
                e.size = entry.size;
                e.modified = entry.modified.ns;
                e.attributes = entry.attributes;
                e.pathBytes = static_cast<uint8_t>(path.size());
                e.targetBytes = static_cast<uint8_t>(target.size());
                std::memcpy(e.path, path.data(), path.size());
                std::memcpy(e.target, target.view().data(), target.size());
                return err;
        }

        // Writes a new link file at @p path on @p volume. Lock held.
        Error writeLinkLocked(Volume &volume, StringView path, StringView target, bool &noPath) {
                noPath = false;
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxLinkFile));
                if (buf == nullptr) return Error::NoMem;
                std::memcpy(buf, Cookie, CookieBytes);
                buf[CookieBytes] = 0xFF;
                buf[CookieBytes + 1] = 0xFE;
                const size_t n = toUtf16(target, buf + CookieBytes + 2, 2 * MaxTarget);
                buf[CookieBytes + 2 + n] = 0;
                buf[CookieBytes + 3 + n] = 0;
                const Error err = n == 0 ? Error(Error::Invalid) : callVolume(volume, [&] {
                        return volume.writeLink(path, ByteSpan(buf, CookieBytes + 2 + n + 2), noPath);
                });
                Memory::free(buf);
                return err;
        }

        // The link @p link (a normalized path) points at, with @p rest after it.
        Error linkDestination(StringView link, StringView target, StringView rest, String &out) {
                String joined;
                Error  err = Error::Ok;
                if (!target.startsWith('/')) {
                        const size_t slash = link.lastIndexOf('/');
                        err = joined.append(slash == 0 ? StringView("/") : link.left(slash));
                        if (err.isOk()) err = joined.append('/');
                }
                if (err.isOk()) err = joined.append(target);
                if (err.isOk()) err = joined.append(rest);
                if (err.isError()) return err;
                return normalizeLocked(joined, "/", out, true);
        }

        // Finds the first link among the components of normalized @p storage
        // (the last too with @p last) and replaces @p storage with where it
        // leads. True if there was one. Lock held.
        bool followPathLocked(String &storage, bool last) {
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error().isError() || found.value().kind != Kind::Mounted) return false;
                const Node      &node = found.value();
                const StringView rest = node.rest;
                const size_t     mountLength = storage.size() - rest.size();
                bool             followed = false;
                FileInfo         entry;
                for (size_t pos = 1; pos <= rest.size();) {
                        size_t     end = rest.indexOf('/', pos);
                        const bool isLast = end == StringView::NPos;
                        if (isLast) end = rest.size();
                        if (isLast && !last) break;
                        bool noPath = false;
                        if (callVolume(*node.volume, [&] {
                                    return node.volume->stat(rest.left(end), entry, noPath);
                            }).isError())
                                break;
                        if (!entry.isDirectory()) {
                                String target;
                                if (linkCandidate(entry.attributes, entry.type, entry.size) &&
                                    readLinkCachedLocked(*node.volume, rest.left(end), entry, target).isOk()) {
                                        String           next;
                                        const StringView link = storage.view().left(mountLength + end);
                                        if (linkDestination(link, target, rest.mid(end), next).isOk()) {
                                                storage = std::move(next);
                                                followed = true;
                                        }
                                }
                                break;
                        }
                        pos = end + 1;
                }
                return followed;
        }

        // What normalized @p storage names, its links followed (the last one's
        // with @p followLast, else described as a link). @p storage ends as the
        // path of what was found; @p node resolves it. Lock held.
        Result<FileInfo> statFollowLocked(String &storage, bool followLast, Node &node) {
                for (int hops = 0;; ++hops) {
                        if (hops > MaxLinks) return makeError<FileInfo>(Error::TooManyLinks);
                        const Result<Node> found = resolveLocked(storage.view());
                        if (found.error().isError()) return makeError<FileInfo>(found.error());
                        node = found.value();
                        bool             noPath = false;
                        Result<FileInfo> info = statLocked(node, &noPath);
                        if (info.error().isOk()) {
                                FileInfo &fi = info.value();
                                if (node.kind != Kind::Mounted || !linkCandidate(fi.attributes, fi.type, fi.size))
                                        return info;
                                String target;
                                if (readLinkCachedLocked(*node.volume, node.rest, fi, target).isError()) return info;
                                if (!followLast) {
                                        fi.type = FileType::Link;
                                        fi.size = target.size();
                                        return info;
                                }
                                String next;
                                const Error err = linkDestination(storage.view(), target, StringView(), next);
                                if (err.isError()) return makeError<FileInfo>(err);
                                storage = std::move(next);
                                continue;
                        }
                        if (noPath && followPathLocked(storage, false)) continue;
                        return info;
                }
        }

        // The next component of mount path @p mp below virtual directory @p dir.
        StringView childOf(StringView mp, StringView dir) {
                const StringView tail = mp.mid(dir == "/" ? 1 : dir.size() + 1);
                return tail.left(tail.indexOf('/'));
        }

        // The child names of virtual directory @p dir, in order without
        // repeats: "bin", "dev" and "share" in /, "zoneinfo" in /share, then
        // the next component of each mount below. Returns child @p index, or
        // an empty view past the end. Lock held.
        StringView virtualChild(StringView dir, size_t index) {
                static constexpr StringView InRoot[] = {"apps", "bin", "cfg", "dev", "etc", "home", "share", "trash"};
                static constexpr StringView InShare[] = {"zoneinfo"};
                StringView                  root[sizeof(InRoot) / sizeof(InRoot[0])];
                size_t                      rootCount = 0;
                for (const StringView name : InRoot) {
                        if (name == "home" && homeTarget == nullptr) continue;
                        if (name == "cfg" && cfgTarget == nullptr) continue;
                        root[rootCount++] = name;
                }
                Span<const StringView> fixed;
                if (dir == "/") fixed = Span<const StringView>(root, rootCount);
                if (dir == SharePath) fixed = InShare;
                if (index < fixed.size()) return fixed[index];
                size_t emitted = fixed.size();
                for (size_t i = 0; i < mountCount; ++i) {
                        const StringView mp(mounts[i].path);
                        if (!isBelow(mp, dir)) continue;
                        const StringView child = childOf(mp, dir);
                        bool             repeat = false;
                        for (const StringView f : fixed) repeat = repeat || f == child;
                        for (size_t j = 0; j < i && !repeat; ++j) {
                                const StringView other(mounts[j].path);
                                repeat = isBelow(other, dir) && childOf(other, dir) == child;
                        }
                        if (repeat) continue;
                        if (emitted == index) return child;
                        ++emitted;
                }
                return StringView();
        }

        void removeMountLocked(size_t index) {
                mounts[index].volume->unmounted();
                mounts[index].volume->release();
                for (size_t j = index + 1; j < mountCount; ++j) mounts[j - 1] = mounts[j];
                mounts[--mountCount] = Mount();
                trashMountsChangedLocked();
                // Every file: the slots after this one moved.
                Fs::changedLocked(0, 0);
                Fs::tellAllWatchesLocked();
                logHomeLocked();
                mountChanges.notify();
                return;
        }

        // True if mount @p index is on @p device or on a partition of it. Lock held.
        bool isMountOn(size_t index, const BlockDevice &device) {
                const BlockDevice *mounted = mounts[index].device;
                if (mounted == nullptr) return false;
                const BlockDevices::Entry *e = Fs::findEntryLocked(*mounted);
                return mounted == &device || (e != nullptr && e->parent == &device);
        }

        // True if mount @p index shares sectors with @p device: the same
        // device, a partition of it, or the whole device it is part of. Lock held.
        bool overlaps(size_t index, const BlockDevice &device) {
                const BlockDevices::Entry *e = Fs::findEntryLocked(device);
                return isMountOn(index, device) ||
                       (e != nullptr && mounts[index].device != nullptr && e->parent == mounts[index].device);
        }

} // namespace

// --- Directory -------------------------------------------------------------

// Holds the anonymous-namespace Kind as a plain value (a public class's
// member can't have an internal-linkage type).
struct Directory::State {
                uint8_t    kind = 0;
                uint8_t    index = 0; // Virtual directories: the next entry.
                uint32_t   device = 0;
                uint64_t   idPrefix = IdSeed; // The directory's path hashed (children: + "/name").
                Volume         *volume = nullptr;
                Volume::Cursor *cursor = nullptr;            // Mounted
                char            path[MaxMountPath + 1] = {}; // Virtual directories.
                const Vfs::DeviceDirectory *devDir = nullptr;            // DevSubDir
                String     zones{MemSpace::Psram}; // ZoneDir: its children (Zones::listLocked())
                size_t     zoneAt = 0;             // ZoneDir: the next record
                Timestamp  zoneModified;
};

class VfsAccess {
        public:
                static Directory make(Directory::State *state) {
                        Directory d;
                        d._state = state;
                        return d;
                }
};

Directory::~Directory() {
        close();
}

Directory::Directory(Directory &&other) : _state(std::exchange(other._state, nullptr)) {}

Directory &Directory::operator=(Directory &&other) {
        if (this != &other) {
                close();
                _state = std::exchange(other._state, nullptr);
        }
        return *this;
}

void Directory::close() {
        if (_state == nullptr) return;
        const Kind kind = static_cast<Kind>(_state->kind);
        if (kind == Kind::Mounted && _state->volume->locksItself()) {
                // Its own lock; the reference the open took let go after.
                _state->volume->closeDirectory(_state->cursor);
                _state->volume->release();
        } else if (kind == Kind::Mounted || (kind == Kind::TrashDir && _state->cursor != nullptr)) {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                // A stale directory (unmounted) is only freed.
                _state->volume->closeDirectory(_state->cursor);
        }
        _state->~State();
        Memory::free(_state);
        _state = nullptr;
        return;
}

Result<FileInfo> Directory::next() {
        if (_state == nullptr) return makeError<FileInfo>(Error::InvalidState);
        State        &s = *_state;
        FileInfo      info;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        switch (static_cast<Kind>(s.kind)) {
                case Kind::Virtual: {
                        const StringView child = virtualChild(StringView(s.path), s.index);
                        if (child.isEmpty()) return makeError<FileInfo>(Error::EndOfFile);
                        ++s.index;
                        info = directoryInfo(child);
                        if (StringView(s.path) == "/" &&
                            ((child == "home" && homeTarget != nullptr) || (child == "cfg" && cfgTarget != nullptr))) {
                                info.type = FileType::Link;
                                info.size = rootLinkLocked(child).size();
                        }
                        info.id = idAppend(idAppend(s.idPrefix, "/"), child);
                        return makeResult(std::move(info));
                }
                case Kind::EtcDir: {
                        if (s.index >= Etc::count()) return makeError<FileInfo>(Error::EndOfFile);
                        (void)info.name.assign(Etc::at(s.index).name);
                        info.type = FileType::File;
                        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
                        ++s.index;
                        return makeResult(std::move(info));
                }
                case Kind::AppsDir: {
                        if (s.index >= appCountLocked()) return makeError<FileInfo>(Error::EndOfFile);
                        (void)info.name.assign(appNameLocked(s.index));
                        info.type = FileType::Link;
                        info.size = appTargetLocked(info.name.view()).size();
                        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
                        ++s.index;
                        return makeResult(std::move(info));
                }
                case Kind::BinDir: {
                        const Span<const ProgramEntry> programs = BuiltinPrograms::all();
                        if (s.index >= programs.size()) return makeError<FileInfo>(Error::EndOfFile);
                        (void)info.name.assign(programs[s.index].name);
                        info.type = FileType::File;
                        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
                        ++s.index;
                        return makeResult(std::move(info));
                }
                case Kind::ZoneDir: {
                        if (s.zoneAt >= s.zones.size()) return makeError<FileInfo>(Error::EndOfFile);
                        const char      *record = s.zones.data() + s.zoneAt;
                        const uint8_t   *size = reinterpret_cast<const uint8_t *>(record + 1);
                        const StringView name(record + Zones::RecordHeader);
                        s.zoneAt += Zones::RecordHeader + name.size() + 1;
                        info = record[0] == 'd' ? directoryInfo(name) : FileInfo();
                        if (record[0] != 'd') {
                                (void)info.name.assign(name);
                                info.type = FileType::File;
                                info.size = static_cast<uint32_t>(size[0] | size[1] << 8 | size[2] << 16) |
                                            static_cast<uint32_t>(size[3]) << 24;
                        }
                        info.modified = s.zoneModified;
                        info.id = idAppend(idAppend(s.idPrefix, "/"), name);
                        return makeResult(std::move(info));
                }
                case Kind::TrashDir: {
                        // Each filesystem's trashed files in turn, those with an info file,
                        // by their flat names; their ids their files' (FB20).
                        TrashIndex::Mount                   m[MaxMounts];
                        const Span<const TrashIndex::Mount> slots = trashMountsLocked(m);
                        for (;;) {
                                if (s.cursor == nullptr) {
                                        if (s.index >= slots.size()) return makeError<FileInfo>(Error::EndOfFile);
                                        if (slots[s.index].volume == nullptr) {
                                                ++s.index;
                                                continue;
                                        }
                                        bool noPath = false;
                                        auto [cursor, err] =
                                                slots[s.index].volume->openDirectory(TrashIndex::FilesRest, noPath);
                                        if (err.isError()) {
                                                ++s.index;
                                                continue;
                                        }
                                        s.cursor = cursor;
                                        s.volume = slots[s.index].volume;
                                }
                                const Error err = s.cursor->next(info);
                                if (err.isError()) {
                                        s.volume->closeDirectory(s.cursor);
                                        s.cursor = nullptr;
                                        ++s.index;
                                        if (err != Error::EndOfFile) return makeError<FileInfo>(err);
                                        continue;
                                }
                                const StringView flat = TrashIndex::flatNameLocked(slots, s.index, info.name.view());
                                if (flat.isEmpty()) continue;
                                if (linkCandidate(info.attributes, info.type, info.size) &&
                                    (info.attributes & FileInfo::Hidden) == 0) {
                                        info.type = FileType::Link;
                                        info.size = (info.size - CookieBytes - 4) / 2;
                                }
                                info.id = idAppend(
                                        idAppend(idAppend(pathId(slots[s.index].path), TrashIndex::FilesRest), "/"),
                                        info.name.view());
                                info.device = static_cast<uint32_t>(s.index + 1);
                                (void)info.name.assign(flat);
                                return makeResult(std::move(info));
                        }
                }
                case Kind::DevSubDir: {
                        const Vfs::DeviceDirectory &d = *s.devDir;
                        while (s.index < d.count && d.present != nullptr && !d.present(d.arg, s.index)) ++s.index;
                        if (s.index >= d.count) return makeError<FileInfo>(Error::EndOfFile);
                        char      number[8];
                        const int n = std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(s.index));
                        (void)info.name.assign(StringView(number, static_cast<size_t>(n)));
                        info.type = FileType::Device;
                        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
                        ++s.index;
                        return makeResult(std::move(info));
                }
                case Kind::DevDir: {
                        constexpr size_t Fixed = sizeof(DevNodes) / sizeof(DevNodes[0]);
                        const size_t     added = deviceFileCount.load(std::memory_order_acquire);
                        // Added files that come and go, skipped while gone.
                        while (s.index >= Fixed && s.index - Fixed < added && !isPresent(deviceFiles[s.index - Fixed]))
                                ++s.index;
                        if (s.index < Fixed) {
                                (void)info.name.assign(DevNodes[s.index].name);
                                // The console's id is its stream's, unknown here.
                                if (DevNodes[s.index].stream != nullptr) info.id = objectId(DevNodes[s.index].stream);
                        } else if (s.index - Fixed < added) {
                                const Vfs::DeviceFile &f = deviceFiles[s.index - Fixed];
                                (void)info.name.assign(f.name);
                                if (f.size != nullptr) info.size = f.size(f.arg);
                                info.id = objectId(&f);
                                ++s.index;
                                info.type = f.isDevice ? FileType::Device : FileType::File;
                                return makeResult(std::move(info));
                        } else if (s.index - Fixed - added < deviceDirCount.load(std::memory_order_acquire)) {
                                const Vfs::DeviceDirectory &d = deviceDirs[s.index - Fixed - added];
                                ++s.index;
                                info = directoryInfo(StringView(d.name));
                                info.id = idAppend(idAppend(s.idPrefix, "/"), StringView(d.name));
                                return makeResult(std::move(info));
                        } else if (s.index - Fixed - added - deviceDirCount.load(std::memory_order_acquire) <
                                   Fs::deviceCountLocked()) {
                                const BlockDevices::Entry &e = Fs::deviceAtLocked(
                                        s.index - Fixed - added - deviceDirCount.load(std::memory_order_acquire));
                                (void)info.name.assign(e.name);
                                info.size = e.device->sectorCount() * e.device->sectorSize();
                                info.id = objectId(e.device);
                        } else {
                                return makeError<FileInfo>(Error::EndOfFile);
                        }
                        ++s.index;
                        info.type = FileType::Device;
                        return makeResult(std::move(info));
                }
                default: break;
        }
        const Error err = callVolume(*s.volume, [&] { return s.cursor->next(info); });
        if (err.isError()) return makeError<FileInfo>(err);
        // A link by its attributes alone: reading every link to be sure made
        // listing a directory of BusyBox's applets take seconds (each read is
        // a lookup in that directory). System files Windows makes are also
        // Hidden; lstat() and readLink() read the cookie when it matters.
        if (linkCandidate(info.attributes, info.type, info.size) && (info.attributes & FileInfo::Hidden) == 0) {
                info.type = FileType::Link;
                info.size = (info.size - CookieBytes - 4) / 2; // the target's length, if ASCII
        }
        info.id = idAppend(idAppend(s.idPrefix, "/"), info.name.view());
        info.device = s.device;
        return makeResult(std::move(info));
}

// --- Vfs: paths and mounts --------------------------------------------------

void Fs::changedLocked(uint32_t device, uint64_t id) {
        dropLinksLocked();
        Zones::changedLocked(device, id);
        if (changeHook != nullptr) changeHook(device, id);
        return;
}

uint64_t Fs::pathId(StringView normalized) {
        return bmds::pathId(normalized);
}

Error Fs::openReaderLocked(StringView path, Volume *&volume, Volume::Reader *&reader, Timestamp *modified) {
        const Result<Node> found = resolveLocked(path);
        if (found.error().isError()) return found.error();
        const Node &node = found.value();
        if (node.kind != Kind::Mounted) return Error::NotExist;
        if (node.rest.isEmpty()) return Error::IsDir;
        const Result<FileInfo> info = statLocked(node);
        if (info.error().isError()) return info.error();
        if (info.value().isDirectory()) return Error::IsDir;
        if (modified != nullptr) *modified = info.value().modified;
        auto [opened, err] = callVolume(*node.volume, [&] { return node.volume->openReader(node.rest); });
        if (err.isError()) return err;
        volume = node.volume;
        reader = opened;
        return Error::Ok;
}

bool Fs::isLinkCandidate(uint8_t attributes, FileType type, uint64_t size) {
        return (attributes & FileInfo::System) != 0 && type == FileType::File && size > CookieBytes &&
               size <= MaxLinkFile;
}

void Vfs::setChangeHook(ChangeHook hook) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        changeHook = hook;
        return;
}

Error Vfs::init() {
        const Error err = Fs::init();
        if (err.isOk()) Zones::installResolver(); // zone names in the kernel's zone (devplan/time.md §9)
        return err;
}

Error Vfs::sync() {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        Error         first = Fat::flushAllLocked();
        const Error   lost = Fat::takeLostLocked(); // a write-back since the last sync failed
        if (first.isOk()) first = lost;
        for (size_t i = 0; i < mountCount; ++i) {
                if (mounts[i].device == nullptr) continue;
                const Error err = mounts[i].device->sync();
                if (first.isOk() && err != Error::NotSupported) first = err;
        }
        return first;
}

Result<String> Vfs::normalize(StringView path, StringView cwd) {
        if (path.isEmpty()) return makeError<String>(Error::Invalid);
        String out;
        Error  err = out.reserve(path.size() + (path[0] == '/' ? 0 : cwd.size()) + 1);
        // Walks @p text's components into `out`.
        auto walk = [&](StringView text) {
                size_t i = 0;
                while (err.isOk() && i < text.size()) {
                        size_t end = text.indexOf('/', i);
                        if (end == StringView::NPos) end = text.size();
                        const StringView part = text.mid(i, end - i);
                        i = end + 1;
                        if (part.isEmpty() || part == ".") continue;
                        if (part == "..") {
                                const size_t slash = out.view().lastIndexOf('/');
                                out.truncate(slash == StringView::NPos ? 0 : slash);
                                continue;
                        }
                        err = out.append('/');
                        if (err.isOk()) err = out.append(part);
                }
        };
        if (path[0] != '/') walk(cwd);
        walk(path);
        if (err.isOk() && out.isEmpty()) err = out.append('/');
        if (err.isError()) return makeError<String>(err);
        if (out.size() > MaxPath) return makeError<String>(Error::OutOfRange);
        return makeResult(std::move(out));
}

namespace {

        Error mountAt(StringView path, BlockDevice *device, uint64_t limitBytes, Volume *given = nullptr);
        Error mountGiven(StringView path, BlockDevice *device, uint64_t limitBytes, Volume *given);

        // The filesystem on @p device mounted on a volume from its pool: a ROM image in
        // mapped bytes (devplan/rom.md §3d), littlefs on a flash, FAT on anything else
        // (devplan/storage.md §13d). Lock held.
        Error mountVolumeLocked(BlockDevice &device, Volume *&out) {
                if (RomImage::looksLike(device.mapped())) {
                        RomVolume *volume = RomVolume::acquire();
                        if (volume == nullptr) return Error::Full;
                        const Error err = volume->mount(device);
                        if (err.isError()) {
                                volume->release();
                                return err;
                        }
                        out = volume;
                        return Error::Ok;
                }
                if (FlashDevice *flash = device.flash(); flash != nullptr) {
                        LittleFsVolume *volume = LittleFsVolume::acquire();
                        if (volume == nullptr) return Error::Full;
                        const Error err = volume->mount(device, *flash);
                        if (err.isError()) {
                                volume->release();
                                return err;
                        }
                        out = volume;
                        return Error::Ok;
                }
                FatVolume *volume = FatVolume::acquire();
                if (volume == nullptr) return Error::Full;
                const Error err = volume->mount(device);
                if (err.isError()) {
                        volume->release();
                        return err;
                }
                out = volume;
                return Error::Ok;
        }

} // namespace

Error Vfs::mount(StringView path, BlockDevice &device) {
        const Error err = mountAt(path, &device, 0);
        // A pending zone name: its database may be on this filesystem; the leap-second list too.
        if (err.isOk()) {
                Clock::retryZoneName();
                reloadLeapSeconds();
        }
        return err;
}

namespace {

        // @p device null: a tmpfs of at most @p limitBytes, or @p given (Fs::mountVolume()), whose
        // reference the mount takes (released on an error).
        Error mountAt(StringView path, BlockDevice *device, uint64_t limitBytes, Volume *given) {
                const Error err = mountGiven(path, device, limitBytes, given);
                if (err.isError() && given != nullptr) given->release();
                return err;
        }

        Error mountGiven(StringView path, BlockDevice *device, uint64_t limitBytes, Volume *given) {
                using namespace Vfs;
                const Result<String> normalized = normalize(path, "/");
                if (normalized.error().isError()) return normalized.error();
                const StringView p = normalized.value().view();
                if (p == "/" || isAtOrBelow(p, "/dev") || isAtOrBelow(p, BinPath) || isAtOrBelow(p, ZoneInfoPath) ||
                    isAtOrBelow(p, HomePath) || isAtOrBelow(p, CfgPath) || isAtOrBelow(p, EtcPath) ||
                    isAtOrBelow(p, AppsPath) || isAtOrBelow(p, TrashPath) || p.size() > MaxMountPath)
                        return Error::Invalid;
                Mutex::Locker lock(Fs::lock(), WaitForever);
                for (size_t i = 0; i < mountCount; ++i) {
                        const StringView mp(mounts[i].path);
                        if (p == mp) return Error::Exists;
                        // Not inside a mounted filesystem, nor over the path to one.
                        if (isBelow(p, mp) || isBelow(mp, p)) return Error::Invalid;
                        // One filesystem in two volumes would corrupt it.
                        if (device != nullptr && overlaps(i, *device)) return Error::Busy;
                }
                if (mountCount == MaxMounts) return Error::Full;
                Volume *volume = given;
                if (given != nullptr) {
                        // made elsewhere, mounted already
                } else if (device != nullptr) {
                        const Error err = mountVolumeLocked(*device, volume);
                        if (err.isError()) return err;
                } else {
                        TmpVolume *tmp = TmpVolume::acquire();
                        if (tmp == nullptr) return Error::Full;
                        const Error err = tmp->mount(limitBytes);
                        if (err.isError()) {
                                tmp->release();
                                return err;
                        }
                        volume = tmp;
                }
                Mount &m = mounts[mountCount++];
                std::memcpy(m.path, p.data(), p.size());
                m.path[p.size()] = '\0';
                m.volume = volume;
                m.device = device;
                Zones::mountsChangedLocked();
                trashMountsChangedLocked();
                Fs::tellAllWatchesLocked();
                logHomeLocked();
                mountChanges.notify();
                return Error();
        }

} // namespace

Error Vfs::mountTmpfs(StringView path, uint64_t limitBytes) {
        return mountAt(path, nullptr, limitBytes);
}

Error Fs::mountVolume(StringView path, Volume &volume) {
        return mountAt(path, nullptr, 0, &volume);
}

void Fs::unmountVolume(const Volume &volume) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (mounts[i].volume != &volume) continue;
                removeMountLocked(i);
                return;
        }
}

Error Vfs::setTmpfsLimit(StringView path, uint64_t limitBytes) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (path != StringView(mounts[i].path)) continue;
                if (mounts[i].device != nullptr) return Error::Invalid;
                static_cast<TmpVolume *>(mounts[i].volume)->setLimit(limitBytes);
                return Error::Ok;
        }
        return Error::NotExist;
}

void Vfs::setHomeReport(HomeReport report) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        homeReport = report;
        return;
}

Error Vfs::setHome(StringView target, StringView fallback) {
        String t, f;
        if (!target.isEmpty()) {
                Result<String> nt = normalize(target, "/");
                if (nt.error().isError()) return nt.error();
                t = std::move(nt).value();
                if (isAtOrBelow(t.view(), HomePath)) return Error::Invalid; // a loop
        }
        if (!fallback.isEmpty()) {
                Result<String> nf = normalize(fallback, "/");
                if (nf.error().isError()) return nf.error();
                f = std::move(nf).value();
                if (isAtOrBelow(f.view(), HomePath)) return Error::Invalid;
        }
        // Both as C strings in Psram, made before the lock.
        auto copy = [](const String &from) -> char * {
                if (from.isEmpty()) return nullptr;
                auto *to = static_cast<char *>(Memory::alloc(MemSpace::Psram, from.size() + 1));
                if (to != nullptr) std::memcpy(to, from.cStr(), from.size() + 1);
                return to;
        };
        char *newTarget = copy(t);
        char *newFallback = copy(f);
        if ((!t.isEmpty() && newTarget == nullptr) || (!f.isEmpty() && newFallback == nullptr)) {
                Memory::free(newTarget);
                Memory::free(newFallback);
                return Error::NoMem;
        }
        char *oldTarget = nullptr;
        char *oldFallback = nullptr;
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                oldTarget = std::exchange(homeTarget, newTarget);
                oldFallback = std::exchange(homeFallback, newFallback);
                homeShown = 0;
                logHomeLocked();
                // Paths below /home now name other files.
                Fs::changedLocked(0, 0);
                Fs::tellAllWatchesLocked();
        }
        Memory::free(oldTarget);
        Memory::free(oldFallback);
        return Error::Ok;
}

Error Vfs::setCfg(StringView target) {
        String t;
        if (!target.isEmpty()) {
                Result<String> nt = normalize(target, "/");
                if (nt.error().isError()) return nt.error();
                t = std::move(nt).value();
                if (isAtOrBelow(t.view(), CfgPath) || isAtOrBelow(t.view(), HomePath)) return Error::Invalid;
        }
        char *newTarget = nullptr;
        if (!t.isEmpty()) {
                newTarget = static_cast<char *>(Memory::alloc(MemSpace::Psram, t.size() + 1));
                if (newTarget == nullptr) return Error::NoMem;
                std::memcpy(newTarget, t.cStr(), t.size() + 1);
        }
        char *oldTarget = nullptr;
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                oldTarget = std::exchange(cfgTarget, newTarget);
                // Paths below /cfg now name other files.
                Fs::changedLocked(0, 0);
                Fs::tellAllWatchesLocked();
        }
        Memory::free(oldTarget);
        return Error::Ok;
}

Error Vfs::setAppLinks(Span<const AppLink> links) {
        size_t text = 0;
        for (const AppLink &l : links) {
                if (l.name.isEmpty() || l.name.size() > MaxAppName || l.name.contains('/') || l.name == "." ||
                    l.name == ".." || l.target.isEmpty() || l.target[0] != '/' || l.target.contains('\0'))
                        return Error::Invalid;
                text += l.name.size() + 1 + l.target.size() + 1;
        }
        AppTable *table = nullptr;
        if (!links.isEmpty()) {
                const size_t bytes = sizeof(AppTable) + links.size() * sizeof(AppTable::Entry) + text;
                auto        *block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bytes));
                if (block == nullptr) return Error::NoMem;
                table = reinterpret_cast<AppTable *>(block);
                table->count = links.size();
                table->entries = reinterpret_cast<AppTable::Entry *>(block + sizeof(AppTable));
                table->text = reinterpret_cast<char *>(table->entries + links.size());
                uint32_t   at = 0;
                const auto put = [&](StringView part) {
                        const uint32_t start = at;
                        std::memcpy(table->text + at, part.data(), part.size());
                        at += uint32_t(part.size());
                        table->text[at++] = '\0';
                        return start;
                };
                for (size_t i = 0; i < links.size(); ++i) {
                        table->entries[i].name = put(links[i].name);
                        table->entries[i].target = put(links[i].target);
                }
        }
        AppTable *old = nullptr;
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                old = std::exchange(appTable, table);
                // Paths below /apps now lead elsewhere; its watches see entries come and go.
                Fs::changedLocked(0, 0);
                Fs::tellEntryLocked("/apps/-", DirectoryWatch::Entries, false); // any name in /apps
        }
        Memory::free(old);
        return Error::Ok;
}

Error Vfs::pin(StringView path) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (path == StringView(mounts[i].path)) {
                        mounts[i].pinned = true;
                        return Error();
                }
        }
        return Error::NotExist;
}

bool Vfs::isMounted(StringView path) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i)
                if (path == StringView(mounts[i].path)) return true;
        return false;
}

WaitList &Vfs::mountWaiters() {
        return mountChanges;
}

void Vfs::beginSettling() {
        settling.fetch_add(1, std::memory_order_acq_rel);
        return;
}

void Vfs::endSettling() {
        settling.fetch_sub(1, std::memory_order_acq_rel);
        mountChanges.notify();
        return;
}

bool Vfs::isSettling() {
        return settling.load(std::memory_order_acquire) != 0;
}

Error Vfs::unmount(StringView path) {
        const Result<String> normalized = normalize(path, "/");
        if (normalized.error().isError()) return normalized.error();
        Mutex::Locker lock(Fs::lock(), WaitForever);
        for (size_t i = 0; i < mountCount; ++i) {
                if (normalized.value().view() == StringView(mounts[i].path)) {
                        if (mounts[i].pinned) return Error::Busy;
                        removeMountLocked(i);
                        return Error();
                }
        }
        return Error::NotExist;
}

size_t Vfs::unmountDevice(BlockDevice &device) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        size_t        removed = 0;
        for (size_t i = 0; i < mountCount;) {
                if (isMountOn(i, device)) {
                        removeMountLocked(i);
                        ++removed;
                } else {
                        ++i;
                }
        }
        return removed;
}

size_t Vfs::listMounts(Span<MountInfo> out) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const size_t  n = mountCount < out.size() ? mountCount : out.size();
        for (size_t i = 0; i < n; ++i) {
                MountInfo &info = out[i];
                info = MountInfo();
                std::memcpy(info.path, mounts[i].path, sizeof(info.path));
                // None for a filesystem in memory.
                if (mounts[i].device != nullptr) {
                        const BlockDevices::Entry *e = Fs::findEntryLocked(*mounts[i].device);
                        if (e != nullptr) std::memcpy(info.device, e->name, sizeof(info.device));
                }
                info.type = mounts[i].volume->typeName();
                info.network = mounts[i].volume->locksItself();
        }
        return n;
}

BlockDevice *Vfs::findDevice(StringView path, const Context &context) {
        Mutex::Locker      lock(Fs::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error().isError() || found.value().kind != Kind::DevBlock) return nullptr;
        return found.value().device;
}

Error Vfs::format(BlockDevice &device, FsFormat format) {
        bool hadPartitions = false;
        bool whole = true;
        {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                for (size_t i = 0; i < mountCount; ++i) {
                        if (overlaps(i, device)) return Error::Busy;
                }
                const BlockDevices::Entry *e = Fs::findEntryLocked(device);
                whole = e == nullptr || e->parent == nullptr;
                for (size_t i = 0; i < Fs::deviceCountLocked(); ++i)
                        hadPartitions = hadPartitions || Fs::deviceAtLocked(i).parent == &device;
                FlashDevice *flash = device.flash();
                const Error  err =
                        flash != nullptr ? LittleFsVolume::format(*flash) : FatVolume::format(device, format, whole);
                if (err.isError()) return err;
        }
        // A new partition table: register the new partitions instead of the old.
        if (whole && hadPartitions) {
                Partitions::detach(device);
                const Result<size_t> attached = Partitions::attach(device);
                if (attached.error().isError()) return attached.error();
        }
        return Error();
}

// --- Vfs: files and directories ---------------------------------------------

Error Vfs::addDeviceFile(const DeviceFile &file) {
        if (file.name == nullptr || file.open == nullptr) return Error::Invalid;
        const StringView name(file.name);
        if (name.isEmpty() || name.contains('/')) return Error::Invalid;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const size_t  n = deviceFileCount.load(std::memory_order_relaxed);
        if (findDevNode(name) != nullptr || findDeviceFile(name, true) != nullptr ||
            Fs::findEntryLocked(name) != nullptr)
                return Error::Exists;
        if (n >= MaxDeviceFiles) return Error::Full;
        deviceFiles[n] = file;
        deviceFileCount.store(n + 1, std::memory_order_release);
        return Error::Ok;
}

Error Vfs::addDeviceDirectory(const DeviceDirectory &directory) {
        if (directory.name == nullptr || directory.open == nullptr || directory.count == 0) return Error::Invalid;
        const StringView name(directory.name);
        if (name.isEmpty() || name.contains('/')) return Error::Invalid;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        const size_t  n = deviceDirCount.load(std::memory_order_relaxed);
        if (findDevNode(name) != nullptr || findDeviceFile(name, true) != nullptr || findDeviceDir(name) != nullptr ||
            Fs::findEntryLocked(name) != nullptr)
                return Error::Exists;
        if (n >= MaxDeviceDirectories) return Error::Full;
        deviceDirs[n] = directory;
        deviceDirCount.store(n + 1, std::memory_order_release);
        return Error::Ok;
}

Error Vfs::addEtcFile(const EtcFile &file) {
        return Etc::add(file);
}

Result<FileHandle::Ptr> Vfs::open(StringView path, uint8_t flags, const Context &context) {
        if ((flags & (Read | Write)) == 0 || ((flags & (Truncate | Append)) != 0 && (flags & Write) == 0))
                return makeError<FileHandle::Ptr>(Error::Invalid);
        // /etc's files: built without the lock (they read settings, whose store is in /cfg).
        if (Etc::count() != 0) {
                Result<String>   normalized = normalize(path, context.cwd);
                const StringView p = normalized.error().isOk() ? normalized.value().view() : StringView();
                if (isBelow(p, EtcPath) && p.lastIndexOf('/') == EtcPath.size()) {
                        const Vfs::EtcFile *file = Etc::find(p.mid(EtcPath.size() + 1));
                        if (file != nullptr) {
                                if ((flags & (Write | Truncate | Append)) != 0)
                                        return makeError<FileHandle::Ptr>(Error::ReadOnly);
                                Result<FileHandle::Ptr> handle = Etc::open(*file);
                                if (handle.error().isOk()) handle.value()->setIdentity(pathId(p), 0);
                                return handle;
                        }
                }
        }
        // A file another component added: opened without the lock (a clipboard's may wait for
        // the program that copied). /dev has no links, so the name is enough.
        if (deviceFileCount.load(std::memory_order_acquire) != 0 ||
            deviceDirCount.load(std::memory_order_acquire) != 0) {
                Result<String> normalized = normalize(path, context.cwd);
                const Vfs::DeviceDirectory *dir = nullptr;
                size_t                      index = 0;
                if (normalized.error().isOk() && findDeviceEntry(normalized.value().view(), dir, index)) {
                        if ((flags & (Create | Exclusive)) == (Create | Exclusive))
                                return makeError<FileHandle::Ptr>(Error::Exists);
                        Result<FileHandle::Ptr> handle = dir->open(dir->arg, index, flags);
                        if (handle.error().isOk() && dir->identity == nullptr)
                                handle.value()->setIdentity(objectId(dir) + index, 0);
                        return handle;
                }
                if (normalized.error().isOk() && normalized.value().view().startsWith("/dev/")) {
                        const Vfs::DeviceFile *file = findDeviceFile(normalized.value().view().mid(5));
                        if (file != nullptr) {
                                if ((flags & (Create | Exclusive)) == (Create | Exclusive))
                                        return makeError<FileHandle::Ptr>(Error::Exists);
                                Result<FileHandle::Ptr> handle = file->open(file->arg, flags);
                                if (handle.error().isOk()) handle.value()->setIdentity(objectId(file), 0);
                                return handle;
                        }
                }
        }
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        {
                const Error err = normalizeLocked(path, context.cwd, storage, true);
                if (err.isError()) return makeError<FileHandle::Ptr>(err);
        }
        const uint8_t mode = ((flags & Read) ? FileHandle::Read : 0) | ((flags & Write) ? FileHandle::Write : 0);
        for (int hops = 0;; ++hops) {
                if (hops > MaxLinks) return makeError<FileHandle::Ptr>(Error::TooManyLinks);
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error() == Error::NotExist && (flags & Create) != 0 && outsideFilesystemsLocked(storage))
                        return makeError<FileHandle::Ptr>(isReadOnlyTree(storage.view()) ? Error::ReadOnly
                                                                                         : Error::PermissionDenied);
                if (found.error().isError()) return makeError<FileHandle::Ptr>(found.error());
                const Node &node = found.value();
                switch (node.kind) {
                        case Kind::Virtual:
                        case Kind::BinDir:
                        case Kind::ZoneDir:
                        case Kind::EtcDir:
                        case Kind::DevSubDir:
                        case Kind::AppsDir:
                        case Kind::TrashDir:
                        case Kind::DevDir: return makeError<FileHandle::Ptr>(Error::IsDir);
                        case Kind::DevSubFile: return node.devDir->open(node.devDir->arg, node.devIndex, flags);
                        // Reached through a link (the name itself is opened above, unlocked):
                        // building it here would wait on the settings under the lock.
                        case Kind::EtcFile: return makeError<FileHandle::Ptr>(Error::NotSupported);
                        case Kind::ZoneFile: {
                                if ((flags & (Write | Truncate | Append)) != 0)
                                        return makeError<FileHandle::Ptr>(Error::ReadOnly);
                                Result<FileHandle::Ptr> handle = Zones::openLocked(node.zoneOffset, node.zoneLength);
                                if (handle.error().isOk()) handle.value()->setIdentity(pathId(storage.view()), 0);
                                return handle;
                        }
                        case Kind::BinProgram: {
                                // Read-only, and empty: a built-in is run, not read.
                                if ((flags & (Write | Truncate | Append)) != 0)
                                        return makeError<FileHandle::Ptr>(Error::ReadOnly);
                                Result<FileHandle::Ptr> handle = FileHandle::open(devNull, mode);
                                if (handle.error().isOk()) handle.value()->setIdentity(pathId(storage.view()), 0);
                                return handle;
                        }
                        case Kind::DevStream: return FileHandle::open(*node.stream, mode);
                        case Kind::DevFile: return node.file->open(node.file->arg, flags); // added since the look
                        case Kind::DevConsole:
                                if (context.terminal == nullptr) return makeError<FileHandle::Ptr>(Error::NotExist);
                                return FileHandle::open(*context.terminal, mode);
                        case Kind::DevBlock: return makeError<FileHandle::Ptr>(Error::NotSupported); // Raw access: later.
                        default: break;
                }
                if (node.rest.isEmpty()) return makeError<FileHandle::Ptr>(Error::IsDir);
                Error err;
                // Truncating would cut a link file itself: look first.
                String target;
                if ((flags & Truncate) != 0) {
                        const Result<FileInfo> info = statLocked(node);
                        if (info.error().isOk() &&
                            linkCandidate(info.value().attributes, info.value().type, info.value().size) &&
                            readLinkLocked(*node.volume, node.rest, target).isOk()) {
                                String next;
                                err = linkDestination(storage.view(), target, StringView(), next);
                                if (err.isError()) return makeError<FileHandle::Ptr>(err);
                                storage = std::move(next);
                                continue;
                        }
                }
                bool                    noPath = false;
                const FileIdentity      identity{pathId(storage.view()), Fs::folderId(storage.view()), node.mount};
                Result<FileHandle::Ptr> handle = callVolume(
                        *node.volume, [&] { return node.volume->open(node.rest, flags, identity, noPath, &target); });
                if (handle.error().isError()) {
                        // A link as the last component (openFile read it), or along the way.
                        if (!target.isEmpty()) {
                                String next;
                                err = linkDestination(storage.view(), target, StringView(), next);
                                if (err.isError()) return makeError<FileHandle::Ptr>(err);
                                storage = std::move(next);
                                continue;
                        }
                        if (noPath && followPathLocked(storage, false)) continue;
                        return handle;
                }
                handle.value()->setIdentity(pathId(storage.view()), node.mount);
                return handle;
        }
}

namespace {

        // A FileInfo's identity: its path's, or a device node's stream.
        void setIdentity(FileInfo &fi, const Node &node, StringView storage, const Vfs::Context &context) {
                switch (node.kind) {
                        // A device node is its stream (or device), as its handles say.
                        case Kind::DevStream: fi.id = objectId(node.stream); break;
                        case Kind::DevConsole: fi.id = objectId(context.terminal); break;
                        case Kind::DevBlock: fi.id = objectId(node.device); break;
                        case Kind::DevFile: fi.id = objectId(node.file); break;
                        case Kind::DevSubFile:
                                fi.id = node.devDir->identity != nullptr
                                                ? node.devDir->identity(node.devDir->arg, node.devIndex)
                                                : objectId(node.devDir) + node.devIndex;
                                break;
                        default:
                                fi.id = pathId(storage);
                                fi.device = node.mount;
                                break;
                }
                return;
        }

        Result<FileInfo> statAt(StringView path, const Vfs::Context &context, bool followLast) {
                Mutex::Locker lock(Fs::lock(), WaitForever);
                String        storage;
                const Error   err = normalizeLocked(path, context.cwd, storage, followLast);
                if (err.isError()) return makeError<FileInfo>(err);
                Node             node;
                Result<FileInfo> info = statFollowLocked(storage, followLast, node);
                if (info.error().isOk()) setIdentity(info.value(), node, storage.view(), context);
                return info;
        }

} // namespace

Error Vfs::copyFile(StringView from, StringView to, const Context &context) {
        auto [info, serr] = stat(from, context);
        if (serr.isError()) return serr;
        if (info.isDirectory()) return Error::IsDir;
        // Taken: Exists, before anything is opened (onto itself, the write's open would be Busy).
        if (lstat(to, context).isOk()) return Error::Exists;
        auto [in, ierr] = open(from, Read, context);
        if (ierr.isError()) return ierr;
        Error err;
        {
                auto [out, oerr] = open(to, Write | Create | Exclusive, context);
                if (oerr.isError()) return oerr;
                uint8_t buf[512];
                for (;;) {
                        auto [n, rerr] = in->read(WritableByteSpan(buf, sizeof(buf)), WaitForever);
                        if (rerr == Error::EndOfFile) break;
                        err = rerr.isError() ? rerr : out->write(ByteSpan(buf, n), WaitForever).error();
                        if (err.isError()) break;
                }
        }
        if (err.isError()) {
                (void)remove(to, context); // not half a copy
                return err;
        }
        if (info.modified.ns != 0) (void)setModified(to, info.modified, context);
        return Error::Ok;
}

Error Vfs::findProgram(StringView name, StringView searchPath, const Context &context, String &out) {
        static const char *const suffixes[] = {"", ".elf", ".sh"};
        if (name.isEmpty()) return Error::NotExist;
        size_t at = 0;
        for (;;) {
                size_t colon = searchPath.indexOf(':', at);
                if (colon == StringView::NPos) colon = searchPath.size();
                // An empty entry is the current directory, as in POSIX.
                const StringView dir = colon > at ? searchPath.mid(at, colon - at) : StringView(".");
                for (const char *suffix : suffixes) {
                        out.clear();
                        Error err = out.append(dir);
                        if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                        if (err.isOk()) err = out.append(name);
                        if (err.isOk()) err = out.append(suffix);
                        if (err.isError()) return err;
                        const Result<FileInfo> info = stat(out, context);
                        if (info.isOk() && info.value().type == FileType::File) return Error::Ok;
                }
                if (colon == searchPath.size()) break;
                at = colon + 1;
        }
        out.clear();
        return Error::NotExist;
}

Result<FileInfo> Vfs::stat(StringView path, const Context &context) {
        return statAt(path, context, true);
}

Result<FileInfo> Vfs::lstat(StringView path, const Context &context) {
        return statAt(path, context, false);
}

Result<ByteSpan> Vfs::map(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        const Error   err = normalizeLocked(path, context.cwd, storage, true);
        if (err.isError()) return makeError<ByteSpan>(err);
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, true, node);
        if (info.error().isError()) return makeError<ByteSpan>(info.error());
        if (info.value().isDirectory()) return makeError<ByteSpan>(Error::IsDir);
        if (node.kind != Kind::Mounted) return makeError<ByteSpan>(Error::NotSupported);
        bool noPath = false;
        // Bytes in place: a filesystem in memory or on the flash, never one that locks itself.
        if (node.volume->locksItself()) return makeError<ByteSpan>(Error::NotSupported);
        return node.volume->map(node.rest, noPath);
}

Result<String> Vfs::readLink(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        const Error   nerr = normalizeLocked(path, context.cwd, storage, false);
        if (nerr.isError()) return makeError<String>(nerr);
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, false, node);
        if (info.error().isError()) return makeError<String>(info.error());
        if (info.value().type != FileType::Link) return makeError<String>(Error::Invalid);
        if (node.kind == Kind::RootLink) return String::fromView(rootLinkLocked(node.name));
        if (node.kind == Kind::AppLink) return String::fromView(appTargetLocked(node.name));
        String      target;
        const Error err = readLinkLocked(*node.volume, node.rest, target);
        if (err.isError()) return makeError<String>(err);
        return makeResult(std::move(target));
}

Error Vfs::symlink(StringView target, StringView path, const Context &context) {
        if (target.isEmpty() || target.size() > MaxTarget) return Error::Invalid;
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        const Error   nerr = normalizeLocked(path, context.cwd, storage, false);
        if (nerr.isError()) return nerr;
        if (isReadOnlyTree(storage.view())) return Error::ReadOnly; // the built-ins, the zones
        for (int hops = 0;; ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error() == Error::NotExist && outsideFilesystemsLocked(storage)) return Error::PermissionDenied;
                if (found.error().isOk() && (found.value().kind != Kind::Mounted || found.value().rest.isEmpty()))
                        return Error::Exists;
                if (found.error().isError() && found.error() != Error::NotExist) return found.error();
                // resolveLocked() finds a Mounted node whether or not the file exists.
                const Node &node = found.value();
                bool        noPath = false;
                const Error err = writeLinkLocked(*node.volume, node.rest, target, noPath);
                if (noPath && followPathLocked(storage, false)) continue;
                if (err.isOk()) Fs::tellEntryLocked(storage.view(), DirectoryWatch::Entries, false);
                return err;
        }
}

const ProgramEntry *Vfs::builtinProgram(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        if (normalizeLocked(path, context.cwd, storage, true).isError()) return nullptr;
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, true, node); // links followed
        if (info.error().isError() || node.kind != Kind::BinProgram) return nullptr;
        return node.program;
}

Result<String> Vfs::realPath(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        const Error   nerr = normalizeLocked(path, context.cwd, storage, true);
        if (nerr.isError()) return makeError<String>(nerr);
        Node                   node;
        const Result<FileInfo> info = statFollowLocked(storage, true, node);
        if (info.error().isError()) return makeError<String>(info.error());
        return makeResult(std::move(storage));
}

Result<Vfs::Resolved> Vfs::resolve(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        Resolved      out;
        const Error   nerr = normalizeLocked(path, context.cwd, out.path, true);
        if (nerr.isError()) return makeError<Resolved>(nerr);
        Node             node;
        Result<FileInfo> info = statFollowLocked(out.path, true, node);
        if (info.error().isError()) return makeError<Resolved>(info.error());
        out.info = std::move(info).value();
        setIdentity(out.info, node, out.path.view(), context);
        if (node.kind == Kind::BinProgram) out.program = node.program;
        return makeResult(std::move(out));
}

Result<Directory> Vfs::openDirectory(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        const Error   nerr = normalizeLocked(path, context.cwd, storage, true);
        if (nerr.isError()) return makeError<Directory>(nerr);
        for (int hops = 0;; ++hops) {
                if (hops > MaxLinks) return makeError<Directory>(Error::TooManyLinks);
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error().isError()) return makeError<Directory>(found.error());
                const Node &node = found.value();
                if (node.kind == Kind::DevStream || node.kind == Kind::DevConsole || node.kind == Kind::DevBlock ||
                    node.kind == Kind::DevFile || node.kind == Kind::BinProgram || node.kind == Kind::ZoneFile ||
                    node.kind == Kind::EtcFile || node.kind == Kind::DevSubFile)
                        return makeError<Directory>(Error::NotDir);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Directory::State));
                if (mem == nullptr) return makeError<Directory>(Error::NoMem);
                auto *state = ::new (mem) Directory::State();
                state->kind = static_cast<uint8_t>(node.kind);
                state->volume = node.volume;
                state->device = node.mount;
                state->idPrefix = pathId(storage.view());
                state->devDir = node.devDir;
                if (node.kind == Kind::Virtual) {
                        const size_t n = storage.size() < MaxMountPath ? storage.size() : MaxMountPath;
                        std::memcpy(state->path, storage.data(), n);
                        state->path[n] = '\0';
                }
                if (node.kind == Kind::ZoneDir) {
                        // The names, read now: the database needn't stay open.
                        state->zoneModified = node.zoneModified;
                        const StringView name = storage.view() == ZoneInfoPath
                                                        ? StringView()
                                                        : storage.view().mid(ZoneInfoPath.size() + 1);
                        const Error      err = Zones::listLocked(name, state->zones);
                        if (err.isError()) {
                                state->~State();
                                Memory::free(mem);
                                return makeError<Directory>(err);
                        }
                }
                if (node.kind == Kind::Mounted) {
                        bool noPath = false;
                        auto [cursor, err] =
                                callVolume(*node.volume, [&] { return node.volume->openDirectory(node.rest, noPath); });
                        state->cursor = cursor;
                        if (err.isOk() && node.volume->locksItself()) node.volume->retain(); // while open
                        if (err.isError()) {
                                state->~State();
                                Memory::free(mem);
                                // A link along the way, or as the directory itself.
                                if (noPath && followPathLocked(storage, true)) continue;
                                if (err == Error::NotExist) {
                                        const Result<FileInfo> info = statLocked(node);
                                        if (info.error().isOk() && !info.value().isDirectory()) err = Error::NotDir;
                                }
                                return makeError<Directory>(err);
                        }
                }
                return makeResult(VfsAccess::make(state));
        }
}

namespace {

        // Looks up normalized @p storage naming something inside a mounted
        // filesystem (not a mount point, not virtual); other nodes give
        // @p otherError. Lock held.
        Error targetLocked(const String &storage, Error otherError, Node &node) {
                const Result<Node> found = resolveLocked(storage.view());
                if (found.error().isError()) return found.error();
                node = found.value();
                if (node.kind != Kind::Mounted || node.rest.isEmpty()) return otherError;
                return Error::Ok;
        }

        // If @p node (at normalized @p storage) is an entry at the top of its filesystem's
        // trash, removes its info file: it was removed or moved out (FB23). Lock held.
        void dropTrashInfoLocked(const Node &node, StringView storage) {
                StringView name;
                if (node.kind != Kind::Mounted || !isTrashEntry(node.rest, name)) return;
                String info;
                Error  err = info.append(TrashIndex::InfoRest);
                if (err.isOk()) err = info.append('/');
                if (err.isOk()) err = info.append(name);
                if (err.isOk()) err = info.append(TrashIndex::InfoSuffix);
                if (err.isError() || node.volume->remove(info.view()).isError()) return;
                String path;
                if (path.append(storage.left(storage.size() - node.rest.size())).isOk() &&
                    path.append(info.view()).isOk())
                        Fs::tellEntryLocked(path.view(), DirectoryWatch::Entries, false);
                return;
        }

        // The changes below act on the link /home itself, not where it leads (but setModified()).
        Error normalizeInto(StringView path, const Vfs::Context &context, String &storage, bool followHome = false) {
                return normalizeLocked(path, context.cwd, storage, followHome);
        }

} // namespace

// The changes below act on the last component itself (a link is removed,
// renamed, not followed), following links in the directories along the way.

Error Vfs::mkdir(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isReadOnlyTree(storage.view())) return Error::ReadOnly; // the built-ins, the zones
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                Node node;
                err = targetLocked(storage, Error::Exists, node);
                if (err == Error::NotExist && outsideFilesystemsLocked(storage)) err = Error::PermissionDenied;
                if (err.isError()) return err;
                bool        noPath = false;
                const Error made = callVolume(*node.volume, [&] { return node.volume->mkdir(node.rest, noPath); });
                if (noPath && followPathLocked(storage, false)) continue;
                if (made.isOk()) Fs::tellEntryLocked(storage.view(), DirectoryWatch::Entries, false);
                return made;
        }
        return err;
}

Error Vfs::rmdir(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isReadOnlyTree(storage.view())) return Error::ReadOnly; // the built-ins, the zones
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                Node node;
                err = targetLocked(storage, Error::Busy, node);
                if (err == Error::Busy && node.kind != Kind::Mounted) err = Error::PermissionDenied; // virtual, or /dev
                if (err.isError()) return err;
                bool                   noPath = false;
                const Result<FileInfo> info = statLocked(node, &noPath);
                if (noPath && followPathLocked(storage, false)) continue;
                if (info.error().isError()) return info.error();
                if (!info.value().isDirectory()) return Error::NotDir;
                const Error removed = callVolume(*node.volume, [&] { return node.volume->rmdir(node.rest); });
                if (removed.isOk()) {
                        Fs::tellEntryLocked(storage.view(), DirectoryWatch::Entries, true);
                        dropTrashInfoLocked(node, storage.view());
                }
                return removed;
        }
        return err;
}

Error Vfs::remove(StringView path, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage);
        if (err.isOk() && isReadOnlyTree(storage.view())) return Error::ReadOnly; // the built-ins, the zones
        for (int hops = 0; err.isOk(); ++hops) {
                if (hops > MaxLinks) return Error::TooManyLinks;
                Node node;
                err = targetLocked(storage, Error::IsDir, node);
                const bool deviceNode = node.kind == Kind::DevStream || node.kind == Kind::DevConsole ||
                                        node.kind == Kind::DevBlock || node.kind == Kind::DevFile ||
                                        node.kind == Kind::DevSubFile || node.kind == Kind::RootLink ||
                                        node.kind == Kind::AppLink;
                if (err == Error::IsDir && deviceNode) err = Error::PermissionDenied;
                if (err.isError()) return err;
                bool                   noPath = false;
                const Result<FileInfo> info = statLocked(node, &noPath);
                if (noPath && followPathLocked(storage, false)) continue;
                if (info.error().isError()) return info.error();
                if (info.value().isDirectory()) return Error::IsDir;
                err = callVolume(*node.volume, [&] { return node.volume->remove(node.rest); });
                if (err.isOk()) {
                        Fs::changedLocked(node.mount, pathId(storage.view()));
                        Fs::tellEntryLocked(storage.view(), DirectoryWatch::Entries, false);
                        dropTrashInfoLocked(node, storage.view());
                }
                return err;
        }
        return err;
}

namespace {

        // Vfs::rename() under the lock. With @p trashing, a rename to the top of a trash (by
        // /trash or by its files) is left to Trash::move(): @p intoTrash set, the name asked
        // for in @p as (FB23).
        Error renameLocked(StringView from, StringView to, Vfs::RenameMode mode, const Vfs::Context &context,
                           bool trashing, bool &intoTrash, String &as) {
                intoTrash = false;
                String fromStorage;
                String toStorage;
                Error  err = normalizeInto(from, context, fromStorage);
                if (err.isOk()) err = normalizeInto(to, context, toStorage);
                if (err.isOk() && (isReadOnlyTree(fromStorage.view()) || isReadOnlyTree(toStorage.view())))
                        return Error::ReadOnly; // the built-ins
                for (int hops = 0; err.isOk(); ++hops) {
                        if (hops > MaxLinks) return Error::TooManyLinks;
                        if (trashing && isNewTrashName(toStorage.view())) {
                                intoTrash = true;
                                return as.assign(toStorage.view().mid(Vfs::TrashPath.size() + 1));
                        }
                        Node fromNode;
                        Node toNode;
                        err = targetLocked(fromStorage, Error::PermissionDenied, fromNode);
                        if (err.isOk()) {
                                err = targetLocked(toStorage, Error::PermissionDenied, toNode);
                                // Outside every filesystem: not something a rename can do.
                                if (err == Error::NotExist || err == Error::PermissionDenied) err = Error::NotSupported;
                        }
                        if (err.isError()) return err;
                        StringView name;
                        if (trashing && isTrashEntry(toNode.rest, name)) {
                                intoTrash = true;
                                return as.assign(name);
                        }
                        if (fromNode.volume != toNode.volume) return Error::NotSupported;
                        bool        noPath = false;
                        const Error renamed = callVolume(*fromNode.volume, [&] {
                                return fromNode.volume->rename(fromNode.rest, toNode.rest, mode, noPath);
                        });
                        if (noPath && (followPathLocked(fromStorage, false) || followPathLocked(toStorage, false)))
                                continue;
                        if (renamed.isOk()) {
                                Fs::changedLocked(fromNode.mount, pathId(fromStorage.view()));
                                // A file replaced there: its name holds another now.
                                if (mode == Vfs::RenameMode::Replace)
                                        Fs::changedLocked(toNode.mount, pathId(toStorage.view()));
                                Fs::tellEntryLocked(fromStorage.view(), DirectoryWatch::Entries, true);
                                Fs::tellEntryLocked(toStorage.view(), DirectoryWatch::Entries, false);
                                if (trashing) dropTrashInfoLocked(fromNode, fromStorage.view());
                        }
                        return renamed;
                }
                return err;
        }

        Error renameAt(StringView from, StringView to, Vfs::RenameMode mode, const Vfs::Context &context,
                       bool trashing) {
                bool   intoTrash = false;
                String as;
                Error  err;
                {
                        Mutex::Locker lock(Fs::lock(), WaitForever);
                        err = renameLocked(from, to, mode, context, trashing, intoTrash, as);
                }
                if (intoTrash && err.isOk()) return Trash::moveAs(from, as.view(), context, nullptr);
                return err;
        }

} // namespace

Error Fs::locateTrashEntry(StringView name, String &mount, String &real) {
        Mutex::Locker     lock(Fs::lock(), WaitForever);
        TrashIndex::Mount m[MaxMounts];
        size_t            slot = 0;
        StringView        found;
        if (name.isEmpty() || !TrashIndex::findLocked(trashMountsLocked(m), name, slot, found)) return Error::NotExist;
        Error err = mount.assign(StringView(mounts[slot].path));
        if (err.isOk()) err = real.assign(found);
        return err;
}

Error Vfs::rename(StringView from, StringView to, RenameMode mode, const Context &context) {
        return renameAt(from, to, mode, context, true);
}

Error Fs::renameRaw(StringView from, StringView to, Vfs::RenameMode mode, const Vfs::Context &context) {
        return renameAt(from, to, mode, context, false);
}

Error Vfs::setModified(StringView path, Timestamp time, const Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        Error         err = normalizeInto(path, context, storage, true);
        if (err.isOk() && isReadOnlyTree(storage.view())) return Error::ReadOnly; // the built-ins, the zones
        if (err.isError()) return err;
        // The file a link leads to, as `touch` does.
        Node                   found;
        const Result<FileInfo> info = statFollowLocked(storage, true, found);
        if (info.error().isError() && info.error() != Error::NotExist) return info.error();
        Node node;
        err = targetLocked(storage, Error::PermissionDenied, node);
        if (err.isError()) return err;
        err = callVolume(*node.volume, [&] { return node.volume->setModified(node.rest, time); });
        if (err.isOk()) {
                Fs::changedLocked(node.mount, pathId(storage.view()));
                Fs::tellEntryLocked(storage.view(), DirectoryWatch::Contents, false);
        }
        return err;
}

Error Fs::parseLink(const uint8_t *data, size_t size, String &target) {
        target.clear();
        if (size <= CookieBytes || std::memcmp(data, Cookie, CookieBytes) != 0) return Error::Invalid;
        Error err;
        if (size >= CookieBytes + 2 && data[CookieBytes] == 0xFF && data[CookieBytes + 1] == 0xFE) {
                err = fromUtf16(data + CookieBytes + 2, (size - CookieBytes - 2) / 2, target);
        } else {
                // No byte order mark: UTF-8 (older Cygwin, links made by hand).
                size_t n = CookieBytes;
                while (n < size && data[n] != 0) ++n;
                err = target.append(StringView(reinterpret_cast<const char *>(data) + CookieBytes, n - CookieBytes));
        }
        if (err.isOk() && (target.isEmpty() || target.size() > MaxTarget)) err = Error::Invalid;
        return err;
}

Result<FsInfo> Vfs::statfs(StringView path, const Context &context) {
        Mutex::Locker      lock(Fs::lock(), WaitForever);
        String             storage;
        const Result<Node> found = lookupLocked(path, context, storage);
        if (found.error().isError()) return makeError<FsInfo>(found.error());
        if (found.value().kind != Kind::Mounted) return makeError<FsInfo>(Error::NotSupported);
        Volume &volume = *found.value().volume;
        return callVolume(volume, [&] { return volume.info(); });
}

// --- Directory watches (directorywatch.h) ----------------------------------

Error DirectoryWatch::watch(StringView path, const Vfs::Context &context) {
        Mutex::Locker lock(Fs::lock(), WaitForever);
        String        storage;
        Error         err = normalizeLocked(path, context.cwd, storage, true);
        if (err.isOk()) {
                // Where it leads, as realPath() finds it: changes are told by real paths.
                Node                   node;
                const Result<FileInfo> info = statFollowLocked(storage, true, node);
                err = info.error();
                if (err.isOk() && !info.value().isDirectory()) err = Error::NotDir;
        }
        Fs::placeWatchLocked(*this, err.isOk(), err.isOk() ? pathId(storage.view()) : 0);
        return err;
}

BMDS_NAMESPACE_END
