/**
 * @file      ymodem.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * YMODEM: both ends against each other over pipes (clean and noisy lines),
 * cancelling, and rz/sz through the shell on a RAM disk.
 */

#include <doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <bmds/blockdevices.h>
#include <bmds/console.h>
#include <bmds/filecommands.h>
#include <bmds/pipe.h>
#include <bmds/ramblockdevice.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include <bmds/transfercommands.h>
#include <bmds/vfs.h>
#include <bmds/ymodem.h>

using bmds::Error;
namespace Ymodem = bmds::Ymodem;
namespace Vfs = bmds::Vfs;

namespace {

        // One end of a serial line: reads one pipe, writes the other.
        class LineEnd final : public bmds::Stream {
                public:
                        LineEnd(bmds::FileHandle::Ptr in, bmds::FileHandle::Ptr out)
                            : _in(std::move(in)), _out(std::move(out)) {}

                        bmds::Result<size_t> read(bmds::WritableByteSpan buf, unsigned int timeoutMs) override {
                                return _in->read(buf, timeoutMs);
                        }

                        bmds::Result<size_t> write(bmds::ByteSpan data, unsigned int timeoutMs) override {
                                // A noisy line flips a byte now and then.
                                if (noiseEvery > 0 && data.size() > 100 && ++_writes % noiseEvery == 0) {
                                        std::vector<uint8_t> copy(data.begin(), data.end());
                                        copy[copy.size() / 2] ^= 0x40;
                                        return _out->write(bmds::ByteSpan(copy.data(), copy.size()), timeoutMs);
                                }
                                return _out->write(data, timeoutMs);
                        }

                        void close() {
                                _in = bmds::FileHandle::Ptr();
                                _out = bmds::FileHandle::Ptr();
                        }

                        int noiseEvery = 0; // corrupt every n-th block written (0: never)

                private:
                        bmds::FileHandle::Ptr _in;
                        bmds::FileHandle::Ptr _out;
                        int                   _writes = 0;
        };

        // Two connected ends.
        struct Line {
                        Line() {
                                auto [ab, e1] = bmds::Pipe::create(8192);
                                auto [ba, e2] = bmds::Pipe::create(8192);
                                REQUIRE((e1.isOk() && e2.isOk()));
                                a = std::make_unique<LineEnd>(std::move(ba.read), std::move(ab.write));
                                b = std::make_unique<LineEnd>(std::move(ab.read), std::move(ba.write));
                        }
                        ~Line() {
                                a->close();
                                b->close();
                        }
                        std::unique_ptr<LineEnd> a;
                        std::unique_ptr<LineEnd> b;
        };

        struct MemFile {
                        std::string name;
                        std::string data;
                        int64_t     mtime = 0;
        };

        class MemSource final : public Ymodem::SendSource {
                public:
                        explicit MemSource(std::vector<MemFile> files) : _files(std::move(files)) {}

                        bmds::Result<bool> next(Ymodem::FileHeader &h) override {
                                if (_index == _files.size()) return bmds::makeResult(false);
                                _current = &_files[_index++];
                                _pos = 0;
                                (void)h.name.assign(bmds::StringView(_current->name.data(), _current->name.size()));
                                h.size = _current->data.size();
                                h.hasSize = true;
                                h.modified = bmds::Timestamp::fromSeconds(_current->mtime);
                                return bmds::makeResult(true);
                        }

                        bmds::Result<size_t> read(bmds::WritableByteSpan buf) override {
                                // Odd sizes, to exercise the sender's block filling.
                                size_t n = _current->data.size() - _pos;
                                if (n > 700) n = 700;
                                if (n > buf.size()) n = buf.size();
                                std::memcpy(buf.data(), _current->data.data() + _pos, n);
                                _pos += n;
                                return bmds::makeResult(n);
                        }

                private:
                        std::vector<MemFile> _files;
                        size_t               _index = 0;
                        const MemFile       *_current = nullptr;
                        size_t               _pos = 0;
        };

        class MemSink final : public Ymodem::ReceiveHandler {
                public:
                        Error begin(const Ymodem::FileHeader &h) override {
                                if (refuse) return Error::Exists;
                                files.push_back(
                                        MemFile{std::string(h.name.data(), h.name.size()), "", h.modified.seconds()});
                                return Error::Ok;
                        }
                        Error data(bmds::ByteSpan bytes) override {
                                files.back().data.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                                return Error::Ok;
                        }
                        Error end() override {
                                ++ended;
                                return Error::Ok;
                        }
                        void abort() override { ++aborted; }

                        std::vector<MemFile> files;
                        int                  ended = 0;
                        int                  aborted = 0;
                        bool                 refuse = false;
        };

        std::string pattern(size_t n, unsigned seed) {
                std::string s(n, '\0');
                for (size_t i = 0; i < n; ++i) {
                        seed = seed * 1103515245u + 12345u;
                        s[i] = static_cast<char>(seed >> 16);
                }
                return s;
        }

        std::vector<MemFile> sampleFiles() {
                return {
                        {"empty", "", 0},
                        {"one", "x", 1700000000},
                        {"exactly128", pattern(128, 1), 1700000001},
                        {"just over 128", pattern(129, 2), 1700000002},
                        {"exactly1024", pattern(1024, 3), 1700000003},
                        {"padding\x1a\x1a", std::string(1030, '\x1a'), 1700000004},
                        {std::string(200, 'n'), pattern(100 * 1024 + 7, 4), 1700000005},
                };
        }

        // Runs a whole batch both ways; returns (sender error, receiver error).
        std::pair<Error, Error> transfer(Line &line, MemSource &source, MemSink &sink, Ymodem::Stats &sent,
                                         Ymodem::Stats &received) {
                Error       sendErr;
                std::thread sender([&] { sendErr = Ymodem::send(*line.a, source, sent, false); });
                const Error recvErr = Ymodem::receive(*line.b, sink, received, false);
                sender.join();
                return {sendErr, recvErr};
        }

} // namespace

TEST_CASE("YMODEM: CRC-16/XMODEM") {
        const char *check = "123456789";
        CHECK(Ymodem::crc16(bmds::ByteSpan(reinterpret_cast<const uint8_t *>(check), 9)) == 0x31C3);
        CHECK(Ymodem::crc16(bmds::ByteSpan()) == 0);
}

TEST_CASE("YMODEM: a batch arrives whole, with sizes and times") {
        Line          line;
        MemSource     source(sampleFiles());
        MemSink       sink;
        Ymodem::Stats sent, received;
        const auto [sendErr, recvErr] = transfer(line, source, sink, sent, received);
        CHECK(sendErr == Error::Ok);
        CHECK(recvErr == Error::Ok);
        const std::vector<MemFile> want = sampleFiles();
        REQUIRE(sink.files.size() == want.size());
        for (size_t i = 0; i < want.size(); ++i) {
                CAPTURE(want[i].name);
                CHECK(sink.files[i].name == want[i].name);
                CHECK(sink.files[i].data == want[i].data);
                CHECK(sink.files[i].mtime == want[i].mtime);
        }
        CHECK(sink.ended == static_cast<int>(want.size()));
        CHECK(sent.files == want.size());
        CHECK(received.files == want.size());
        CHECK(received.bytes == sent.bytes);
        CHECK(received.retries == 0);
}

TEST_CASE("YMODEM: damaged blocks are sent again") {
        Line line;
        line.a->noiseEvery = 7; // sender to receiver
        MemSource     source({{"noisy", pattern(40 * 1024, 9), 1700000000}});
        MemSink       sink;
        Ymodem::Stats sent, received;
        const auto [sendErr, recvErr] = transfer(line, source, sink, sent, received);
        CHECK(sendErr == Error::Ok);
        CHECK(recvErr == Error::Ok);
        REQUIRE(sink.files.size() == 1);
        CHECK(sink.files[0].data == pattern(40 * 1024, 9));
        CHECK(sent.retries > 0);
        CHECK(received.retries == sent.retries);
}

TEST_CASE("YMODEM: cancelling") {
        SUBCASE("the receiver refuses a file") {
                Line          line;
                MemSource     source({{"f", "data", 0}});
                MemSink       sink;
                Ymodem::Stats sent, received;
                sink.refuse = true;
                const auto [sendErr, recvErr] = transfer(line, source, sink, sent, received);
                CHECK(recvErr == Error::Exists);
                CHECK(sendErr == Error::Cancelled);
        }
        SUBCASE("Ctrl-C before the first block") {
                Line          line;
                MemSink       sink;
                Ymodem::Stats received;
                const uint8_t ctrlC = 0x03;
                REQUIRE(line.a->write(bmds::ByteSpan(&ctrlC, 1), 1000).error().isOk());
                CHECK(Ymodem::receive(*line.b, sink, received, true) == Error::Cancelled);
        }
        SUBCASE("the other end sends CAN CAN") {
                Line          line;
                MemSource     source({{"f", "data", 0}});
                Ymodem::Stats sent;
                const uint8_t cans[] = {Ymodem::Can, Ymodem::Can};
                REQUIRE(line.b->write(bmds::ByteSpan(cans, 2), 1000).error().isOk());
                CHECK(Ymodem::send(*line.a, source, sent, false) == Error::Cancelled);
        }
}

namespace {

        // A console whose line is one end of a Line; the peer runs on a thread.
        class LineConsole final : public bmds::Console {
                public:
                        explicit LineConsole(LineEnd &end) : _end(end) {}
                        using Console::write;
                        const char          *name() const override { return "line"; }
                        uint32_t             capabilities() const override { return 0; }
                        bmds::Result<size_t> write(bmds::ByteSpan data, unsigned int timeoutMs) override {
                                return _end.write(data, timeoutMs);
                        }
                        bmds::Error flush(unsigned int) override { return bmds::Error(); }

                protected:
                        bmds::Result<size_t> readInput(bmds::WritableByteSpan buf, unsigned int timeoutMs) override {
                                return _end.read(buf, timeoutMs);
                        }
                        bmds::Result<size_t> tryReadInput(bmds::WritableByteSpan buf) override { return _end.read(buf, 1); }
                        bool                 hasInput() const override { return false; } // never polled

                private:
                        LineEnd &_end;
        };

        constexpr bmds::Command Commands[] = {
                {"cat", "", "", bmds::TextCommands::cat},
                {"cd", "", "", bmds::FileCommands::cd},
                {"rz", "[-y]", "", bmds::TransferCommands::rz},
                {"sz", "file...", "", bmds::TransferCommands::sz},
        };

        // Reads what the shell printed on the line after a transfer, until it is quiet.
        std::string drain(LineEnd &end) {
                std::string out;
                uint8_t     buf[256];
                for (;;) {
                        auto [n, err] = end.read(bmds::WritableByteSpan(buf), 200);
                        if (err.isError() || n == 0) return out;
                        out.append(reinterpret_cast<const char *>(buf), n);
                }
        }

        struct ShellOnLine {
                        ShellOnLine() : console(*line.b), disk(512) {
                                REQUIRE(disk.allocate(4 * 2048, bmds::MemSpace::Psram).isOk());
                                char name[bmds::BlockDevices::MaxName + 1];
                                REQUIRE(bmds::BlockDevices::addNumbered(disk, "ym", name).isOk());
                                REQUIRE(Vfs::format(disk, bmds::FsFormat::Auto).isOk());
                                REQUIRE(Vfs::automount(disk).value() == 1);
                                shell = std::make_unique<bmds::Shell>(
                                        console, bmds::Span<const bmds::Command>(Commands), 1 << 20);
                                REQUIRE(shell->init().isOk());
                                REQUIRE(shell->execute("cd /ym0/p0") == 0);
                        }
                        ~ShellOnLine() {
                                shell.reset();
                                Vfs::eject(disk);
                                bmds::BlockDevices::remove(disk);
                        }

                        Line                         line; // a: the PC, b: the shell's console
                        LineConsole                  console;
                        bmds::RamBlockDevice         disk;
                        std::unique_ptr<bmds::Shell> shell;
        };

        std::string readFile(const char *path) {
                auto [h, err] = Vfs::open(path, Vfs::Read, Vfs::Context{"/", nullptr});
                if (err.isError()) return "<missing>";
                std::string out;
                uint8_t     buf[4096];
                for (;;) {
                        auto [n, rerr] = h->read(bmds::WritableByteSpan(buf), bmds::WaitForever);
                        if (rerr.isError() || n == 0) return out;
                        out.append(reinterpret_cast<const char *>(buf), n);
                }
        }

} // namespace

TEST_CASE("YMODEM: rz and sz through the shell") {
        ShellOnLine       s;
        const std::string big = pattern(70 * 1024 + 3, 5);

        // rz: the PC sends two files; a path in the name is dropped.
        MemSource     source({{"dir/hello.txt", "hello\n", 1700000000}, {"big.bin", big, 1700000100}});
        Ymodem::Stats sent;
        Error         sendErr;
        std::thread   pc([&] { sendErr = Ymodem::send(*s.line.a, source, sent, false); });
        CHECK(s.shell->execute("rz") == 0);
        pc.join();
        CHECK(sendErr == Error::Ok);
        std::string out = drain(*s.line.a);
        CHECK(out.find("rz: hello.txt: 6 bytes\n") != std::string::npos);
        CHECK(out.find("rz: big.bin: 71683 bytes\n") != std::string::npos);
        CHECK(out.find("rz: 2 files, 71689 bytes in ") != std::string::npos);
        CHECK(readFile("/ym0/p0/hello.txt") == "hello\n");
        CHECK(readFile("/ym0/p0/big.bin") == big);
        CHECK(Vfs::stat("/ym0/p0/big.bin", Vfs::Context{}).value().modified.seconds() == 1700000100);

        // An existing file is refused without -y, and nothing is left half-written.
        MemSource again({{"hello.txt", "changed\n", 0}});
        pc = std::thread([&] { sendErr = Ymodem::send(*s.line.a, again, sent, false); });
        CHECK(s.shell->execute("rz") == 1);
        pc.join();
        CHECK(sendErr == Error::Cancelled);
        out = drain(*s.line.a);
        CHECK(out.find("rz: hello.txt: File exists (rz -y replaces files)\n") != std::string::npos);
        CHECK(readFile("/ym0/p0/hello.txt") == "hello\n");
        MemSource replace({{"hello.txt", "changed\n", 0}});
        pc = std::thread([&] { sendErr = Ymodem::send(*s.line.a, replace, sent, false); });
        CHECK(s.shell->execute("rz -y") == 0);
        pc.join();
        (void)drain(*s.line.a);
        CHECK(readFile("/ym0/p0/hello.txt") == "changed\n");

        // sz: the PC receives them.
        MemSink       sink;
        Ymodem::Stats received;
        Error         recvErr;
        pc = std::thread([&] { recvErr = Ymodem::receive(*s.line.a, sink, received, false); });
        CHECK(s.shell->execute("sz hello.txt /ym0/p0/big.bin") == 0);
        pc.join();
        CHECK(recvErr == Error::Ok);
        REQUIRE(sink.files.size() == 2);
        CHECK(sink.files[0].name == "hello.txt");
        CHECK(sink.files[0].data == "changed\n");
        CHECK(sink.files[1].name == "big.bin");
        CHECK(sink.files[1].data == big);
        CHECK(sink.files[1].mtime == 1700000100);

        // Errors before a transfer starts.
        (void)drain(*s.line.a);
        CHECK(s.shell->execute("sz nope") == 1);
        CHECK(drain(*s.line.a) == "sz: nope: No such file or directory\n");
        CHECK(s.shell->execute("rz > out") == 1);
        CHECK(drain(*s.line.a) == "rz: the serial line must be the terminal (no redirections or pipes)\n");
}
