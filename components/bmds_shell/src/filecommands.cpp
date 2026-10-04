/**
 * @file      filecommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * File built-ins. Their arguments and output are a userspace interface; they
 * follow the GNU tools they are named after where those are simple. Errors
 * are "command: path: reason", with POSIX's wording.
 */

#include <cstring>
#include <utility>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/filecommands.h>
#include <bmds/glob.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/utf8.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // Copies move this much at a time, in DMA-capable memory (whole
        // sectors go straight to the card).
        constexpr size_t CopyBufferBytes = 32 * 1024;

        // Deepest directory tree cp -r, mv and rm -r walk.
        constexpr int MaxDepth = 64;

        // Reports @p err for @p path; returns ExitFailure.
        int fail(CommandContext &ctx, StringView path, Error err) {
                ctx.eprintf("%s: %.*s: %s\n", ctx.command().name, static_cast<int>(path.size()), path.data(),
                            CommandContext::errorText(err));
                return Shell::ExitFailure;
        }

        // Single-letter options from the front of the arguments ("-la", "--"
        // ends them). Sets the bit of each letter of @p letters seen; returns
        // the index of the first operand, or 0 for an unknown option.
        size_t parseOptions(Args args, const char *letters, unsigned &flags) {
                flags = 0;
                size_t i = 1;
                for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                        if (StringView(args[i]) == "--") return i + 1;
                        for (const char *o = args[i] + 1; *o != '\0'; ++o) {
                                const size_t at = StringView(letters).indexOf(*o);
                                if (at == StringView::NPos) return 0;
                                flags |= 1u << at;
                        }
                }
                return i;
        }

        // @p dir and @p name joined with one '/'.
        Error join(StringView dir, StringView name, String &out) {
                out.clear();
                Error err = out.assign(dir);
                if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(name);
                return err;
        }

        // The last component of @p path ("a/b/" gives "b").
        StringView baseName(StringView path) {
                while (path.size() > 1 && path.endsWith('/')) path = path.left(path.size() - 1);
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        // Reports @p err for @p path; returns false (for the tree walkers).
        bool report(CommandContext &ctx, StringView path, Error err) {
                fail(ctx, path, err);
                return false;
        }

        // A normalized absolute path, or an empty string on error.
        String absolute(CommandContext &ctx, StringView path) {
                Result<String> normalized = Vfs::normalize(path, ctx.cwd());
                return normalized.error().isOk() ? std::move(normalized).value() : String();
        }

        void sortByName(List<FileInfo> &entries) {
                // Insertion sort: listings are short, and FileInfo only moves.
                for (size_t i = 1; i < entries.size(); ++i) {
                        FileInfo item = std::move(entries[i]);
                        size_t   j = i;
                        for (; j > 0 && Glob::nameLess(item.name, entries[j - 1].name); --j)
                                entries[j] = std::move(entries[j - 1]);
                        entries[j] = std::move(item);
                }
                return;
        }

        bool isHidden(const FileInfo &info) {
                // A link is a System file underneath; it isn't hidden for that.
                const uint8_t hiding = info.type == FileType::Link ? FileInfo::Hidden : FileInfo::Hidden | FileInfo::System;
                return info.name.view().startsWith('.') || (info.attributes & hiding) != 0;
        }

        // --- ls ---------------------------------------------------------------

        enum LsFlag : unsigned {
                LsLong = 1,
                LsAll = 2,
                LsOnePerLine = 4
        };

        struct LsStyle {
                        bool     color = false;
                        bool     columns = false;
                        unsigned width = 80;
                        TimeZone zone;
        };

        // The name as listed: directories get a '/', colors when allowed.
        void appendName(String &out, const FileInfo &info, const LsStyle &style) {
                const char *color = nullptr;
                if (style.color && info.type == FileType::Directory) color = "\x1b[1;34m";
                if (style.color && info.type == FileType::Device) color = "\x1b[33m";
                if (style.color && info.type == FileType::Link) color = "\x1b[1;36m";
                if (color != nullptr) (void)out.append(color);
                (void)out.append(info.name.view());
                if (info.type == FileType::Directory) (void)out.append('/');
                if (color != nullptr) (void)out.append("\x1b[0m");
                return;
        }

        size_t listedWidth(const FileInfo &info) {
                return Utf8::displayWidth(info.name.view()) + (info.type == FileType::Directory ? 1 : 0);
        }

        void printLong(CommandContext &ctx, const List<FileInfo> &entries, const LsStyle &style, StringView dir) {
                int sizeWidth = 1;
                for (const FileInfo &e : entries) {
                        int digits = 1;
                        for (uint64_t v = e.size; v >= 10; v /= 10) ++digits;
                        if (digits > sizeWidth) sizeWidth = digits;
                }
                for (const FileInfo &e : entries) {
                        String line;
                        // A listing calls a link by its attributes; reading it tells for sure.
                        String target;
                        bool   link = false;
                        if (e.type == FileType::Link) {
                                String path;
                                if (dir.isEmpty() ? path.assign(e.name).isOk() : join(dir, e.name.view(), path).isOk()) {
                                        auto [t, err] = Vfs::readLink(path.view(), ctx.vfs());
                                        link = err.isOk();
                                        if (link) target = std::move(t);
                                }
                        }
                        const char type = e.type == FileType::Directory ? 'd'
                                          : e.type == FileType::Device  ? 'c'
                                          : link                        ? 'l'
                                                                        : '-';
                        (void)line.appendf("%cr%c-  %*llu  ", type, (e.attributes & FileInfo::ReadOnly) ? '-' : 'w',
                                           sizeWidth, static_cast<unsigned long long>(e.size));
                        if (e.modified.ns == 0) {
                                (void)line.append("                ");
                        } else {
                                (void)formatTime(line, "%Y-%m-%d %H:%M", style.zone.localTime(e.modified));
                        }
                        (void)line.append("  ");
                        appendName(line, e, style);
                        if (link) {
                                (void)line.append(" -> ");
                                (void)line.append(target);
                        }
                        (void)line.append('\n');
                        ctx.print(line);
                }
                return;
        }

        void printShort(CommandContext &ctx, const List<FileInfo> &entries, const LsStyle &style) {
                const size_t n = entries.size();
                if (n == 0) return;
                if (!style.columns) {
                        for (const FileInfo &e : entries) {
                                String line;
                                appendName(line, e, style);
                                (void)line.append('\n');
                                ctx.print(line);
                        }
                        return;
                }
                // As GNU ls: the most columns that fit, filled top to bottom.
                size_t cols = n;
                size_t rows = 1;
                for (; cols > 1; --cols) {
                        rows = (n + cols - 1) / cols;
                        size_t total = 0;
                        for (size_t c = 0; c < cols; ++c) {
                                size_t widest = 0;
                                for (size_t r = 0; r < rows && c * rows + r < n; ++r) {
                                        const size_t w = listedWidth(entries[c * rows + r]);
                                        if (w > widest) widest = w;
                                }
                                total += widest + (c + 1 < cols ? 2 : 0);
                        }
                        if (total <= style.width) break;
                }
                rows = (n + cols - 1) / cols;
                size_t colWidth[64] = {};
                if (cols > 64) cols = 64;
                for (size_t c = 0; c < cols; ++c) {
                        for (size_t r = 0; r < rows && c * rows + r < n; ++r) {
                                const size_t w = listedWidth(entries[c * rows + r]);
                                if (w > colWidth[c]) colWidth[c] = w;
                        }
                }
                for (size_t r = 0; r < rows && !ctx.isInterrupted(); ++r) {
                        String line;
                        for (size_t c = 0; c < cols; ++c) {
                                const size_t i = c * rows + r;
                                if (i >= n) break;
                                appendName(line, entries[i], style);
                                const bool last = c + 1 == cols || (c + 1) * rows + r >= n;
                                if (!last) {
                                        for (size_t pad = listedWidth(entries[i]); pad < colWidth[c] + 2; ++pad)
                                                (void)line.append(' ');
                                }
                        }
                        (void)line.append('\n');
                        ctx.print(line);
                }
                return;
        }

        void printEntries(CommandContext &ctx, List<FileInfo> &entries, unsigned flags, const LsStyle &style,
                          StringView dir) {
                sortByName(entries);
                if (flags & LsLong) {
                        printLong(ctx, entries, style, dir);
                } else {
                        printShort(ctx, entries, style);
                }
                return;
        }

        Error listDirectory(CommandContext &ctx, StringView path, unsigned flags, List<FileInfo> &entries) {
                auto [dir, err] = Vfs::openDirectory(path, ctx.vfs());
                if (err.isError()) return err;
                for (;;) {
                        if (ctx.isInterrupted()) return Error::Interrupted;
                        auto [info, nextErr] = dir.next();
                        if (nextErr == Error::EndOfFile) return Error();
                        if (nextErr.isError()) return nextErr;
                        if (!(flags & LsAll) && isHidden(info)) continue;
                        const Error appended = entries.append(std::move(info));
                        if (appended.isError()) return appended;
                }
        }

        // --- copying and removing trees -----------------------------------------

        Error copyFile(CommandContext &ctx, StringView from, StringView to, uint8_t *buf) {
                const Vfs::Context vfs = ctx.vfs();
                auto [in, inErr] = Vfs::open(from, Vfs::Read, vfs);
                if (inErr.isError()) return inErr;
                auto [out, outErr] = Vfs::open(to, Vfs::Write | Vfs::Create | Vfs::Truncate, vfs);
                if (outErr.isError()) return outErr;
                for (;;) {
                        if (ctx.isInterrupted()) return Error::Interrupted;
                        auto [n, err] = in->read(WritableByteSpan(buf, CopyBufferBytes), WaitForever);
                        if (err == Error::EndOfFile) break;
                        if (err.isError()) return err;
                        const Error written = out->write(ByteSpan(buf, n), WaitForever).error();
                        if (written.isError()) return written;
                }
                return out->stream().flush(WaitForever);
        }

        // Copies @p from (a file, or with @p recursive a directory tree) to
        // @p to. Reports errors itself; returns false if any.
        bool copyTree(CommandContext &ctx, StringView from, StringView to, bool recursive, uint8_t *buf, int depth) {
                const Vfs::Context vfs = ctx.vfs();
                // A link named on the command line is followed; one inside a
                // tree is copied as a link (as GNU cp -R does).
                auto [info, err] = depth == 0 ? Vfs::stat(from, vfs) : Vfs::lstat(from, vfs);
                if (err.isError()) return report(ctx, from, err);
                if (info.type == FileType::Link) {
                        auto [target, linkErr] = Vfs::readLink(from, vfs);
                        err = linkErr.isOk() ? Vfs::symlink(target.view(), to, vfs) : linkErr;
                        return err.isOk() || report(ctx, to, err);
                }
                if (!info.isDirectory()) {
                        err = copyFile(ctx, from, to, buf);
                        if (err == Error::Interrupted) return false;
                        return err.isOk() || report(ctx, to, err);
                }
                if (!recursive) {
                        ctx.eprintf("%s: %.*s: Is a directory (not copied without -r)\n", ctx.command().name,
                                    static_cast<int>(from.size()), from.data());
                        return false;
                }
                if (depth > MaxDepth) return report(ctx, from, Error::OutOfRange);
                err = Vfs::mkdir(to, vfs);
                if (err == Error::Exists) {
                        auto [target, targetErr] = Vfs::stat(to, vfs);
                        err = targetErr.isOk() && target.isDirectory() ? Error() : Error(Error::NotDir);
                }
                if (err.isError()) return report(ctx, to, err);
                List<FileInfo> entries;
                err = listDirectory(ctx, from, LsAll, entries);
                if (err.isError()) return err != Error::Interrupted && report(ctx, from, err);
                bool ok = true;
                for (const FileInfo &e : entries) {
                        if (ctx.isInterrupted()) return false;
                        String childFrom;
                        String childTo;
                        if (join(from, e.name.view(), childFrom).isError() ||
                            join(to, e.name.view(), childTo).isError())
                                return report(ctx, from, Error::NoMem);
                        ok = copyTree(ctx, childFrom.view(), childTo.view(), true, buf, depth + 1) && ok;
                }
                return ok;
        }

        // Removes @p path (a file, or with @p recursive a directory tree).
        Error removeTree(CommandContext &ctx, StringView path, bool recursive, int depth) {
                const Vfs::Context vfs = ctx.vfs();
                // A link is removed itself, never what it leads to.
                auto [info, err] = Vfs::lstat(path, vfs);
                if (err.isError()) return err;
                if (!info.isDirectory()) return Vfs::remove(path, vfs);
                if (!recursive) return Error::IsDir;
                if (depth > MaxDepth) return Error::OutOfRange;
                // Names first, then removal: the directory isn't changed while open.
                List<FileInfo> entries;
                err = listDirectory(ctx, path, LsAll, entries);
                if (err.isError()) return err;
                for (const FileInfo &e : entries) {
                        if (ctx.isInterrupted()) return Error::Interrupted;
                        String child;
                        err = join(path, e.name.view(), child);
                        if (err.isOk()) err = removeTree(ctx, child.view(), true, depth + 1);
                        if (err.isError()) return err;
                }
                return Vfs::rmdir(path, vfs);
        }

        // Where source @p from goes for cp/mv: into @p target if it is a
        // directory, else @p target itself.
        Error destination(CommandContext &ctx, StringView from, StringView target, bool targetIsDir, String &out) {
                if (targetIsDir) return join(target, baseName(from), out);
                (void)ctx;
                return out.assign(target);
        }

        // Checks that copying or moving @p from to @p to makes sense.
        bool sameOrInside(CommandContext &ctx, StringView from, StringView to) {
                const String a = absolute(ctx, from);
                const String b = absolute(ctx, to);
                if (a.isEmpty() || b.isEmpty()) return false;
                if (a.view() == b.view()) {
                        ctx.eprintf("%s: %s and %s are the same file\n", ctx.command().name, a.cStr(), b.cStr());
                        return true;
                }
                if (b.view().startsWith(a.view()) && b.size() > a.size() && b[a.size()] == '/') {
                        ctx.eprintf("%s: cannot copy %s into itself\n", ctx.command().name, a.cStr());
                        return true;
                }
                return false;
        }

        // The operands of cp/mv: sources, the target, and whether files go into it.
        bool targetOf(CommandContext &ctx, Args operands, bool &intoDir) {
                const StringView target = operands[operands.size() - 1];
                auto [info, err] = Vfs::stat(target, ctx.vfs());
                intoDir = err.isOk() && info.isDirectory();
                if (operands.size() > 2 && !intoDir) {
                        ctx.eprintf("%s: %.*s: Not a directory\n", ctx.command().name, static_cast<int>(target.size()),
                                    target.data());
                        return false;
                }
                return true;
        }

        // --- hexdump ----------------------------------------------------------

        void hexLine(CommandContext &ctx, uint64_t offset, const uint8_t *bytes, size_t n) {
                String line;
                (void)line.appendf("%08llx  ", static_cast<unsigned long long>(offset));
                for (size_t i = 0; i < 16; ++i) {
                        if (i < n) {
                                (void)line.appendf("%02x ", bytes[i]);
                        } else {
                                (void)line.append("   ");
                        }
                        if (i == 7) (void)line.append(' ');
                }
                (void)line.append(" |");
                for (size_t i = 0; i < n; ++i)
                        (void)line.append(bytes[i] >= 0x20 && bytes[i] < 0x7f ? char(bytes[i]) : '.');
                (void)line.append("|\n");
                ctx.print(line);
                return;
        }

} // namespace

// --- Commands ----------------------------------------------------------------

int FileCommands::pwd(CommandContext &ctx) {
        if (ctx.args().size() > 1) return ctx.usage();
        const StringView cwd = ctx.cwd();
        ctx.printf("%.*s\n", static_cast<int>(cwd.size()), cwd.data());
        return Shell::ExitSuccess;
}

int FileCommands::cd(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() > 2) {
                ctx.eprint("cd: too many arguments\n");
                return Shell::ExitFailure;
        }
        ShellState &state = ctx.executor().state();
        StringView  target;
        bool        announce = false;
        if (args.size() == 1 || StringView(args[1]) == "-") {
                const char   *var = args.size() == 1 ? "HOME" : "OLDPWD";
                const String *value = state.vars.value(var);
                if (value == nullptr || value->isEmpty()) {
                        ctx.eprintf("cd: %s not set\n", var);
                        return Shell::ExitFailure;
                }
                target = value->view();
                announce = args.size() == 2;
        } else {
                target = args[1];
        }
        // The physical path (links followed once, here, as cd -P).
        Result<String> real = Vfs::realPath(target, ctx.vfs());
        if (real.error().isError()) return fail(ctx, target, real.error());
        String to = std::move(real).value();
        auto [info, err] = Vfs::stat(to.view(), ctx.vfs());
        if (err.isOk() && !info.isDirectory()) err = Error::NotDir;
        if (err.isError()) return fail(ctx, target, err);
        String from;
        err = from.assign(ctx.cwd());
        if (err.isOk()) err = state.vars.set("OLDPWD", from.view());
        if (err.isOk()) err = state.vars.set("PWD", to.view());
        if (err.isError()) return fail(ctx, target, err);
        state.cwd = std::move(to);
        if (announce) ctx.printf("%s\n", state.cwd.cStr());
        return Shell::ExitSuccess;
}

int FileCommands::ls(CommandContext &ctx) {
        const Args   args = ctx.args();
        unsigned     flags = 0;
        const size_t first = parseOptions(args, "la1", flags);
        if (first == 0) return ctx.usage();
        static char *const Here[] = {const_cast<char *>(".")};
        const Args         operands = first < args.size() ? args.mid(first) : Args(Here);

        LsStyle     style;
        FileHandle *out = ctx.fds().handle(FdTable::Stdout);
        const bool  terminal = out != nullptr && out->stream().isTerminal();
        style.color = ctx.colorDepth() != ColorDepth::None;
        style.columns = terminal && !(flags & LsOnePerLine);
        style.width = ctx.shell().console().size().columns;
        const String *columns = ctx.executor().state().vars.value("COLUMNS");
        if (columns != nullptr) {
                unsigned v = 0;
                for (char c : columns->view()) v = (c >= '0' && c <= '9') ? v * 10 + unsigned(c - '0') : 0;
                if (v > 0) style.width = v;
        }
        style.zone = ctx.displayZone();

        // GNU's order: the files named, together; then each directory.
        int            status = Shell::ExitSuccess;
        List<FileInfo> files;
        List<size_t>   dirs;
        for (size_t i = 0; i < operands.size(); ++i) {
                // As GNU ls: a link operand is listed itself with -l, followed without.
                auto [info, err] = (flags & LsLong) ? Vfs::lstat(operands[i], ctx.vfs()) : Vfs::stat(operands[i], ctx.vfs());
                if (err.isError()) {
                        fail(ctx, operands[i], err);
                        status = Shell::ExitUsage; // GNU: 2 when an operand can't be reached
                        continue;
                }
                if (info.isDirectory()) {
                        (void)dirs.append(i);
                } else {
                        // Listed as named, not by its last component.
                        (void)info.name.assign(operands[i]);
                        (void)files.append(std::move(info));
                }
        }
        printEntries(ctx, files, flags, style, StringView());
        const bool headers = operands.size() > 1;
        for (size_t d = 0; d < dirs.size() && !ctx.isInterrupted(); ++d) {
                const char *path = operands[dirs[d]];
                if (headers) ctx.printf("%s%s:\n", (d > 0 || !files.isEmpty()) ? "\n" : "", path);
                List<FileInfo> entries;
                const Error    err = listDirectory(ctx, path, flags, entries);
                if (err == Error::Interrupted) break;
                if (err.isError()) {
                        fail(ctx, path, err);
                        status = Shell::ExitUsage;
                        continue;
                }
                printEntries(ctx, entries, flags, style, path);
        }
        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
        return status;
}

int FileCommands::mkdir(CommandContext &ctx) {
        const Args   args = ctx.args();
        unsigned     flags = 0;
        const size_t first = parseOptions(args, "p", flags);
        if (first == 0 || first >= args.size()) return ctx.usage();
        const bool parents = flags & 1;
        int        status = Shell::ExitSuccess;
        for (size_t i = first; i < args.size(); ++i) {
                if (!parents) {
                        const Error err = Vfs::mkdir(args[i], ctx.vfs());
                        if (err.isError()) status = fail(ctx, args[i], err);
                        continue;
                }
                // -p: each missing directory on the way; existing ones are fine.
                const String path = absolute(ctx, args[i]);
                if (path.isEmpty()) {
                        status = fail(ctx, args[i], Error::Invalid);
                        continue;
                }
                for (size_t end = 1; end <= path.size(); ++end) {
                        if (end < path.size() && path[end] != '/') continue;
                        const StringView prefix = path.view().left(end);
                        auto [info, err] = Vfs::stat(prefix, ctx.vfs());
                        if (err == Error::NotExist)
                                err = Vfs::mkdir(prefix, ctx.vfs());
                        else if (err.isOk() && !info.isDirectory())
                                err = Error::NotDir;
                        if (err.isError()) {
                                status = fail(ctx, args[i], err);
                                break;
                        }
                }
        }
        return status;
}

int FileCommands::rmdir(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() < 2) return ctx.usage();
        int status = Shell::ExitSuccess;
        for (size_t i = 1; i < args.size(); ++i) {
                const Error err = Vfs::rmdir(args[i], ctx.vfs());
                if (err.isError()) status = fail(ctx, args[i], err);
        }
        return status;
}

int FileCommands::rm(CommandContext &ctx) {
        const Args   args = ctx.args();
        unsigned     flags = 0;
        const size_t first = parseOptions(args, "rf", flags);
        if (first == 0 || (first >= args.size() && !(flags & 2))) return ctx.usage();
        const bool recursive = flags & 1;
        const bool force = flags & 2;
        int        status = Shell::ExitSuccess;
        for (size_t i = first; i < args.size(); ++i) {
                const Error err = removeTree(ctx, args[i], recursive, 0);
                if (err == Error::Interrupted) return Shell::ExitInterrupted;
                // -f: a missing file is not an error.
                // -f: a name FAT can't hold (`rm -f *.o` with nothing to match)
                // can't exist either.
                const bool missing = err == Error::NotExist || err == Error::Invalid;
                if (err.isError() && !(force && missing)) status = fail(ctx, args[i], err);
        }
        return status;
}

int FileCommands::touch(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() < 2) return ctx.usage();
        int             status = Shell::ExitSuccess;
        const Timestamp now = Clock::now();
        for (size_t i = 1; i < args.size(); ++i) {
                auto [info, err] = Vfs::stat(args[i], ctx.vfs());
                if (err == Error::NotExist) {
                        err = Vfs::open(args[i], Vfs::Write | Vfs::Create, ctx.vfs()).error();
                } else if (err.isOk()) {
                        err = Vfs::setModified(args[i], now, ctx.vfs());
                }
                if (err.isError()) status = fail(ctx, args[i], err);
        }
        return status;
}

int FileCommands::cp(CommandContext &ctx) {
        const Args   args = ctx.args();
        unsigned     flags = 0;
        const size_t first = parseOptions(args, "r", flags);
        if (first == 0 || args.size() - first < 2) return ctx.usage();
        const Args operands = args.mid(first);
        bool       intoDir = false;
        if (!targetOf(ctx, operands, intoDir)) return Shell::ExitFailure;
        auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, CopyBufferBytes));
        if (buf == nullptr) return fail(ctx, operands[0], Error::NoMem);
        int status = Shell::ExitSuccess;
        for (size_t i = 0; i + 1 < operands.size() && !ctx.isInterrupted(); ++i) {
                String to;
                if (destination(ctx, operands[i], operands[operands.size() - 1], intoDir, to).isError()) {
                        status = fail(ctx, operands[i], Error::NoMem);
                        continue;
                }
                if (sameOrInside(ctx, operands[i], to.view())) {
                        status = Shell::ExitFailure;
                        continue;
                }
                if (!copyTree(ctx, operands[i], to.view(), flags & 1, buf, 0)) status = Shell::ExitFailure;
        }
        Memory::free(buf);
        return ctx.isInterrupted() ? Shell::ExitInterrupted : status;
}

int FileCommands::mv(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() < 3) return ctx.usage();
        const Args operands = args.mid(1);
        bool       intoDir = false;
        if (!targetOf(ctx, operands, intoDir)) return Shell::ExitFailure;
        int status = Shell::ExitSuccess;
        for (size_t i = 0; i + 1 < operands.size() && !ctx.isInterrupted(); ++i) {
                const StringView from = operands[i];
                String           to;
                if (destination(ctx, from, operands[operands.size() - 1], intoDir, to).isError()) {
                        status = fail(ctx, from, Error::NoMem);
                        continue;
                }
                if (sameOrInside(ctx, from, to.view())) {
                        status = Shell::ExitFailure;
                        continue;
                }
                Error err = Vfs::rename(from, to.view(), ctx.vfs());
                if (err == Error::Exists) {
                        // mv replaces a file (not a directory) in the way.
                        auto [target, targetErr] = Vfs::stat(to.view(), ctx.vfs());
                        if (targetErr.isOk() && !target.isDirectory()) err = Vfs::remove(to.view(), ctx.vfs());
                        if (err.isOk()) err = Vfs::rename(from, to.view(), ctx.vfs());
                }
                if (err == Error::NotSupported) {
                        // Across filesystems: copy, then remove the original.
                        auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, CopyBufferBytes));
                        if (buf == nullptr) {
                                status = fail(ctx, from, Error::NoMem);
                                continue;
                        }
                        const bool copied = copyTree(ctx, from, to.view(), true, buf, 0);
                        Memory::free(buf);
                        if (!copied) {
                                status = Shell::ExitFailure;
                                continue;
                        }
                        err = removeTree(ctx, from, true, 0);
                }
                if (err.isError()) status = fail(ctx, from, err);
        }
        return ctx.isInterrupted() ? Shell::ExitInterrupted : status;
}

int FileCommands::ln(CommandContext &ctx) {
        const Args args = ctx.args();
        size_t     i = 1;
        bool       symbolic = false;
        bool       force = false;
        for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                for (const char *o = args[i] + 1; *o != '\0'; ++o) {
                        if (*o == 's') {
                                symbolic = true;
                        } else if (*o == 'f') {
                                force = true;
                        } else {
                                return ctx.usage();
                        }
                }
        }
        if (args.size() - i < 2) return ctx.usage();
        if (!symbolic) {
                ctx.eprint("ln: hard links: Operation not supported (FAT has none; use ln -s)\n");
                return Shell::ExitFailure;
        }
        const Vfs::Context vfs = ctx.vfs();
        const StringView   last = args[args.size() - 1];
        auto [lastInfo, lastErr] = Vfs::stat(last, vfs);
        const bool intoDir = lastErr.isOk() && lastInfo.isDirectory();
        if (!intoDir && args.size() - i > 2) {
                ctx.eprintf("ln: %s: Not a directory\n", args[args.size() - 1]);
                return Shell::ExitFailure;
        }
        int status = Shell::ExitSuccess;
        for (size_t t = i; t + 1 < args.size(); ++t) {
                const StringView target = args[t];
                String           name;
                Error            err = intoDir ? join(last, baseName(target), name) : name.assign(last);
                if (err.isOk() && force) {
                        auto [old, oldErr] = Vfs::lstat(name.view(), vfs);
                        if (oldErr.isOk() && !old.isDirectory()) err = Vfs::remove(name.view(), vfs);
                }
                if (err.isOk()) err = Vfs::symlink(target, name.view(), vfs);
                if (err.isError()) {
                        fail(ctx, name.view(), err);
                        status = Shell::ExitFailure;
                }
        }
        return status;
}

int FileCommands::hexdump(CommandContext &ctx) {
        const Args   args = ctx.args();
        unsigned     flags = 0;
        const size_t first = parseOptions(args, "C", flags);
        if (first == 0 || args.size() - first > 1) return ctx.usage();
        CommandContext::Input input;
        const char           *name = first < args.size() ? args[first] : "-";
        Error                 err = ctx.openInput(name, input);
        if (err.isError()) return fail(ctx, name, err);

        uint8_t  line[16];
        uint8_t  previous[16];
        size_t   filled = 0;
        uint64_t offset = 0;
        bool     havePrevious = false;
        bool     squeezing = false;
        uint8_t  buf[512];
        for (;;) {
                if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                auto [n, readErr] = input.read(WritableByteSpan(buf));
                if (readErr == Error::EndOfFile) break;
                if (readErr.isError()) return fail(ctx, name, readErr);
                for (size_t i = 0; i < n; ++i) {
                        line[filled++] = buf[i];
                        if (filled < 16) continue;
                        // As hexdump: a run of identical lines shows as one "*".
                        if (havePrevious && std::memcmp(line, previous, 16) == 0) {
                                if (!squeezing) ctx.print("*\n");
                                squeezing = true;
                        } else {
                                hexLine(ctx, offset, line, 16);
                                squeezing = false;
                        }
                        std::memcpy(previous, line, 16);
                        havePrevious = true;
                        offset += 16;
                        filled = 0;
                }
        }
        if (filled > 0) hexLine(ctx, offset, line, filled);
        offset += filled;
        if (offset > 0) ctx.printf("%08llx\n", static_cast<unsigned long long>(offset));
        return Shell::ExitSuccess;
}

BMDS_SHELL_COMMAND(cd, FileCommands::cd, "[dir | -]", "Change the current directory ($HOME without a dir)")
BMDS_SHELL_COMMAND(cp, FileCommands::cp, "[-r] source... target", "Copy files (-r: directories too)")
BMDS_SHELL_COMMAND(hexdump, FileCommands::hexdump, "[-C] [file]", "Show a file (or standard input) in hex and ASCII")
BMDS_SHELL_COMMAND(ln, FileCommands::ln, "-s [-f] target name | -s [-f] target... dir",
                   "Make symbolic links (emulated on FAT, as Cygwin does)")
BMDS_SHELL_COMMAND(ls, FileCommands::ls, "[-la1] [path]...",
                   "List directories (-l: long, -a: hidden files too, -1: one per line)")
BMDS_SHELL_COMMAND(mkdir, FileCommands::mkdir, "[-p] dir...", "Make directories (-p: parents too, no error if there)")
BMDS_SHELL_COMMAND(mv, FileCommands::mv, "source... target", "Move or rename files and directories")
BMDS_SHELL_COMMAND(pwd, FileCommands::pwd, "", "Print the current directory")
BMDS_SHELL_COMMAND(rm, FileCommands::rm, "[-rf] path...", "Remove files (-r: directories too, -f: ignore missing)")
BMDS_SHELL_COMMAND(rmdir, FileCommands::rmdir, "dir...", "Remove empty directories")
BMDS_SHELL_COMMAND(touch, FileCommands::touch, "file...", "Create files, or set their time to now")

BMDS_NAMESPACE_END
