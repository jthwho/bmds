/**
 * @file      controlsapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/controls: the applications' actions and their bindings, changed (devplan/controls.md §7d).
 * An ordinary program on the public API, built in to have it at hand: nothing in the OS refers to
 * it. Its text form's output is a userspace interface once shipped.
 */

#include <cstdio>
#include <cstring>
#include <bmds/builtinprograms.h>
#include <bmds/fdtable.h>
#include <bmds/filehandle.h>
#include <bmds/programlauncher.h>
#include "controlsbook.h"
#include "controlswindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        void write(const BuiltinProgram::Context &context, int fd, const char *text) {
                FileHandle *h = context.fds != nullptr ? context.fds->handle(fd) : nullptr;
                if (h != nullptr)
                        (void)h->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                       WaitForever);
                return;
        }

        void out(const BuiltinProgram::Context &context, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);
        void out(const BuiltinProgram::Context &context, const char *fmt, ...) {
                char    line[512];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(line, sizeof(line), fmt, args);
                va_end(args);
                write(context, FdTable::Stdout, line);
                return;
        }

        void err(const BuiltinProgram::Context &context, const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);
        void err(const BuiltinProgram::Context &context, const char *fmt, ...) {
                char    line[512];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(line, sizeof(line), fmt, args);
                va_end(args);
                write(context, FdTable::Stderr, line);
                return;
        }

        StringView homeOf(const BuiltinProgram::Context &context) {
                for (const StringView &e : context.environment)
                        if (e.startsWith(StringView("HOME=")) && e.size() > 5) return e.mid(5);
                return StringView("/home");
        }

        constexpr const char *Usage = "usage: controls [-l | ID [list | bind ACTION keys|pad TEXT | reset [ACTION]]]\n";

        int listApps(const BuiltinProgram::Context &context) {
                List<ControlsBook::App> apps(MemSpace::Psram);
                if (ControlsBook::apps(apps).isError()) {
                        err(context, "controls: the applications: out of memory\n");
                        return 1;
                }
                out(context, "ID               NAME\n");
                for (const ControlsBook::App &a : apps) out(context, "%-16s %s\n", a.id.cStr(), a.name.cStr());
                return 0;
        }

        int list(const BuiltinProgram::Context &context, ControlsBook &book) {
                const Controls &c = book.current();
                out(context, "%-16s %-24s %-24s %s\n", "ACTION", "KEYS", "PAD", "LABEL");
                for (size_t i = 0; i < c.count(); ++i) {
                        const Controls::Action &a = c.action(i);
                        char                    keys[256], pad[256];
                        size_t                  n = 0;
                        (void)c.bindingText(a.id, false, keys, sizeof(keys), n);
                        (void)c.bindingText(a.id, true, pad, sizeof(pad), n);
                        out(context, "%-16s %-24s %-24s %s\n", a.name, keys, pad[0] != '\0' ? pad : "-", a.label);
                }
                return 0;
        }

        // The text form: list, bind, reset.
        int text(const BuiltinProgram::Context &context, ControlsBook &book) {
                const Span<const StringView> &args = context.args;
                const StringView              verb = args[2];
                if (verb == StringView("list") && args.size() == 3) return list(context, book);
                int index = -1;
                if (args.size() >= 4) {
                        index = book.current().indexOf(args[3]);
                        if (index < 0) {
                                err(context, "controls: %.*s has no action %.*s\n", int(book.id().size()),
                                    book.id().data(), int(args[3].size()), args[3].data());
                                return 1;
                        }
                }
                if (verb == StringView("bind") && args.size() >= 5) {
                        const bool pad = args[4] == StringView("pad");
                        if (!pad && args[4] != StringView("keys")) {
                                write(context, FdTable::Stderr, Usage);
                                return 2;
                        }
                        // The rest of the words are the binding (a pair: "Left Right").
                        char   binding[256] = {};
                        size_t used = 0;
                        for (size_t i = 5; i < args.size(); ++i) {
                                const int w = std::snprintf(binding + used, sizeof(binding) - used, "%s%.*s",
                                                            i == 5 ? "" : " ", int(args[i].size()), args[i].data());
                                if (w < 0 || size_t(w) >= sizeof(binding) - used) break;
                                used += size_t(w);
                        }
                        if (book.current().setBinding(size_t(index), pad, StringView(binding, used)).isError()) {
                                err(context, "controls: \"%s\": not a binding for it%s\n", binding,
                                    pad ? "" : " (an action keeps a key)");
                                return 1;
                        }
                } else if (verb == StringView("reset") && args.size() <= 4) {
                        if (index >= 0)
                                book.reset(size_t(index));
                        else
                                book.resetAll();
                } else {
                        write(context, FdTable::Stderr, Usage);
                        return 2;
                }
                const Error e = book.save();
                if (e.isError()) {
                        err(context, "controls: saving: %s\n", e.desc());
                        return 1;
                }
                return 0;
        }

        int controlsMain(const BuiltinProgram::Context &context) {
                const Span<const StringView> &args = context.args;
                if (args.size() == 2 && (args[1] == StringView("-h") || args[1] == StringView("--help"))) {
                        write(context, FdTable::Stdout, Usage);
                        return 0;
                }
                if (args.size() == 2 && args[1] == StringView("-l")) return listApps(context);
                if (args.size() <= 2)
                        return ControlsWindow::run(context, args.size() == 2 ? args[1] : StringView(), homeOf(context));
                ControlsBook *book = ControlsBook::create();
                if (book == nullptr) {
                        err(context, "controls: out of memory\n");
                        return 1;
                }
                int         status = 1;
                const Error e = book->load(args[1], homeOf(context));
                if (e == Error::NotExist)
                        err(context, "controls: %.*s: no such application with actions\n", int(args[1].size()),
                            args[1].data());
                else if (e.isError())
                        err(context, "controls: %.*s: %s\n", int(args[1].size()), args[1].data(), e.desc());
                else
                        status = text(context, *book);
                ControlsBook::destroy(book);
                return status;
        }

} // namespace

BMDS_PROGRAM(controls, controlsMain, 16 * 1024)

BMDS_NAMESPACE_END
