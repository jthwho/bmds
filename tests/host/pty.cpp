/**
 * @file      pty.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Pseudo-terminals (devplan/pty.md).
 */

#include <doctest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <bmds/interrupt.h>
#include <bmds/pty.h>
#include <bmds/semaphore.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::MemSpace;
using bmds::Pty;
using bmds::WritableByteSpan;

namespace {

        ByteSpan bytes(const std::string &s) {
                return ByteSpan(reinterpret_cast<const uint8_t *>(s.data()), s.size());
        }

        // What the emulator can read now (waiting at most @p ms for the first bytes).
        std::string drain(Pty &pty, unsigned int ms = 1) {
                std::string out;
                uint8_t     buf[256];
                for (;;) {
                        auto r = pty.read(WritableByteSpan(buf, sizeof(buf)), out.empty() ? ms : 1);
                        if (r.isError()) break;
                        out.append(reinterpret_cast<char *>(buf), r.value());
                }
                return out;
        }

        std::string typed(Pty &pty) {
                std::string out;
                uint8_t     buf[64];
                for (;;) {
                        auto r = pty.console().tryRead(WritableByteSpan(buf, sizeof(buf)));
                        if (r.isError()) break;
                        out.append(reinterpret_cast<char *>(buf), r.value());
                }
                return out;
        }

        struct Opened {
                        Pty *pty;
                        Opened() {
                                auto r = Pty::open(MemSpace::Psram);
                                REQUIRE(r.isOk());
                                pty = r.value();
                        }
                        ~Opened() { pty->release(); }
        };

} // namespace

TEST_CASE("Pty: numbered pairs, all of them, and back") {
        std::vector<Pty *> open;
        for (size_t i = 0; i < Pty::MaxPtys; ++i) {
                auto r = Pty::open(MemSpace::Psram);
                REQUIRE(r.isOk());
                CHECK(r.value()->number() == i);
                CHECK(std::string(r.value()->console().name()) == "pty" + std::to_string(i));
                open.push_back(r.value());
        }
        CHECK(Pty::open(MemSpace::Psram).error() == Error::Full);
        open[3]->release();
        auto again = Pty::open(MemSpace::Psram);
        REQUIRE(again.isOk());
        CHECK(again.value()->number() == 3);
        for (Pty *p : open) p->release();
}

TEST_CASE("Pty: output to the emulator (\\n as CR LF), keys to the program") {
        Opened o;
        Pty   &pty = *o.pty;
        CHECK(pty.console().write(bytes("ab\nc"), 0).value() == 4);
        CHECK(drain(pty) == "ab\r\nc");
        auto mode = pty.console().mode();
        mode.nlToCrNl = false;
        pty.console().setMode(mode);
        CHECK(pty.console().write(bytes("x\ny"), 0).isOk());
        CHECK(drain(pty) == "x\ny");
        // Nothing to read: the emulator's read times out.
        uint8_t b[4];
        CHECK(pty.read(WritableByteSpan(b, 4), 5).error() == Error::Timeout);
        // Keys.
        CHECK(pty.write(bytes("ls\r"), 0).value() == 3);
        CHECK((pty.console().readiness() & bmds::Stream::Readable) != 0);
        CHECK(typed(pty) == "ls\r");
}

TEST_CASE("Pty: Ctrl-C raises the console's interrupt and drops the input before it") {
        Opened o;
        Pty   &pty = *o.pty;
        CHECK(!pty.console().interrupt().isRaised());
        CHECK(pty.write(bytes("abc\x03"
                              "de"),
                        0)
                      .value() == 6);
        CHECK(pty.console().interrupt().isRaised());
        CHECK(typed(pty) == "de");
        pty.console().interrupt().clear();
        // With signals off it is a byte like any other.
        auto mode = pty.console().mode();
        mode.signals = false;
        pty.console().setMode(mode);
        CHECK(pty.write(bytes("a\x03"), 0).value() == 2);
        CHECK(!pty.console().interrupt().isRaised());
        CHECK(typed(pty) == "a\x03");
}

TEST_CASE("Pty: a full output waits for the emulator; nothing is dropped") {
        Opened            o;
        Pty              &pty = *o.pty;
        const std::string big(Pty::OutputBytes + 1000, 'x');
        // No reader: the write stops when full, after its timeout, with what it wrote.
        auto partial = pty.console().write(bytes(big), 20);
        CHECK(partial.error() == Error::Timeout);
        CHECK(partial.value() <= Pty::OutputBytes);
        CHECK(drain(pty).size() == partial.value());
        // With a reader on another task, all of it.
        std::string got;
        std::thread reader([&] {
                uint8_t buf[300];
                while (got.size() < big.size()) {
                        auto r = pty.read(WritableByteSpan(buf, sizeof(buf)), 1000);
                        if (r.isError()) break;
                        got.append(reinterpret_cast<char *>(buf), r.value());
                }
        });
        CHECK(pty.console().write(bytes(big), 2000).value() == big.size());
        reader.join();
        CHECK(got == big);
}

TEST_CASE("Pty: the size the emulator sets, seen by the program") {
        Opened                 o;
        Pty                   &pty = *o.pty;
        const uint32_t         generation = pty.console().sizeGeneration();
        bmds::Semaphore        woken(1, 0);
        bmds::Interrupt::Watch watch(pty.console().resized(), woken);
        pty.setSize(bmds::Console::Size{100, 30});
        CHECK(pty.console().size().columns == 100);
        CHECK(pty.console().size().rows == 30);
        CHECK(pty.console().sizeGeneration() != generation);
        CHECK(woken.acquire(10).isOk()); // SIGWINCH's trigger
}

TEST_CASE("Pty: the emulator hangs up: the program's reads end, its writes are dropped") {
        Opened o;
        Pty   &pty = *o.pty;
        CHECK(pty.write(bytes("last"), 0).isOk());
        std::thread hang([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                pty.hangUp();
        });
        uint8_t     buf[16];
        auto        r = pty.console().read(WritableByteSpan(buf, sizeof(buf)), 1000);
        CHECK(r.value() == 4);                                            // what was typed first
        r = pty.console().read(WritableByteSpan(buf, sizeof(buf)), 1000); // waits for the hangup
        CHECK(r.error() == Error::EndOfFile);
        hang.join();
        CHECK(pty.console().write(bytes("into nothing"), 0).value() == 12);
        CHECK((pty.console().readiness() & bmds::Stream::Readable) != 0); // a read wouldn't wait
}

TEST_CASE("Pty: the program's end finishes: the emulator reads the rest, then the end") {
        Opened                o;
        Pty                  &pty = *o.pty;
        bmds::Semaphore       woken(1, 0);
        bmds::WaitList::Entry entry(*pty.readinessWaiters(), woken);
        REQUIRE(entry.isAdded());
        CHECK(pty.console().write(bytes("bye"), 0).isOk());
        CHECK(woken.acquire(10).isOk()); // pollers told of the output
        pty.programEnded();
        CHECK((pty.readiness() & bmds::Stream::HungUp) != 0);
        uint8_t buf[16];
        auto    r = pty.read(WritableByteSpan(buf, sizeof(buf)), 0);
        CHECK(r.value() == 3);
        CHECK(pty.read(WritableByteSpan(buf, sizeof(buf)), 0).error() == Error::EndOfFile);
        CHECK(pty.write(bytes("k"), 0).error() == Error::BrokenPipe);
}
