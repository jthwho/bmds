/**
 * @file      clipcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `clip`: copy, paste, list, clear (devplan/clipboard.md C8, §4e).
 */

#include <cinttypes>
#include <new>
#include <bmds/clipboard.h>
#include <bmds/clipboardreader.h>
#include <bmds/clipcommand.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Selection = Clipboard::Selection;

        // How long a paste waits for an owner's next bytes.
        constexpr unsigned int OwnerTimeoutMs = 5000;

        enum class Action : uint8_t {
                Paste,
                Copy,
                List,
                Clear
        };

        struct Options {
                        Action     action = Action::Paste;
                        bool       primary = false;
                        StringView types[Clipboard::MaxTypes];
                        size_t     typeCount = 0;
                        StringView file;
                        bool       hasFile = false;
        };

        const char *selectionName(Selection s) {
                return s == Selection::Primary ? "the primary selection" : "the clipboard";
        }

        // Parses the arguments; false: a usage error (reported).
        bool parse(CommandContext &ctx, Options &o) {
                const Span<char *const> args = ctx.args();
                size_t                  i = 1;
                if (i < args.size()) {
                        const StringView first(args[i]);
                        if (first == "paste" || first == "copy" || first == "list" || first == "clear") {
                                o.action = first == "paste"  ? Action::Paste
                                           : first == "copy" ? Action::Copy
                                           : first == "list" ? Action::List
                                                             : Action::Clear;
                                ++i;
                        }
                }
                bool options = true;
                for (; i < args.size(); ++i) {
                        const StringView a(args[i]);
                        if (options && a == "--") {
                                options = false;
                        } else if (options && a == "-p") {
                                o.primary = true;
                        } else if (options && a.startsWith("-t")) {
                                StringView type = a.mid(2);
                                if (type.isEmpty()) {
                                        if (++i == args.size()) return false;
                                        type = StringView(args[i]);
                                }
                                if (o.typeCount == Clipboard::MaxTypes) {
                                        ctx.eprintf("clip: %zu types at most\n", Clipboard::MaxTypes);
                                        return false;
                                }
                                if (!ClipboardType::isValid(type)) {
                                        ctx.eprintf("clip: not a type: %.*s\n", int(type.size()), type.data());
                                        return false;
                                }
                                o.types[o.typeCount++] = type;
                        } else if (options && a.size() > 1 && a.startsWith('-')) {
                                return false;
                        } else if (!o.hasFile && o.action == Action::Copy) {
                                o.file = a;
                                o.hasFile = true;
                        } else {
                                return false;
                        }
                }
                // One type for a paste; none for the others but a copy.
                if (o.action == Action::Paste && o.typeCount > 1) return false;
                if ((o.action == Action::List || o.action == Action::Clear) && o.typeCount != 0) return false;
                return true;
        }

        int copy(CommandContext &ctx, Clipboard &clipboard, const Options &o, Selection s) {
                CommandContext::Input input;
                const StringView      path = o.hasFile ? o.file : StringView("-");
                Error                 err = ctx.openInput(path, input);
                if (err.isError()) {
                        ctx.eprintf("clip: %.*s: %s\n", int(path.size()), path.data(), CommandContext::errorText(err));
                        return 1;
                }
                String  data(MemSpace::Psram);
                uint8_t buf[512];
                for (;;) {
                        auto [n, rerr] = input.read(WritableByteSpan(buf, sizeof(buf)));
                        if (rerr == Error::EndOfFile) break;
                        if (rerr == Error::Interrupted) return Shell::ExitInterrupted;
                        if (rerr.isError()) {
                                ctx.eprintf("clip: %.*s: %s\n", int(path.size()), path.data(),
                                            CommandContext::errorText(rerr));
                                return 1;
                        }
                        if (data.size() + n > clipboard.limit()) {
                                ctx.eprintf("clip: over the clipboard's %zu bytes\n", clipboard.limit());
                                return 1;
                        }
                        err = data.append(StringView(reinterpret_cast<const char *>(buf), n));
                        if (err.isError()) {
                                ctx.eprintf("clip: %s\n", err.desc());
                                return 1;
                        }
                }
                StringView types[Clipboard::MaxTypes];
                size_t     count = o.typeCount;
                for (size_t i = 0; i < count; ++i) types[i] = o.types[i];
                if (count == 0) {
                        const StringView byName = o.hasFile ? ClipboardType::fromFileName(o.file) : StringView();
                        types[count++] = byName.isEmpty() ? ClipboardType::Text : byName;
                }
                auto [serial, cerr] = clipboard.copy(
                        s, Span<const StringView>(types, count),
                        ByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size()), "clip", 0);
                (void)serial;
                if (cerr == Error::NoSpace) {
                        ctx.eprintf("clip: over the clipboard's %zu bytes\n", clipboard.limit());
                        return 1;
                }
                if (cerr.isError()) {
                        ctx.eprintf("clip: %s\n", cerr.desc());
                        return 1;
                }
                return Shell::ExitSuccess;
        }

        int paste(CommandContext &ctx, Clipboard &clipboard, const Options &o, Selection s) {
                const StringView wanted = o.typeCount != 0 ? o.types[0] : ClipboardType::Text;
                void            *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipboardReader));
                if (mem == nullptr) {
                        ctx.eprint("clip: out of memory\n");
                        return 1;
                }
                auto *reader = ::new (mem) ClipboardReader(clipboard);
                int   status = Shell::ExitSuccess;
                Error err = reader->open(s, wanted);
                if (err == Error::Empty) {
                        ctx.eprintf("clip: %s is empty\n", selectionName(s));
                        status = 1;
                } else if (err == Error::NotFound) {
                        ctx.eprintf("clip: %s can't be read as %.*s ('clip list' shows what it can)\n",
                                    selectionName(s), int(wanted.size()), wanted.data());
                        status = 1;
                } else if (err == Error::NotSupported) {
                        ctx.eprintf("clip: only a converter program makes %.*s (not yet)\n", int(wanted.size()),
                                    wanted.data());
                        status = 1;
                } else if (err.isError()) {
                        ctx.eprintf("clip: %s\n", err.desc());
                        status = 1;
                }
                while (status == Shell::ExitSuccess) {
                        uint8_t buf[512];
                        auto [n, rerr] = reader->read(WritableByteSpan(buf, sizeof(buf)), OwnerTimeoutMs);
                        if (rerr == Error::EndOfFile) break;
                        if (rerr.isError()) {
                                ctx.eprintf("clip: %s\n",
                                            rerr == Error::Timeout ? "its owner stopped answering" : rerr.desc());
                                status = rerr == Error::Interrupted ? Shell::ExitInterrupted : 1;
                                break;
                        }
                        if (ctx.write(1, ByteSpan(buf, n)).isError()) {
                                status = ctx.isInterrupted() ? Shell::ExitInterrupted : 1;
                                break;
                        }
                }
                reader->~ClipboardReader();
                Memory::free(reader);
                return status;
        }

        void list(CommandContext &ctx, const Clipboard &clipboard, Selection s) {
                const char *title = s == Selection::Primary ? "Primary" : "Clipboard";
                auto       *d = static_cast<Clipboard::Description *>(
                        Memory::alloc(MemSpace::Psram, sizeof(Clipboard::Description)));
                if (d == nullptr) {
                        ctx.eprint("clip: out of memory\n");
                        return;
                }
                ::new (d) Clipboard::Description();
                clipboard.describe(s, *d);
                if (d->serial == 0) {
                        ctx.printf("%s: empty\n", title);
                        Memory::free(d);
                        return;
                }
                ctx.printf("%s: from %s", title, d->owner[0] != '\0' ? d->owner : "?");
                if (d->process != 0 && d->window != 0)
                        ctx.printf(" (pid %" PRId32 ", window %u)", d->process, unsigned(d->window));
                else if (d->process != 0)
                        ctx.printf(" (pid %" PRId32 ")", d->process);
                else if (d->window != 0)
                        ctx.printf(" (window %u)", unsigned(d->window));
                ctx.print("\n");
                StringView offered[Clipboard::MaxTypes];
                for (size_t i = 0; i < d->count; ++i) {
                        offered[i] = StringView(d->types[i].type);
                        if (d->types[i].size >= 0)
                                ctx.printf("  %-30s %" PRId64 " bytes\n", d->types[i].type, d->types[i].size);
                        else
                                ctx.printf("  %-30s on request\n", d->types[i].type);
                }
                // What else it can be read as: each converter's type a way reaches, once.
                const ClipboardConversions *table = clipboard.conversions();
                bool                        any = false;
                for (size_t c = 0; table != nullptr && c < table->count(); ++c) {
                        const StringView to = table->converter(c).to;
                        bool             seen = false;
                        for (size_t i = 0; i < d->count && !seen; ++i) seen = ClipboardType::equal(offered[i], to);
                        for (size_t e = 0; e < c && !seen; ++e) seen = ClipboardType::equal(table->converter(e).to, to);
                        if (seen || table->find(Span<const StringView>(offered, d->count), to).error().isError())
                                continue;
                        char normal[ClipboardType::BufferBytes];
                        auto [n, err] = ClipboardType::normalize(to, Span<char>(normal, sizeof(normal)));
                        if (err.isError()) continue;
                        ctx.printf("%s%.*s", any ? ", " : "  also as: ", int(n.size()), n.data());
                        any = true;
                }
                if (any) ctx.print("\n");
                Memory::free(d);
                return;
        }

} // namespace

int ClipCommand::run(CommandContext &ctx) {
        Options o;
        if (!parse(ctx, o)) return ctx.usage();
        auto [clipboard, err] = Clipboard::clipboard(0);
        if (err.isError()) {
                ctx.eprint("clip: no clipboard\n");
                return 1;
        }
        const Selection s = o.primary ? Selection::Primary : Selection::Clipboard;
        switch (o.action) {
                case Action::Copy: return copy(ctx, *clipboard, o, s);
                case Action::Paste: return paste(ctx, *clipboard, o, s);
                case Action::List:
                        if (!o.primary) list(ctx, *clipboard, Selection::Clipboard);
                        list(ctx, *clipboard, Selection::Primary);
                        return Shell::ExitSuccess;
                case Action::Clear: (void)clipboard->clear(s, 0); return Shell::ExitSuccess;
        }
        return Shell::ExitSuccess;
}

BMDS_NAMESPACE_END
