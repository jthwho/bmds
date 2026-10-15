/**
 * @file      controlsbook.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One application's actions for the Controls window and `controls` (devplan/controls.md §7d).
 */

#include <cstring>
#include <new>
#include <bmds/apps.h>
#include <bmds/memory.h>
#include <bmds/programcontrols.h>
#include <bmds/vfs.h>
#include "controlsbook.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxText = ProgramControls::MaxDeclaration;

        // @p path's text into @p out (at most MaxText bytes). @return Its error (NotExist: none).
        Error readText(StringView path, String &out) {
                auto [file, err] = Vfs::open(path, Vfs::Read, Vfs::Context());
                if (err.isError()) return err;
                char *buffer = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxText));
                if (buffer == nullptr) return Error::NoMem;
                size_t got = 0;
                while (got < MaxText) {
                        auto [n, rerr] =
                                file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buffer) + got, MaxText - got),
                                           WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                        if (rerr.isError()) {
                                Memory::free(buffer);
                                return rerr;
                        }
                        got += n;
                }
                err = out.assign(StringView(buffer, got));
                Memory::free(buffer);
                return err;
        }

        struct Found {
                        String folder{MemSpace::Psram};
                        String id{MemSpace::Psram};
                        String name{MemSpace::Psram};
        };

        struct Visit {
                        List<Found> *found;
                        StringView   only; // an id, or empty: all
                        Error        err;
        };

        void visitApp(void *arg, const Apps::App &app) {
                auto &v = *static_cast<Visit *>(arg);
                if (!v.only.isEmpty() && v.only != StringView(app.id)) return;
                Found f;
                if (f.folder.assign(app.folder.view()).isError() || f.id.assign(StringView(app.id)).isError() ||
                    f.name.assign(StringView(app.entry.name[0] != '\0' ? app.entry.name : app.id)).isError() ||
                    v.found->append(std::move(f)).isError())
                        v.err = Error::NoMem;
                return;
        }

        Error installed(StringView only, List<Found> &out) {
                Apps *apps = Apps::system();
                if (apps == nullptr) return Error::NotExist;
                Visit v{&out, only, Error::Ok};
                apps->visit(visitApp, &v);
                return v.err;
        }

        Error declarationPath(const Found &f, String &out) {
                Error err = out.assign(f.folder.view());
                if (err.isOk()) err = out.append("/controls.conf");
                return err;
        }

        template <typename T> T *make() {
                void *mem = Memory::allocAligned(MemSpace::Psram, sizeof(T), alignof(T));
                return mem != nullptr ? ::new (mem) T() : nullptr;
        }

        template <typename T> void unmake(T *p) {
                if (p == nullptr) return;
                p->~T();
                Memory::free(p);
                return;
        }

} // namespace

Error ControlsBook::apps(List<App> &out) {
        List<Found> found(MemSpace::Psram);
        Error       err = installed(StringView(), found);
        for (size_t i = 0; i < found.size() && err.isOk(); ++i) {
                String path(MemSpace::Psram);
                if (declarationPath(found[i], path).isError() || Vfs::stat(path.view(), Vfs::Context()).isError())
                        continue;
                App a;
                if (a.id.assign(found[i].id.view()).isError() || a.name.assign(found[i].name.view()).isError() ||
                    out.append(std::move(a)).isError())
                        err = Error::NoMem;
        }
        return err;
}

ControlsBook *ControlsBook::create() {
        void *mem = Memory::allocAligned(MemSpace::Psram, sizeof(ControlsBook), alignof(ControlsBook));
        if (mem == nullptr) return nullptr;
        auto *book = ::new (mem) ControlsBook();
        book->_defaults = make<Controls>();
        book->_current = make<Controls>();
        if (book->_defaults == nullptr || book->_current == nullptr) {
                destroy(book);
                return nullptr;
        }
        return book;
}

void ControlsBook::destroy(ControlsBook *book) {
        if (book == nullptr) return;
        unmake(book->_defaults);
        unmake(book->_current);
        book->~ControlsBook();
        Memory::free(book);
        return;
}

Error ControlsBook::load(StringView id, StringView home) {
        List<Found> found(MemSpace::Psram);
        Error       err = installed(id, found);
        if (err.isError()) return err;
        if (found.isEmpty()) return Error::NotExist;
        String path(MemSpace::Psram), text(MemSpace::Psram), user(MemSpace::Psram);
        err = declarationPath(found[0], path);
        if (err.isOk()) err = readText(path.view(), text);
        if (err.isError()) return err;
        uint32_t line = 0;
        err = _defaults->declare(text.view(), line);
        if (err.isOk()) err = _current->declare(text.view(), line);
        if (err.isError()) return err;
        // $HOME/.config/bmds/controls/ID.conf: the user's, if any.
        if (_userDir.assign(home).isError() || _userDir.append("/.config/bmds/controls").isError() ||
            _userPath.assign(_userDir.view()).isError() || _userPath.append("/").isError() ||
            _userPath.append(id).isError() || _userPath.append(".conf").isError() || _id.assign(id).isError() ||
            _name.assign(found[0].name.view()).isError())
                return Error::NoMem;
        if (readText(_userPath.view(), user).isOk()) _current->applyUser(user.view());
        return Error::Ok;
}

Error ControlsBook::save() {
        char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxText));
        if (text == nullptr) return Error::NoMem;
        const size_t n = Controls::formatUser(*_defaults, *_current, text, MaxText);
        Error        err;
        if (n == 0) {
                err = Vfs::remove(_userPath.view(), Vfs::Context());
                if (err == Error::NotExist) err = Error::Ok;
        } else {
                // The folders on the way, each made if missing.
                const StringView dir = _userDir.view();
                for (size_t at = 1; at <= dir.size() && err.isOk(); ++at) {
                        if (at != dir.size() && dir[at] != '/') continue;
                        const Error made = Vfs::mkdir(dir.left(at), Vfs::Context());
                        if (made.isError() && made != Error::Exists) err = made;
                }
                if (err.isOk()) {
                        auto [file, oerr] =
                                Vfs::open(_userPath.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, Vfs::Context());
                        err = oerr;
                        if (err.isOk())
                                err = file->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), n), WaitForever)
                                              .error();
                }
        }
        if (err.isOk()) ProgramControls::userChanged(_id.view(), StringView(text, n));
        Memory::free(text);
        return err;
}

void ControlsBook::reset(size_t index) {
        if (index >= _current->count()) return;
        const int d = _defaults->indexOf(StringView(_current->action(index).name));
        if (d < 0) return;
        for (int pad = 0; pad < 2; ++pad) {
                char   text[256];
                size_t n = 0;
                if (_defaults->bindingText(_defaults->action(size_t(d)).id, pad == 1, text, sizeof(text), n).isOk())
                        (void)_current->setBinding(index, pad == 1, StringView(text, n));
        }
        return;
}

void ControlsBook::resetAll() {
        for (size_t i = 0; i < _current->count(); ++i) reset(i);
        return;
}

BMDS_NAMESPACE_END
