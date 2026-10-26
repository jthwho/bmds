/**
 * @file      patternfiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The desktop patterns' files: found on the path, listed, and the saved palettes' text read and
 * written whole (devplan/wallpaper.md §4a-§4c).
 */

#include <bmds/memory.h>
#include <bmds/patternfiles.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Suffix(".png");

        // The next directory of a `;` list, the rest left in @p rest.
        StringView nextDir(StringView &rest) {
                const size_t     semi = rest.indexOf(';');
                const StringView dir = semi == StringView::NPos ? rest : rest.left(semi);
                rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                return dir.trimmed();
        }

        Error join(StringView dir, StringView name, String &out) {
                out.clear();
                Error err = out.append(dir);
                if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(name);
                if (err.isOk()) err = out.append(Suffix);
                return err;
        }

        bool isFile(StringView path) {
                auto [info, err] = Vfs::stat(path, Vfs::Context());
                return err.isOk() && !info.isDirectory();
        }

        // Reads @p path whole into a new PSRAM buffer of at most @p max bytes.
        Error readWhole(StringView path, size_t max, uint8_t *&data, size_t &size) {
                data = nullptr;
                size = 0;
                auto [info, serr] = Vfs::stat(path, Vfs::Context());
                if (serr.isError()) return serr;
                if (info.isDirectory()) return Error::IsDir;
                if (info.size > max) return Error::OutOfRange;
                auto [file, err] = Vfs::open(path, Vfs::Read, Vfs::Context());
                if (err.isError()) return err;
                auto *buffer = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size_t(info.size) + 1));
                if (buffer == nullptr) return Error::NoMem;
                size_t got = 0;
                while (got < info.size) {
                        auto [n, rerr] =
                                file->read(WritableByteSpan(buffer + got, size_t(info.size) - got), WaitForever);
                        if (rerr.isError()) {
                                Memory::free(buffer);
                                return rerr;
                        }
                        if (n == 0) break;
                        got += n;
                }
                data = buffer;
                size = got;
                return Error::Ok;
        }

} // namespace

Error PatternFiles::Bytes::read(StringView path) {
        reset();
        return readWhole(path, MaxFileBytes, _data, _size);
}

void PatternFiles::Bytes::reset() {
        Memory::free(_data);
        _data = nullptr;
        _size = 0;
        return;
}

Error PatternFiles::find(StringView paths, StringView name, String &path) {
        if (name.isEmpty() || name.indexOf('/') != StringView::NPos) return Error::Invalid;
        for (StringView rest = paths; !rest.isEmpty();) {
                const StringView dir = nextDir(rest);
                if (dir.isEmpty()) continue;
                if (join(dir, name, path).isError()) return Error::NoMem;
                if (isFile(path.view())) return Error::Ok;
        }
        path.clear();
        return Error::NotExist;
}

Error PatternFiles::load(StringView name, Bytes &out, String *path) {
        String paths(MemSpace::Psram), found(MemSpace::Psram);
        if (SysConf::get(StringView(PathsKey), paths).isError()) return Error::NotExist;
        Error err = find(paths.view(), name, found);
        if (err.isOk()) err = out.read(found.view());
        if (path != nullptr) (void)path->assign(found.view());
        return err;
}

Error PatternFiles::list(StringView paths, Found fn, void *arg) {
        String path(MemSpace::Psram), earlier(MemSpace::Psram);
        size_t index = 0;
        for (StringView rest = paths; !rest.isEmpty(); ++index) {
                const StringView dir = nextDir(rest);
                if (dir.isEmpty()) continue;
                auto [d, err] = Vfs::openDirectory(dir, Vfs::Context());
                if (err.isError()) continue; // not there (yet)
                for (;;) {
                        auto [info, nerr] = d.next();
                        if (nerr.isError()) break;
                        const StringView file = info.name.view();
                        if (info.isDirectory() || !file.endsWith(Suffix) || file.size() == Suffix.size()) continue;
                        const StringView name = file.left(file.size() - Suffix.size());
                        // Listed already if a directory before this one has it.
                        bool       shadowed = false;
                        StringView before = paths;
                        for (size_t i = 0; i < index && !shadowed && !before.isEmpty(); ++i) {
                                const StringView other = nextDir(before);
                                if (other.isEmpty()) continue;
                                if (join(other, name, earlier).isError()) return Error::NoMem;
                                shadowed = isFile(earlier.view());
                        }
                        if (shadowed) continue;
                        if (join(dir, name, path).isError()) return Error::NoMem;
                        fn(arg, name, path.view());
                }
        }
        return Error::Ok;
}

Error PatternFiles::readText(StringView path, String &out) {
        out.clear();
        uint8_t    *data = nullptr;
        size_t      size = 0;
        const Error err = readWhole(path, MaxTextBytes, data, size);
        if (err == Error::NotExist) return Error::Ok;
        if (err.isError()) return err;
        const Error aerr = out.assign(StringView(reinterpret_cast<const char *>(data), size));
        Memory::free(data);
        return aerr;
}

Error PatternFiles::makeFolders(StringView path) {
        // Each folder down to it, there already or made (what failed shows at the end).
        for (size_t at = 1; at <= path.size(); ++at)
                if (at == path.size() || path[at] == '/') (void)Vfs::mkdir(path.left(at), Vfs::Context());
        auto [info, err] = Vfs::stat(path, Vfs::Context());
        if (err.isError()) return err;
        return info.isDirectory() ? Error(Error::Ok) : Error(Error::NotDir);
}

Error PatternFiles::writeText(StringView path, StringView text) {
        const size_t slash = path.lastIndexOf('/');
        if (slash == StringView::NPos || slash + 1 == path.size()) return Error::Invalid;
        if (slash > 0) {
                const Error ferr = makeFolders(path.left(slash));
                if (ferr.isError()) return ferr;
        }
        String temp(MemSpace::Psram);
        if (temp.assign(path).isError() || temp.append(".new").isError()) return Error::NoMem;
        Error err;
        {
                auto [file, oerr] = Vfs::open(temp.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, Vfs::Context());
                err = oerr;
                if (err.isOk() && !text.isEmpty())
                        err = file->write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()),
                                          WaitForever)
                                      .error();
        }
        if (err.isOk()) err = Vfs::rename(temp.view(), path, Vfs::RenameMode::Replace, Vfs::Context());
        if (err.isError()) (void)Vfs::remove(temp.view(), Vfs::Context());
        return err;
}

BMDS_NAMESPACE_END
