/**
 * @file      transfercommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * rz and sz: YMODEM over the console the shell runs on. While a transfer
 * runs the console is in binary mode (no Ctrl-C, no newline translation, no
 * color rewriting, no log lines), so nothing can be printed; the results
 * are reported afterwards.
 */

#include <cstring>
#include <bmds/clock.h>
#include <bmds/console.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/transfercommands.h>
#include <bmds/vfs.h>
#include <bmds/ymodem.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // Files are read and written in pieces this big (whole sectors go
        // straight between the card and the buffer), not a block at a time.
        constexpr size_t BufferBytes = 32 * 1024;

        // Output still queued for the terminal before and after a transfer.
        constexpr unsigned int FlushTimeoutMs = 1000;

        // After a transfer: the line is left this long without input first.
        constexpr unsigned int QuietMs = 250;

        // The console the shell runs on, if the command's stdin and stdout are
        // that console (the serial line can't be a pipe or a file).
        Console *terminalOf(CommandContext &ctx) {
                Console    &console = ctx.shell().console();
                FileHandle *in = ctx.fds().handle(FdTable::Stdin);
                FileHandle *out = ctx.fds().handle(FdTable::Stdout);
                if (in == nullptr || out == nullptr || &in->stream() != &console || &out->stream() != &console)
                        return nullptr;
                return &console;
        }

        // The console in binary mode for the life of the object. Input already
        // waiting is kept: it may be the other end's first `C` (both ends
        // skip anything else).
        class BinaryMode {
                public:
                        explicit BinaryMode(Console &console) : _console(console), _saved(console.mode()) {
                                (void)console.flush(FlushTimeoutMs);
                                console.setMode(Console::TermMode::binary());
                        }
                        ~BinaryMode() {
                                (void)_console.flush(FlushTimeoutMs);
                                // Wait for the line to go quiet: what the other end still
                                // sends (CANs, retries) isn't for the shell, and the program
                                // there must have exited before anything is printed, or it
                                // swallows the message.
                                uint8_t buf[64];
                                for (;;) {
                                        auto [n, err] = _console.read(WritableByteSpan(buf), QuietMs);
                                        if (err.isError() || n == 0) break;
                                }
                                _console.setMode(_saved);
                        }
                        BinaryMode(const BinaryMode &) = delete;
                        BinaryMode &operator=(const BinaryMode &) = delete;

                private:
                        Console          &_console;
                        Console::TermMode _saved;
        };

        // A buffer in DMA-capable PSRAM.
        class Buffer {
                public:
                        Buffer() : _data(static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, BufferBytes))) {}
                        ~Buffer() { Memory::free(_data); }
                        Buffer(const Buffer &) = delete;
                        Buffer  &operator=(const Buffer &) = delete;
                        uint8_t *data() const { return _data; }

                private:
                        uint8_t *_data;
        };

        // What a failed transfer reports.
        const char *transferError(Error err) {
                switch (err.code()) {
                        case Error::Cancelled: return "cancelled";
                        case Error::Timeout: return "timed out: no answer from the other end";
                        case Error::CorruptData: return "too many errors on the line";
                        default: return CommandContext::errorText(err);
                }
        }

        // Elapsed time and rate, for the summary line.
        void printSummary(CommandContext &ctx, const char *verb, const Ymodem::Stats &stats, int64_t startNs) {
                const int64_t  ms = (Clock::monotonicNs() - startNs) / 1000000;
                const uint64_t rate = ms > 0 ? stats.bytes * 1000 / static_cast<uint64_t>(ms) / 1024 : 0;
                ctx.printf("%s: %lu file%s, %llu bytes in %lld.%01lld s (%llu KB/s)", verb,
                           static_cast<unsigned long>(stats.files), stats.files == 1 ? "" : "s",
                           static_cast<unsigned long long>(stats.bytes), static_cast<long long>(ms / 1000),
                           static_cast<long long>(ms % 1000 / 100), static_cast<unsigned long long>(rate));
                if (stats.retries > 0) ctx.printf(", %u retries", stats.retries);
                ctx.print("\n");
                return;
        }

        // --- rz ---------------------------------------------------------------

        class Receiver final : public Ymodem::ReceiveHandler {
                public:
                        Receiver(CommandContext &ctx, bool overwrite) : _ctx(ctx), _overwrite(overwrite) {}

                        Error begin(const Ymodem::FileHeader &h) override {
                                // Only the last component of what the sender calls it.
                                StringView   name = h.name;
                                const size_t slash = name.lastIndexOf('/');
                                if (slash != StringView::NPos) name = name.mid(slash + 1);
                                Error err = _path.assign(name);
                                if (err.isError()) return err;
                                if (name.isEmpty() || name == "." || name == "..") return Error::Invalid;
                                uint8_t flags = Vfs::Write | Vfs::Create | Vfs::Truncate;
                                if (!_overwrite) flags |= Vfs::Exclusive;
                                auto [file, oerr] = Vfs::open(name, flags, _ctx.vfs());
                                if (oerr.isError()) return oerr;
                                _file = std::move(file);
                                _modified = h.modified;
                                _fill = 0;
                                _size = 0;
                                return Error::Ok;
                        }

                        Error data(ByteSpan bytes) override {
                                while (!bytes.isEmpty()) {
                                        const size_t n =
                                                bytes.size() < BufferBytes - _fill ? bytes.size() : BufferBytes - _fill;
                                        std::memcpy(_buffer.data() + _fill, bytes.data(), n);
                                        _fill += n;
                                        _size += n;
                                        bytes = bytes.mid(n);
                                        if (_fill == BufferBytes) {
                                                Error err = flush();
                                                if (err.isError()) return err;
                                        }
                                }
                                return Error::Ok;
                        }

                        Error end() override {
                                Error err = flush();
                                if (err.isOk()) err = _file->stream().flush(WaitForever);
                                _file = FileHandle::Ptr();
                                if (err.isError()) return err;
                                if (_modified.ns > 0) (void)Vfs::setModified(_path, _modified, _ctx.vfs());
                                // Reported after the transfer (the line carries data now).
                                (void)_report.appendf("rz: %s: %llu bytes\n", _path.cStr(),
                                                      static_cast<unsigned long long>(_size));
                                return Error::Ok;
                        }

                        void abort() override {
                                // An incomplete file is no use: remove it.
                                if (_file) {
                                        _file = FileHandle::Ptr();
                                        (void)Vfs::remove(_path, _ctx.vfs());
                                }
                                return;
                        }

                        bool          hasBuffer() const { return _buffer.data() != nullptr; }
                        const String &path() const { return _path; }
                        const String &report() const { return _report; }

                private:
                        Error flush() {
                                if (_fill == 0) return Error::Ok;
                                Error err = _file->write(ByteSpan(_buffer.data(), _fill), WaitForever).error();
                                _fill = 0;
                                return err;
                        }

                        CommandContext &_ctx;
                        bool            _overwrite;
                        Buffer          _buffer;
                        FileHandle::Ptr _file;
                        String          _path;
                        String          _report;
                        Timestamp       _modified;
                        size_t          _fill = 0;
                        uint64_t        _size = 0;
        };

        // --- sz ---------------------------------------------------------------

        class Sender final : public Ymodem::SendSource {
                public:
                        Sender(CommandContext &ctx, Args paths) : _ctx(ctx), _paths(paths) {}

                        Result<bool> next(Ymodem::FileHeader &h) override {
                                _file = FileHandle::Ptr();
                                if (_index == _paths.size()) return makeResult(false);
                                const char            *path = _paths[_index++];
                                const Result<FileInfo> info = Vfs::stat(path, _ctx.vfs());
                                auto [file, err] = Vfs::open(path, Vfs::Read, _ctx.vfs());
                                if (err.isOk()) err = info.error();
                                if (err.isError()) return makeError<bool>(err);
                                _file = std::move(file);
                                _pos = 0;
                                _fill = 0;
                                const StringView p = path;
                                const size_t     slash = p.lastIndexOf('/');
                                err = h.name.assign(slash == StringView::NPos ? p : p.mid(slash + 1));
                                h.size = info.value().size;
                                h.hasSize = true;
                                h.modified = info.value().modified;
                                return Result<bool>(true, err);
                        }

                        Result<size_t> read(WritableByteSpan buf) override {
                                if (_pos == _fill) {
                                        auto [n, err] =
                                                _file->read(WritableByteSpan(_buffer.data(), BufferBytes), WaitForever);
                                        if (err == Error::EndOfFile) return makeResult(size_t(0));
                                        if (err.isError()) return makeError<size_t>(err);
                                        _pos = 0;
                                        _fill = n;
                                }
                                const size_t n = buf.size() < _fill - _pos ? buf.size() : _fill - _pos;
                                std::memcpy(buf.data(), _buffer.data() + _pos, n);
                                _pos += n;
                                return makeResult(n);
                        }

                        bool hasBuffer() const { return _buffer.data() != nullptr; }

                private:
                        CommandContext &_ctx;
                        Args            _paths;
                        size_t          _index = 0;
                        Buffer          _buffer;
                        FileHandle::Ptr _file;
                        size_t          _pos = 0;
                        size_t          _fill = 0;
        };

} // namespace

int TransferCommands::rz(CommandContext &ctx) {
        const Args args = ctx.args();
        bool       overwrite = false;
        if (args.size() == 2 && StringView(args[1]) == "-y") {
                overwrite = true;
        } else if (args.size() != 1) {
                return ctx.usage();
        }
        Console *console = terminalOf(ctx);
        if (console == nullptr) {
                ctx.eprint("rz: the serial line must be the terminal (no redirections or pipes)\n");
                return Shell::ExitFailure;
        }
        Receiver receiver(ctx, overwrite);
        if (!receiver.hasBuffer()) {
                ctx.eprint("rz: out of memory\n");
                return Shell::ExitFailure;
        }
        // No capital C in this: a sender already running would take it for the
        // receiver's request to start.
        ctx.eprint("rz: waiting for files (send them with sz --ymodem); ctrl-c to cancel\n");
        Ymodem::Stats stats;
        const int64_t start = Clock::monotonicNs();
        Error         err;
        {
                BinaryMode binary(*console);
                err = Ymodem::receive(*console, receiver, stats, true);
        }
        ctx.print(receiver.report());
        if (err.isError()) {
                if (err == Error::Exists) {
                        ctx.eprintf("rz: %s: File exists (rz -y replaces files)\n", receiver.path().cStr());
                } else if (err != Error::Cancelled && err != Error::Timeout && err != Error::CorruptData &&
                           !receiver.path().isEmpty()) {
                        ctx.eprintf("rz: %s: %s\n", receiver.path().cStr(), transferError(err));
                } else {
                        ctx.eprintf("rz: %s\n", transferError(err));
                }
        }
        if (err.isOk() || stats.files > 0) printSummary(ctx, "rz", stats, start);
        return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
}

int TransferCommands::sz(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() < 2) return ctx.usage();
        Console *console = terminalOf(ctx);
        if (console == nullptr) {
                ctx.eprint("sz: the serial line must be the terminal (no redirections or pipes)\n");
                return Shell::ExitFailure;
        }
        // Check the files first: once the transfer runs nothing can be said.
        for (const char *path : args.mid(1)) {
                const Result<FileInfo> info = Vfs::stat(path, ctx.vfs());
                Error                  err = info.error();
                if (err.isOk() && info.value().type != FileType::File)
                        err = info.value().isDirectory() ? Error::IsDir : Error::NotSupported;
                if (err.isError()) {
                        ctx.eprintf("sz: %s: %s\n", path, CommandContext::errorText(err));
                        return Shell::ExitFailure;
                }
        }
        Sender sender(ctx, args.mid(1));
        if (!sender.hasBuffer()) {
                ctx.eprint("sz: out of memory\n");
                return Shell::ExitFailure;
        }
        ctx.eprint("sz: sending (receive them with rz --ymodem); ctrl-c to cancel\n");
        Ymodem::Stats stats;
        const int64_t start = Clock::monotonicNs();
        Error         err;
        {
                BinaryMode binary(*console);
                err = Ymodem::send(*console, sender, stats, true);
        }
        if (err.isError()) ctx.eprintf("sz: %s\n", transferError(err));
        if (err.isOk() || stats.files > 0) printSummary(ctx, "sz", stats, start);
        return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
}

BMDS_SHELL_COMMAND(rz, TransferCommands::rz, "[-y]",
                   "Receive files into the current directory over the console (YMODEM; -y replaces files)")
BMDS_SHELL_COMMAND(sz, TransferCommands::sz, "file...", "Send files over the console (YMODEM)")

BMDS_NAMESPACE_END
