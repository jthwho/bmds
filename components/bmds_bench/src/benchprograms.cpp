/**
 * @file      benchprograms.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs' benchmarks (devplan/bench.md §4f): descriptors from `bench/paths`, and a program run
 * with its standard output on a pipe.
 */

#include "benchprograms.h"

#include <bmds/executor.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/pipe.h>
#include <bmds/programlauncher.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/task.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(benchPaths, "bench/paths", Config::Type::Paths, "/rom/share/bench;/sd0/p0/share/bench",
                     "Where 'bench' finds programs' benchmarks: their *.bench descriptors", 0, 0, nullptr, nullptr)

        constexpr int    Sigint = 2;
        constexpr size_t MaxLine = 256;

        Error readFile(StringView path, const Vfs::Context &context, String &out) {
                auto [file, err] = Vfs::open(path, Vfs::Read, context);
                if (err.isError()) return err;
                char buf[256];
                for (;;) {
                        auto [n, rerr] = file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                    WaitForever);
                        if (rerr == Error::EndOfFile) return Error::Ok;
                        if (rerr.isError()) return rerr;
                        if (out.size() + n > BenchPrograms::MaxDescriptorBytes) return Error::Full;
                        if (Error aerr = out.append(StringView(buf, n)); aerr.isError()) return aerr;
                }
        }

        void findIn(CommandContext &ctx, StringView                                       folder,
                    void (*found)(void *arg, const BenchText::ProgramBench &bench), void *arg) {
                const Vfs::Context context = ctx.vfs();
                auto [dir, err] = Vfs::openDirectory(folder, context);
                if (err.isError()) return; // not there: nothing installed
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr.isError()) return;
                        const StringView name = info.name.view();
                        if (info.isDirectory() || !name.endsWith(".bench")) continue;
                        String path(MemSpace::Psram), text(MemSpace::Psram);
                        Error  e = path.assign(folder);
                        if (e.isOk() && !folder.endsWith('/')) e = path.append('/');
                        if (e.isOk()) e = path.append(name);
                        if (e.isOk()) e = readFile(path.view(), context, text);
                        if (e.isError()) {
                                ctx.eprintf("bench: %s: %s\n", path.cStr(), e.desc());
                                continue;
                        }
                        if (BenchText::parseDescriptor(text.view(), found, arg) == 0)
                                ctx.eprintf("bench: %s: no benchmark in it\n", path.cStr());
                }
        }

} // namespace

void BenchPrograms::find(CommandContext &ctx, void (*found)(void *arg, const BenchText::ProgramBench &bench),
                         void           *arg) {
        String paths(MemSpace::Psram);
        if (SysConf::get(bmdsConf_benchPaths.key, paths).isError()) return;
        StringView rest = paths.view();
        while (!rest.isEmpty()) {
                const size_t     semi = rest.indexOf(';');
                const StringView folder = rest.left(semi).trimmed();
                if (!folder.isEmpty()) findIn(ctx, folder, found, arg);
                rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
        }
        return;
}

Error BenchPrograms::run(CommandContext &ctx, Span<const StringView> words, void (*line)(void *arg, StringView text),
                         void *arg, Ending &ending) {
        ProgramLauncher *launcher = ctx.shell().launcher();
        ProcessControl  *process = ctx.shell().process();
        if (launcher == nullptr || process == nullptr || words.size() == 0) return Error::NotSupported;
        // The program: a path as given, else found on $PATH as the shell finds it.
        const Variables &vars = ctx.executor().state().vars;
        String           path(MemSpace::Psram);
        if (words[0].contains('/')) {
                if (Error err = path.assign(words[0]); err.isError()) return err;
        } else {
                const String *search = vars.value("PATH");
                if (Error err = Vfs::findProgram(words[0], search != nullptr ? search->view() : StringView(), ctx.vfs(),
                                                 path);
                    err.isError())
                        return err;
        }
        // The shell's exported variables, as a program it starts gets them.
        List<String>     env(MemSpace::Psram);
        List<StringView> envViews(MemSpace::Psram);
        for (const Variables::Entry &e : vars.entries()) {
                if ((e.flags & Variables::Exported) == 0 || (e.flags & Variables::IsSet) == 0) continue;
                String text(MemSpace::Psram);
                Error  err = text.append(e.name);
                if (err.isOk()) err = text.append('=');
                if (err.isOk()) err = text.append(e.value);
                if (err.isOk()) err = env.append(std::move(text));
                if (err.isError()) return err;
        }
        if (envViews.reserve(env.size()).isError()) return Error::NoMem;
        for (const String &e : env) (void)envViews.append(e.view());
        // Its standard output on a pipe read here; standard input /dev/null; its errors as ours.
        auto [ends, perr] = Pipe::create();
        if (perr.isError()) return perr;
        FdTable fds = ctx.fds();
        (void)fds.set(FdTable::Stdout, std::move(ends.write));
        auto [null, nerr] = Vfs::open("/dev/null", Vfs::Read, ctx.vfs());
        if (nerr.isOk()) (void)fds.set(FdTable::Stdin, std::move(null));
        ProgramLauncher::Request request;
        request.path = path.view();
        request.args = words;
        request.environment = Span<const StringView>(envViews.data(), envViews.size());
        request.fds = &fds;
        request.cwd = ctx.cwd();
        request.console = &ctx.shell().console();
        request.interrupt = &ctx.shell().interrupt();
        request.waited = true;   // our child, waited for below; a group of its own, not the terminal's
        request.group = nullptr; // foreground: Ctrl-C is the shell's, which tells the program
        int8_t core = -1;
        request.core = &core;
        auto [pid, serr] = launcher->start(request);
        ending.core = core;
        fds.closeAll(); // the program has its own: the pipe ends when it does
        if (serr.isError()) return serr;
        char   buf[256];
        char   text[MaxLine + 1];
        size_t used = 0;
        bool   signalled = false;
        for (;;) {
                if (!signalled && ctx.isInterrupted()) {
                        (void)launcher->sendSignal(pid, Sigint);
                        signalled = true;
                }
                auto [n, rerr] = ends.read->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), 100);
                if (rerr == Error::EndOfFile) break;
                if (rerr == Error::Interrupted) {
                        Task::sleep(10); // Ctrl-C: the program told above; its output still read
                        continue;
                }
                if (rerr.isError() && rerr != Error::Timeout && rerr != Error::WouldBlock) break;
                for (size_t i = 0; i < n; ++i) {
                        if (buf[i] == '\n' || used == MaxLine) {
                                line(arg, StringView(text, used));
                                used = 0;
                                if (buf[i] == '\n') continue;
                        }
                        text[used++] = buf[i];
                }
        }
        if (used > 0) line(arg, StringView(text, used));
        int32_t status = 0;
        for (;;) {
                const Result<int32_t> r = process->waitChild(pid, ProcessControl::WaitBlock, status);
                if (r.isOk()) break;
                if (r.error() == Error::Interrupted && !process->isEnding()) {
                        Task::sleep(10); // Ctrl-C raised: the program is ending on its SIGINT
                        continue;
                }
                process->disownChild(pid);
                return r.error();
        }
        ending.exited = (status & 0x7f) == 0;
        ending.status = ending.exited ? (status >> 8) & 0xff : status & 0x7f;
        return Error::Ok;
}

BMDS_NAMESPACE_END
