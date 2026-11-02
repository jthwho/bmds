/**
 * @file      pkgjob.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "pkgjob.h"

#include <bmds/fdtable.h>
#include <bmds/guiapplication.h>
#include <bmds/pipe.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t WorkerStackBytes = 8 * 1024;
        constexpr int    SigTerm = 15;
        // The longest line kept: past it a line is dropped (pkg's longest is a description).
        constexpr size_t MaxLineBytes = PkgRecord::MaxString + 1024;

} // namespace

PkgJob::PkgJob(GuiApplication &app) : _app(app), _taken(1, 0) {}

PkgJob::~PkgJob() {
        _stopping.store(true, std::memory_order_release);
        if (_pid > 0 && _launcher != nullptr) (void)_launcher->sendSignal(_pid, SigTerm);
        _taken.release(); // a hand-over waiting: not taken now
        if (_started) _worker.join();
        _app.cancelPosts(this);
}

void PkgJob::setHandlers(RecordHandler onRecord, FinishedHandler onFinished, void *arg) {
        _onRecord = onRecord;
        _onFinished = onFinished;
        _arg = arg;
        return;
}

Error PkgJob::start(const BuiltinProgram::Context &context, StringView args) {
        if (_running) return Error::InvalidState;
        if (context.launcher == nullptr || context.process == nullptr) return Error::NotSupported;
        if (_started) { // the last one's thread, ended
                _worker.join();
                _started = false;
        }
        String command(MemSpace::Psram);
        Error  err = command.assign("pkg --json ");
        if (err.isOk()) err = command.append(args);
        if (err.isError()) return err;
        auto [out, perr] = Pipe::create();
        if (perr.isError()) return perr;
        auto [null, nerr] = Vfs::open("/dev/null", Vfs::Read | Vfs::Write, Vfs::Context{});
        FdTable fds;
        if (nerr.isOk()) {
                (void)fds.set(FdTable::Stdin, null);
                (void)fds.set(FdTable::Stderr, null);
        }
        (void)fds.set(FdTable::Stdout, std::move(out.write));
        const StringView         argv[] = {"sh", "-c", command.view()};
        ProgramLauncher::Request request;
        request.path = "/bin/sh";
        request.args = Span<const StringView>(argv, 3);
        request.environment = context.environment;
        request.fds = &fds;
        request.cwd = "/";
        request.waited = true;
        auto [pid, serr] = context.launcher->start(request);
        if (serr.isError()) return serr;
        _launcher = context.launcher;
        _process = context.process;
        _pid = pid;
        _output = std::move(out.read);
        _status = 0;
        _stopping.store(false, std::memory_order_release);
        Thread::Config config;
        config.name = "pkgjob";
        config.stackBytes = WorkerStackBytes;
        config.stackSpace = MemSpace::Psram;
        err = _worker.start(config, workerMain, this);
        if (err.isError()) {
                (void)context.launcher->sendSignal(pid, SigTerm);
                int32_t status = 0;
                (void)context.process->waitChild(pid, ProcessControl::WaitBlock, status);
                _pid = -1;
                _output.reset();
                return err;
        }
        _started = true;
        _running = true;
        return Error::Ok;
}

// The worker: pkg's lines, each read into a record and handed to the loop; then its end.
void PkgJob::workerMain(void *arg) {
        auto  *self = static_cast<PkgJob *>(arg);
        String line(MemSpace::Psram);
        bool   tooLong = false;
        for (;;) {
                char buf[256];
                auto [n, err] = self->_output->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                    WaitForever);
                if (err.isError() || n == 0) break;
                for (size_t i = 0; i < n; ++i) {
                        if (buf[i] != '\n') {
                                if (line.size() < MaxLineBytes) {
                                        (void)line.append(buf[i]);
                                } else {
                                        tooLong = true;
                                }
                                continue;
                        }
                        // A line that isn't a record (or was cut) is passed over.
                        if (!tooLong && self->_record.parse(line.view()).isOk()) self->handOver();
                        line.clear();
                        tooLong = false;
                }
        }
        self->_output.reset();
        int32_t               status = 0;
        const Result<int32_t> waited = self->_process->waitChild(self->_pid, ProcessControl::WaitBlock, status);
        self->_status = waited.isOk() ? int(status) : -1;
        self->_pid = -1;
        if (!self->_stopping.load(std::memory_order_acquire)) (void)self->_app.post(finished, self);
        return;
}

void PkgJob::handOver() {
        if (_stopping.load(std::memory_order_acquire)) return;
        if (_app.post(deliver, this).isError()) return;
        (void)_taken.acquire(WaitForever);
        return;
}

void PkgJob::deliver(void *arg) {
        auto *self = static_cast<PkgJob *>(arg);
        if (self->_onRecord != nullptr) self->_onRecord(self->_arg, self->_record);
        self->_taken.release();
        return;
}

void PkgJob::finished(void *arg) {
        auto *self = static_cast<PkgJob *>(arg);
        self->_running = false;
        if (self->_onFinished != nullptr) self->_onFinished(self->_arg, self->_status);
        return;
}

BMDS_NAMESPACE_END
