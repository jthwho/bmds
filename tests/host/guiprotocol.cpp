/**
 * @file      guiprotocol.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The GUI protocol (devplan/guiprograms.md §4a, §8): the C++ codec both ways, and the SDK's C
 * client (libbmdsgui) against a scripted service speaking through the C++ codec, on a
 * socketpair: the two halves of the ABI agree byte for byte.
 */

#include <doctest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <bmds/gui.h>
#include <bmds/guiclasses.h> // compiles as C++ too
#include <bmds/guiprotocol.h>
#include <bmds/list.h>

namespace GP = bmds::GuiProtocol;
using bmds::ByteSpan;
using bmds::List;
using bmds::StringView;

namespace {

        ByteSpan spanOf(const List<uint8_t> &l) {
                return ByteSpan(l.data(), l.size());
        }

        // The scripted service's side: whole messages read from and written to an fd.
        struct Server {
                        int                  fd;
                        std::vector<uint8_t> in;

                        // The next request, whole.
                        std::vector<uint8_t> next() {
                                for (;;) {
                                        const int64_t length = GP::messageLength(ByteSpan(in.data(), in.size()),
                                                                                 GP::RequestHeaderBytes);
                                        REQUIRE(length >= 0);
                                        if (length > 0 && in.size() >= size_t(length)) {
                                                std::vector<uint8_t> m(in.begin(), in.begin() + length);
                                                in.erase(in.begin(), in.begin() + length);
                                                return m;
                                        }
                                        uint8_t       buf[4096];
                                        const ssize_t n = read(fd, buf, sizeof(buf));
                                        REQUIRE(n > 0);
                                        in.insert(in.end(), buf, buf + n);
                                }
                        }

                        void send(const List<uint8_t> &out) {
                                REQUIRE(write(fd, out.data(), out.size()) == ssize_t(out.size()));
                        }
        };

        uint16_t opcodeOf(const std::vector<uint8_t> &m) {
                return uint16_t(m[0] | m[1] << 8);
        }

        GP::Reader bodyOf(const std::vector<uint8_t> &m) {
                return GP::Reader(ByteSpan(m.data(), m.size()).mid(GP::RequestHeaderBytes));
        }

        std::vector<std::string> errorsSeen;

        void collectError(void *, const bmds_gui_error_t *e) {
                errorsSeen.push_back(std::string(bmds_gui_error_name(e->code)) + " " + std::to_string(e->sequence) +
                                     " " + std::to_string(e->bad));
        }

} // namespace

TEST_CASE("GuiProtocol: every tag written and read back, padded to 4; lists by their elements") {
        List<uint8_t> out;
        GP::Writer    w(out);
        w.beginRequest(GP::Opcode::Set, 0);
        w.value(GP::Value::integer(-5));
        w.value(GP::Value::boolean(true));
        w.value(GP::Value::string("héllo"));
        w.value(GP::Value::atom(42));
        w.value(GP::Value::id(0x01000003));
        GP::Value rect;
        rect.tag = GP::Tag::Rect;
        rect.rect = {-3, 4, 640, 480};
        w.value(rect);
        GP::Value big;
        big.tag = GP::Tag::Long;
        big.number = -1234567890123LL;
        w.value(big);
        GP::Value color;
        color.tag = GP::Tag::Color;
        color.number = 0x123456;
        color.role = 7;
        w.value(color);
        w.listHeader(GP::Tag::String, 2);
        w.element(GP::Value::string("a"));
        w.element(GP::Value::string("bcdef"));
        w.end();
        REQUIRE(w.ok());
        CHECK(out.size() % 4 == 0);
        CHECK(GP::messageLength(spanOf(out), GP::RequestHeaderBytes) == int64_t(out.size()));

        GP::Reader r(spanOf(out).mid(GP::RequestHeaderBytes));
        GP::Value  v;
        REQUIRE(r.value(v));
        CHECK((v.tag == GP::Tag::Int && v.number == -5));
        REQUIRE(r.value(v));
        CHECK((v.tag == GP::Tag::Bool && v.number == 1));
        REQUIRE(r.value(v));
        CHECK(v.text == StringView("héllo"));
        REQUIRE(r.value(v));
        CHECK((v.tag == GP::Tag::Atom && v.number == 42));
        REQUIRE(r.value(v));
        CHECK((v.tag == GP::Tag::Id && v.number == 0x01000003));
        REQUIRE(r.value(v));
        CHECK((v.rect.x == -3 && v.rect.y == 4 && v.rect.width == 640 && v.rect.height == 480));
        REQUIRE(r.value(v));
        CHECK(v.number == -1234567890123LL);
        REQUIRE(r.value(v));
        CHECK((v.number == 0x123456 && v.role == 7));
        REQUIRE(r.value(v));
        CHECK((v.tag == GP::Tag::List && v.elements == GP::Tag::String && v.count == 2));
        GP::Reader items(v.items);
        GP::Value  e;
        REQUIRE(items.element(GP::Tag::String, e));
        CHECK(e.text == StringView("a"));
        REQUIRE(items.element(GP::Tag::String, e));
        CHECK(e.text == StringView("bcdef"));
        CHECK(r.remaining() == 0);
        // Past the end: not a value, and the reader stays failed.
        CHECK_FALSE(r.value(v));
        CHECK_FALSE(r.ok());
}

TEST_CASE("GuiProtocol: message lengths: whole, waiting, and wrong") {
        const uint8_t short4[] = {1, 0, 0, 0};
        CHECK(GP::messageLength(ByteSpan(short4, 4), 8) == 0);
        const uint8_t odd[] = {1, 0, 0, 0, 10, 0, 0, 0};
        CHECK(GP::messageLength(ByteSpan(odd, 8), 8) == -1); // not a multiple of 4
        const uint8_t tiny[] = {1, 0, 0, 0, 4, 0, 0, 0};
        CHECK(GP::messageLength(ByteSpan(tiny, 8), 8) == -1); // shorter than its header
        const uint8_t huge[] = {1, 0, 0, 0, 0, 0, 0, 1};
        CHECK(GP::messageLength(ByteSpan(huge, 8), 8) == -1); // over MaxMessage
        // A bad tag and a bad bool aren't values.
        const uint8_t badTag[] = {99, 0, 0, 0};
        GP::Reader    r(ByteSpan(badTag, 4));
        GP::Value     v;
        CHECK_FALSE(r.value(v));
        const uint8_t badBool[] = {3, 0, 0, 0, 2, 0, 0, 0};
        GP::Reader    b(ByteSpan(badBool, 8));
        CHECK_FALSE(b.value(v));
}

TEST_CASE("GuiProtocol: the C client and a C++ service agree: setup, names, requests, answers, events, errors") {
        int fds[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        Server server{fds[1], {}};
        errorsSeen.clear();

        std::thread service([&] {
                // Hello: the magic, version 1.0, no authentication.
                auto hello = server.next();
                CHECK(opcodeOf(hello) == uint16_t(GP::Opcode::Hello));
                GP::Reader h = bodyOf(hello);
                ByteSpan   magic = h.bytes(8);
                CHECK(std::memcmp(magic.data(), GP::Magic, 8) == 0);
                CHECK(h.u16() == GP::Major);
                CHECK(h.u16() == GP::Minor);
                CHECK(h.u32() == 0);
                CHECK(h.u32() == 0);
                List<uint8_t> out;
                GP::Writer    w(out);
                w.beginAnswer(GP::Kind::Reply, 0, 0);
                w.u32(uint32_t(GP::HelloStatus::Accepted));
                w.u16(GP::Major);
                w.u16(GP::Minor);
                w.u32(0x01000000); // the id base
                w.u32(0x00ffffff); // the mask
                w.u32(GP::MaxMessage);
                w.u32(0); // no extensions
                w.u32(1); // the display: its screen
                w.string("screen");
                GP::Value screen;
                screen.tag = GP::Tag::Rect;
                screen.rect = {0, 0, 1280, 720};
                w.value(screen);
                w.end();
                server.send(out);

                // InternNames: two names, numbered 100 and 101.
                auto names = server.next();
                CHECK(opcodeOf(names) == uint16_t(GP::Opcode::InternNames));
                GP::Reader n = bodyOf(names);
                CHECK(n.u32() == 2);
                CHECK(n.string() == StringView("Button"));
                CHECK(n.string() == StringView("text"));
                out.clear();
                w.beginAnswer(GP::Kind::Reply, uint16_t(GP::Opcode::InternNames), 1);
                w.u32(2);
                w.u32(100);
                w.u32(101);
                w.end();
                server.send(out);

                // Create and Set come in one batch, unanswered; Sync answers.
                auto create = server.next();
                CHECK(opcodeOf(create) == uint16_t(GP::Opcode::Create));
                GP::Reader c = bodyOf(create);
                CHECK(c.u32() == 0x01000001);
                CHECK(c.u32() == 100);
                CHECK(c.u32() == 0);
                CHECK(c.u32() == 1);
                CHECK(c.u32() == 101);
                GP::Value v;
                REQUIRE(c.value(v));
                CHECK(v.text == StringView("OK"));
                auto set = server.next();
                CHECK(opcodeOf(set) == uint16_t(GP::Opcode::Set));
                auto sync = server.next();
                CHECK(opcodeOf(sync) == uint16_t(GP::Opcode::Sync));
                out.clear();
                // An error for request 3 (the Set) comes before Sync's answer, and an event.
                w.beginAnswer(GP::Kind::Error, uint16_t(GP::ErrorCode::BadName), 3);
                w.u32(uint32_t(GP::Opcode::Set));
                w.u32(999);
                w.end();
                w.beginAnswer(GP::Kind::Event, 0, 4);
                w.u32(0x01000001); // the source
                w.u32(102);        // clicked
                w.u32(1234);       // the time
                w.u32(1);
                w.value(GP::Value::string("pressed"));
                w.end();
                w.beginAnswer(GP::Kind::Reply, uint16_t(GP::Opcode::Sync), 4);
                w.end();
                server.send(out);

                // Get: one property's value.
                auto get = server.next();
                CHECK(opcodeOf(get) == uint16_t(GP::Opcode::Get));
                out.clear();
                w.beginAnswer(GP::Kind::Reply, uint16_t(GP::Opcode::Get), 5);
                w.u32(1);
                w.value(GP::Value::string("OK"));
                w.end();
                // A Get that fails answers an error instead.
                server.send(out);
                auto bad = server.next();
                CHECK(opcodeOf(bad) == uint16_t(GP::Opcode::Get));
                out.clear();
                w.beginAnswer(GP::Kind::Error, uint16_t(GP::ErrorCode::BadId), 6);
                w.u32(uint32_t(GP::Opcode::Get));
                w.u32(0x01000099);
                w.end();
                server.send(out);
                close(server.fd);
        });

        bmds_gui_t *gui = bmds_gui_connect_fd(fds[0]);
        REQUIRE(gui != nullptr);
        bmds_gui_on_error(gui, collectError, nullptr);
        bmds_gui_value_t screen;
        REQUIRE(bmds_gui_display(gui, "screen", &screen) == 0);
        CHECK((screen.tag == BMDS_GUI_RECT && screen.width == 1280 && screen.height == 720));
        bmds_gui_value_t none;
        CHECK(bmds_gui_display(gui, "nothing", &none) == -1);

        const char *wanted[] = {"Button", "text"};
        uint32_t    atoms[2];
        REQUIRE(bmds_gui_atoms(gui, wanted, 2, atoms) == 0);
        CHECK((atoms[0] == 100 && atoms[1] == 101));
        CHECK(bmds_gui_atom(gui, "text") == 101); // cached: no request

        const uint32_t id = bmds_gui_new_id(gui);
        CHECK(id == 0x01000001);
        bmds_gui_prop_t text = {101, bmds_gui_string("OK")};
        REQUIRE(bmds_gui_create(gui, id, 100, 0, &text, 1) == 0);
        bmds_gui_prop_t bogus = {999, bmds_gui_int(1)};
        REQUIRE(bmds_gui_set(gui, id, &bogus, 1) == 0); // no answer: its error comes later
        REQUIRE(bmds_gui_sync(gui) == 0);
        REQUIRE(errorsSeen.size() == 1);
        CHECK(errorsSeen[0] == "BadName 3 999");

        bmds_gui_event_t e;
        REQUIRE(bmds_gui_next_event(gui, &e, 1000) == 1); // queued while Sync waited
        CHECK((e.source == id && e.name == 102 && e.time_ms == 1234 && e.count == 1));
        CHECK(std::string(e.values[0].text, e.values[0].length) == "pressed");

        bmds_gui_value_t got;
        const uint32_t   property = 101;
        REQUIRE(bmds_gui_get(gui, id, &property, 1, &got) == 0);
        CHECK(std::string(got.text, got.length) == "OK");
        errno = 0;
        CHECK(bmds_gui_get(gui, 0x01000099, &property, 1, &got) == -1);
        CHECK(errno == EINVAL);
        CHECK(errorsSeen.size() == 2);

        CHECK(bmds_gui_next_event(gui, &e, 1000) == -1); // the service has gone
        CHECK(errno == EPIPE);
        service.join();
        bmds_gui_close(gui);
}
