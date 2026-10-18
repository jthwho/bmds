/**
 * @file      fileworker.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A thread running a file manager's long operations (devplan/filebrowser.md §4j, FB13).
 */

#include <new>
#include <bmds/clock.h>
#include <bmds/fileworker.h>
#include <bmds/memory.h>
#include <bmds/trash.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        const Vfs::Context Root{"/", nullptr};

        // The next line of @p rest, @p rest moved past it.
        StringView nextLine(StringView &rest) {
                const size_t     end = rest.indexOf('\n');
                const StringView line = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                return line;
        }

        // Counting: only stopped, nothing told (the job tells its own failures).
        class Counting final : public FileOps::Progress {
                public:
                        explicit Counting(std::atomic<bool> &cancelled) : _cancelled(cancelled) {}
                        bool isCancelled() override { return _cancelled.load(std::memory_order_acquire); }

                private:
                        std::atomic<bool> &_cancelled;
        };

        // @p path's name in /trash (its path there, or the name alone).
        StringView trashName(StringView path) {
                const StringView t = Vfs::TrashPath;
                if (path.size() > t.size() + 1 && path.startsWith(t) && path[t.size()] == '/')
                        return path.mid(t.size() + 1);
                return path;
        }

        // Folder @p from's contents moved into folder @p into (a name taken there asked as a move's),
        // then @p from removed if all of it went: a restored folder merged into the one in its place.
        Error mergeInto(StringView from, StringView into, FileOps::Progress &progress) {
                List<String> names(MemSpace::Psram);
                {
                        auto [dir, err] = Vfs::openDirectory(from, Root);
                        if (err.isError()) return err;
                        for (;;) {
                                auto [info, nerr] = dir.next();
                                if (nerr == Error::EndOfFile) break;
                                if (nerr.isError()) return nerr;
                                err = names.append(std::move(info.name));
                                if (err.isError()) return err;
                        }
                }
                String child(MemSpace::Psram);
                for (const String &name : names) {
                        if (child.assign(from).isError() || child.append('/').isError() ||
                            child.append(name.view()).isError())
                                return Error::NoMem;
                        const Error err = FileOps::move(child.view(), into, Root, progress);
                        if (err == Error::Interrupted) return err;
                        if (err.isError()) progress.failed(child.view(), err);
                }
                const Error err = Vfs::rmdir(from, Root);
                return err == Error::NotEmpty ? Error(Error::Ok) : err; // something skipped stays in the trash
        }

        // A /trash entry put back where it was (devplan/filebrowser.md §4k, FB24); its place taken
        // asked: Replace (a folder merged), Keep Both, Skip or Cancel.
        Error restoreOne(StringView path, FileOps::Progress &progress) {
                const StringView name = trashName(path);
                Trash::Entry     e;
                Error            err = Trash::find(name, e);
                if (err.isError()) return err;
                progress.began(path, 0);
                err = Trash::restore(name, Trash::Conflict::Refuse, nullptr);
                if (err != Error::Exists) return err;
                auto [there, serr] = Vfs::lstat(e.path.view(), Root);
                const bool folder = serr.isOk() && there.isDirectory();
                switch (progress.conflict(path, e.path.view(), folder)) {
                        case FileOps::Resolution::Replace: {
                                if (!folder || !e.isDirectory)
                                        return Trash::restore(name, Trash::Conflict::Replace, nullptr);
                                String from(MemSpace::Psram);
                                if (from.assign(Vfs::TrashPath).isError() || from.append('/').isError() ||
                                    from.append(name).isError())
                                        return Error::NoMem;
                                return mergeInto(from.view(), e.path.view(), progress);
                        }
                        case FileOps::Resolution::KeepBoth:
                                return Trash::restore(name, Trash::Conflict::KeepBoth, nullptr);
                        case FileOps::Resolution::Skip: return Error::Ok;
                        default: return Error::Interrupted;
                }
        }

} // namespace

// A job's view of FileOps: the status kept, questions asked of the owner.
class FileWorker::Progress final : public FileOps::Progress {
        public:
                Progress(FileWorker &worker, Job &job) : _w(worker), _job(job) {}

                bool isCancelled() override { return _w._cancelled.load(std::memory_order_acquire); }

                void began(StringView path, uint64_t bytes) override {
                        {
                                Mutex::Locker lock(_w._lock, WaitForever);
                                ++_w._status.items;
                                if (_job.kind == Kind::Measure) _w._status.bytes += bytes; // counted, not copied
                                (void)_w._status.item.assign(path);
                        }
                        _w.tell(false);
                        return;
                }

                void advanced(uint64_t bytes) override {
                        {
                                Mutex::Locker lock(_w._lock, WaitForever);
                                _w._status.bytes += bytes;
                        }
                        _w.tell(false);
                        return;
                }

                void failed(StringView path, Error err) override {
                        ++failures;
                        if (first.isOk()) {
                                first = err;
                                (void)firstPath.assign(path);
                        }
                        return;
                }

                FileOps::Resolution conflict(StringView source, StringView target, bool isDirectory) override {
                        if (_job.answered) return _job.resolution;
                        {
                                Mutex::Locker lock(_w._lock, WaitForever);
                                if (isCancelled()) return FileOps::Resolution::Cancel;
                                _w._status.asking = true;
                                _w._answerGiven = false;
                                (void)_w._status.askSource.assign(source);
                                (void)_w._status.askTarget.assign(target);
                                _w._status.askIsDirectory = isDirectory;
                        }
                        _w.tell(true);
                        (void)_w._answered.acquire(WaitForever); // answer() or cancel()
                        FileOps::Resolution r;
                        bool                forAll;
                        {
                                Mutex::Locker lock(_w._lock, WaitForever);
                                _w._status.asking = false;
                                r = isCancelled() ? FileOps::Resolution::Cancel : _w._answer;
                                forAll = _w._answerForAll;
                        }
                        if (forAll && r != FileOps::Resolution::Cancel) {
                                _job.answered = true;
                                _job.resolution = r;
                        }
                        return r;
                }

                size_t failures = 0;
                Error  first;
                String firstPath{MemSpace::Psram};

        private:
                FileWorker &_w;
                Job        &_job;
};

FileWorker::~FileWorker() {
        {
                Mutex::Locker lock(_lock, WaitForever);
                for (Job *job : _queue) freeJob(job);
                _queue.clear();
                _cancelled.store(true, std::memory_order_release);
                if (_status.asking && !_answerGiven) {
                        _answerGiven = true;
                        _answered.release();
                }
        }
        if (_thread.isStarted()) _thread.join();
        for (Done *d : _done) {
                d->~Done();
                Memory::free(d);
        }
        _done.clear();
}

void FileWorker::freeJob(Job *job) {
        job->~Job();
        Memory::free(job);
        return;
}

Result<uint32_t> FileWorker::add(Kind kind, StringView paths, StringView target) {
        if (paths.trimmed().isEmpty()) return makeError<uint32_t>(Error::Invalid);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Job));
        if (mem == nullptr) return makeError<uint32_t>(Error::NoMem);
        Job  *job = ::new (mem) Job();
        Error err = job->paths.assign(paths);
        if (err.isOk()) err = job->target.assign(target);
        job->kind = kind;
        Mutex::Locker lock(_lock, WaitForever);
        if (err.isOk()) err = _queue.append(job);
        if (err.isError()) {
                freeJob(job);
                return makeError<uint32_t>(err);
        }
        job->id = _nextId++;
        if (_nextId == 0) _nextId = 1;
        // A thread that left its loop takes no lock again: joined here, then one started.
        if (_ended) {
                _thread.join();
                _ended = false;
        }
        if (!_thread.isStarted()) {
                Thread::Config config;
                config.name = "fileworker";
                config.stackBytes = StackBytes;
                config.stackSpace = MemSpace::Psram;
                config.priority = 1; // below the loop: the desktop stays responsive
                err = _thread.start(config, threadMain, this);
                if (err.isError()) {
                        _queue.removeAt(_queue.size() - 1);
                        freeJob(job);
                        return makeError<uint32_t>(err);
                }
        }
        if (_current != nullptr) _status.waiting = _queue.size();
        return makeResult(job->id);
}

void FileWorker::cancel(uint32_t job) {
        Mutex::Locker lock(_lock, WaitForever);
        for (size_t i = 0; i < _queue.size(); ++i) {
                if (_queue[i]->id != job) continue;
                freeJob(_queue[i]);
                _queue.removeAt(i);
                if (_current != nullptr) _status.waiting = _queue.size();
                return;
        }
        if (_current == nullptr || _current->id != job) return;
        _cancelled.store(true, std::memory_order_release);
        if (_status.asking && !_answerGiven) {
                _answerGiven = true;
                _answered.release();
        }
        return;
}

void FileWorker::answer(FileOps::Resolution resolution, bool forAll) {
        Mutex::Locker lock(_lock, WaitForever);
        if (!_status.asking || _answerGiven) return;
        _answer = resolution;
        _answerForAll = forAll;
        _answerGiven = true;
        _answered.release();
        return;
}

void FileWorker::status(Status &out) {
        Mutex::Locker lock(_lock, WaitForever);
        out.job = _status.job;
        out.kind = _status.kind;
        out.counting = _status.counting;
        out.items = _status.items;
        out.totalItems = _status.totalItems;
        out.bytes = _status.bytes;
        out.totalBytes = _status.totalBytes;
        out.waiting = _status.waiting;
        out.asking = _status.asking && !_answerGiven;
        out.askIsDirectory = _status.askIsDirectory;
        // Short of memory, a name shown empty: nothing else depends on it.
        (void)out.item.assign(_status.item.view());
        (void)out.target.assign(_status.target.view());
        (void)out.askSource.assign(_status.askSource.view());
        (void)out.askTarget.assign(_status.askTarget.view());
        return;
}

bool FileWorker::takeDone(Done &out) {
        Done *d = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                if (_done.isEmpty()) return false;
                d = _done[0];
                _done.removeAt(0);
        }
        out.job = d->job;
        out.kind = d->kind;
        out.err = d->err;
        out.failures = d->failures;
        out.failed = std::move(d->failed);
        out.totals = d->totals;
        d->~Done();
        Memory::free(d);
        return true;
}

bool FileWorker::isBusy() {
        Mutex::Locker lock(_lock, WaitForever);
        return _current != nullptr || !_queue.isEmpty();
}

void FileWorker::reap() {
        Mutex::Locker lock(_lock, WaitForever);
        if (!_ended) return;
        _thread.join();
        _ended = false;
        return;
}

void FileWorker::tell(bool now) {
        const int64_t t = Clock::monotonicNs();
        if (!now && t - _toldNs < int64_t(NotifyEveryMs) * 1000000) return;
        _toldNs = t;
        _notify(_notifyArg);
        return;
}

void FileWorker::threadMain(void *arg) {
        auto *self = static_cast<FileWorker *>(arg);
        for (;;) {
                Job *job = nullptr;
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        if (self->_queue.isEmpty()) {
                                self->_ended = true; // no lock taken after this: the owner joins
                                break;
                        }
                        job = self->_queue[0];
                        self->_queue.removeAt(0);
                        self->_current = job;
                        self->_cancelled.store(false, std::memory_order_release);
                        Status &s = self->_status;
                        s.job = job->id;
                        s.kind = job->kind;
                        s.counting = true;
                        s.items = s.totalItems = s.bytes = s.totalBytes = 0;
                        s.waiting = self->_queue.size();
                        s.item.clear();
                        (void)s.target.assign(job->kind == Kind::Copy || job->kind == Kind::Move ? job->target.view()
                                                                                                 : StringView());
                        s.asking = false;
                }
                self->tell(true);
                self->run(*job);
                {
                        Mutex::Locker lock(self->_lock, WaitForever);
                        self->_current = nullptr;
                        self->_status.job = 0;
                }
                freeJob(job);
                self->tell(true);
        }
        self->tell(true);
        return;
}

void FileWorker::run(Job &job) {
        Progress progress(*this, job);
        // The paths, and those counted: a move's on the target's filesystem are renames, an item each.
        List<StringView> paths(MemSpace::Psram), counted(MemSpace::Psram);
        uint64_t         renames = 0;
        uint32_t         targetDevice = 0;
        if (job.kind == Kind::Move) {
                auto [info, err] = Vfs::stat(job.target.view(), Root);
                if (err.isOk()) targetDevice = info.device;
        }
        Error      err;
        StringView rest = job.paths.view();
        while (err.isOk() && !rest.isEmpty()) {
                const StringView path = nextLine(rest).trimmed();
                if (path.isEmpty()) continue;
                err = paths.append(path);
                if (err.isError()) break;
                if (job.kind == Kind::Move && targetDevice != 0) {
                        auto [info, lerr] = Vfs::lstat(path, Root);
                        if (lerr.isOk() && info.device == targetDevice) {
                                ++renames;
                                continue;
                        }
                }
                err = counted.append(path);
        }
        Counting counting(_cancelled);
        const bool counts = job.kind != Kind::Restore && job.kind != Kind::Measure; // counted first
        auto [totals, merr] = err.isOk() && counts ? FileOps::measure(counted.span(), Root, counting)
                                                   : Result<FileOps::Totals>(FileOps::Totals(), err);
        if (job.kind == Kind::Restore) totals.items = paths.size(); // renames, an item each
        FileOps::Totals measured;
        if (job.kind == Kind::Measure && err.isOk()) {
                // Counting is the job: told as it goes.
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        _status.counting = false;
                }
                auto [m, e] = FileOps::measure(paths.span(), Root, progress);
                measured = m;
                if (e == Error::Interrupted) err = e;
        }
        {
                Mutex::Locker lock(_lock, WaitForever);
                _status.counting = false;
                _status.totalItems = totals.items + renames;
                _status.totalBytes = job.kind == Kind::Copy || job.kind == Kind::Move ? totals.bytes : 0;
        }
        tell(true);
        // Each path; one failing whole (not there, into itself) is told as an item failed.
        Error first = err;
        bool  stopped = false; // Cancel answered, or cancel()
        for (const StringView path : paths) {
                if (job.kind == Kind::Measure) break; // done above
                if (err.isError() || _cancelled.load(std::memory_order_acquire)) break;
                const size_t before = progress.failures;
                Error        e;
                switch (job.kind) {
                        case Kind::Copy: e = FileOps::copy(path, job.target.view(), Root, progress); break;
                        case Kind::Move: e = FileOps::move(path, job.target.view(), Root, progress); break;
                        case Kind::Remove: e = FileOps::remove(path, Root, progress); break;
                        case Kind::Restore: e = restoreOne(path, progress); break;
                        case Kind::Measure: break;
                }
                if (e == Error::Interrupted) {
                        stopped = true;
                        break;
                }
                if (e.isError() && progress.failures == before) progress.failed(path, e);
        }
        if (first.isOk()) first = progress.first;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Done));
        if (mem == nullptr) return; // the job ends untold: its windows follow the folders anyway
        Done *d = ::new (mem) Done();
        d->job = job.id;
        d->kind = job.kind;
        d->err = stopped || _cancelled.load(std::memory_order_acquire) ? Error(Error::Interrupted) : first;
        d->failures = progress.failures;
        d->failed = std::move(progress.firstPath);
        d->totals = measured;
        (void)merr; // counting cut short: the bar is only less exact
        Mutex::Locker lock(_lock, WaitForever);
        if (_done.append(d).isError()) {
                d->~Done();
                Memory::free(d);
        }
        return;
}

BMDS_NAMESPACE_END
