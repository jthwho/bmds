/**
 * @file      controlscalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The program's actions (devplan/controls.md §7c): its declaration (its folder's controls.conf,
 * or text), the user's bindings laid over it, the active set, a binding as text.
 */

#include <cstdio>
#include <cstring>
#include <bmds/errno.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/process.h>
#include <bmds/programcontrols.h>
#include <bmds/programsurface.h>
#include <bmds/sys.h>
#include <bmds/vfs.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::KernelCall;
using bmds::StringView;

namespace {

        static_assert(BMDS_ACTION_PRESS == uint32_t(bmds::Controls::Output::Type::Press) &&
                              BMDS_ACTION_RELEASE == uint32_t(bmds::Controls::Output::Type::Release) &&
                              BMDS_ACTION_REPEAT == uint32_t(bmds::Controls::Output::Type::Repeat) &&
                              BMDS_ACTION_AXIS == uint32_t(bmds::Controls::Output::Type::Axis),
                      "the ABI's action flags");
        static_assert(BMDS_SOURCE_KEYBOARD == bmds::Controls::Keyboard && BMDS_SOURCE_PAD == bmds::Controls::Pad,
                      "the ABI's sources");

        constexpr size_t MaxText = bmds::ProgramControls::MaxDeclaration; // a declaration's, a user's file's
        constexpr size_t MaxSetName = 64;
        constexpr size_t MaxPath = 256;

        // The value of environment variable @p name (empty: none).
        StringView environment(char **envp, StringView name) {
                for (char **e = envp; e != nullptr && *e != nullptr; ++e) {
                        const StringView entry(*e);
                        if (entry.size() > name.size() && entry.startsWith(name) && entry[name.size()] == '=')
                                return entry.mid(name.size() + 1);
                }
                return StringView();
        }

        // A file's text, at most MaxText bytes, in @p buffer (Psram). @return Its length; 0: none.
        size_t readFile(StringView path, char *buffer) {
                auto [file, err] = bmds::Vfs::open(path, bmds::Vfs::Read, bmds::Vfs::Context());
                if (err.isError()) return 0;
                size_t got = 0;
                while (got < MaxText) {
                        auto [n, rerr] = file->read(
                                bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(buffer) + got, MaxText - got),
                                bmds::WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                return got;
        }

        struct Buffer {
                        char *text = static_cast<char *>(bmds::Memory::alloc(bmds::MemSpace::Psram, MaxText));
                        ~Buffer() { bmds::Memory::free(text); }
        };

        int32_t declare(KernelCall &call, const void *arg, uint32_t size) {
                bmds::Process         &process = *call.operator->();
                bmds::ProgramControls *controls = process.controls();
                if (controls == nullptr) return -BMDS_ENOMEM;
                Buffer declaration, user;
                if (declaration.text == nullptr || user.text == nullptr) return -BMDS_ENOMEM;
                char       path[MaxPath];
                StringView text;
                if (arg == nullptr) { // the folder's controls.conf
                        const StringView dir = environment(process.envp(), "BMDS_APP_DIR");
                        if (dir.isEmpty()) return -BMDS_ENOENT;
                        const int n =
                                std::snprintf(path, sizeof(path), "%.*s/controls.conf", int(dir.size()), dir.data());
                        if (n < 0 || size_t(n) >= sizeof(path)) return -BMDS_ENAMETOOLONG;
                        const size_t got = readFile(StringView(path, size_t(n)), declaration.text);
                        if (got == 0) return -BMDS_ENOENT;
                        text = StringView(declaration.text, got);
                } else {
                        if (size > MaxText) return -BMDS_E2BIG;
                        std::memcpy(declaration.text, arg, size);
                        text = StringView(declaration.text, size);
                }
                // The user's changes: $HOME/.config/bmds/controls/ID.conf.
                StringView       userText;
                const StringView id = environment(process.envp(), "BMDS_APP_ID");
                StringView       home = environment(process.envp(), "HOME");
                if (home.isEmpty()) home = "/home";
                if (!id.isEmpty()) {
                        const int n = std::snprintf(path, sizeof(path), "%.*s/.config/bmds/controls/%.*s.conf",
                                                    int(home.size()), home.data(), int(id.size()), id.data());
                        if (n > 0 && size_t(n) < sizeof(path))
                                userText = StringView(user.text, readFile(StringView(path, size_t(n)), user.text));
                }
                uint32_t    line = 0;
                const Error err = controls->declare(text, userText, id, line);
                if (err.isError()) {
                        bmdsWarn("controls: %.*s: refused (%s, line %lu)", int(id.size()), id.data(), err.desc(),
                                 static_cast<unsigned long>(line));
                        return err == Error::Full ? -BMDS_E2BIG : -BMDS_EINVAL;
                }
                // Its surfaces match against it from now on (those opened later get it at open).
                for (int fd = 0; fd < bmds::FdTable::MaxFds; ++fd) {
                        const bmds::FileHandle::Ptr h = process.handle(fd);
                        if (!h) continue;
                        if (bmds::ProgramSurface *s = h->stream().programSurface(); s != nullptr)
                                s->setControls(controls);
                }
                return 0;
        }

} // namespace

extern "C" {

int32_t bmds_k_controls(uint32_t op, void *arg, uint32_t size) {
        KernelCall call;
        switch (op) {
                case BMDS_CONTROLS_DECLARE: return declare(call, arg, size);
                case BMDS_CONTROLS_SET: {
                        bmds::ProgramControls *controls = call->existingControls();
                        if (controls == nullptr) return -BMDS_ENOENT; // nothing declared
                        if (arg != nullptr && size > MaxSetName) return -BMDS_EINVAL;
                        const Error err = controls->setSet(
                                arg != nullptr ? StringView(static_cast<const char *>(arg), size) : StringView());
                        return err.isOk() ? 0 : err == Error::NotExist ? -BMDS_ENOENT : -BMDS_EINVAL;
                }
                case BMDS_CONTROLS_BINDING: {
                        bmds::ProgramControls *controls = call->existingControls();
                        if (controls == nullptr) return -BMDS_ENOENT;
                        bmds_controls_binding_t b;
                        int32_t r = bmds::detail::copyIn(static_cast<const bmds_controls_binding_t *>(arg), b);
                        if (r < 0) return r;
                        if (b.id == 0 || b.id > 65535 || (b.which != BMDS_BINDING_KEYS && b.which != BMDS_BINDING_PAD))
                                return -BMDS_EINVAL;
                        if (b.text == nullptr || b.text_size == 0) return -BMDS_EFAULT;
                        char        text[256];
                        size_t      length = 0;
                        const Error err = controls->bindingText(uint16_t(b.id), b.which == BMDS_BINDING_PAD, text,
                                                                sizeof(text), length);
                        if (err.isError()) return -BMDS_ENOENT;
                        bmds::detail::copyText(StringView(text, length), b.text, b.text_size);
                        return int32_t(length);
                }
                default: return -BMDS_EINVAL;
        }
}

} // extern "C"
