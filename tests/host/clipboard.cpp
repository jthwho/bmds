/**
 * @file      clipboard.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The clipboard's pure parts (devplan/clipboard.md): types, conversions,
 * base64 in pieces.
 */

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>
#include <bmds/base64.h>
#include <bmds/clipboard.h>
#include <bmds/clipboardconversions.h>
#include <bmds/clipboardreader.h>
#include <bmds/clipboardtype.h>
#include <bmds/clipcommand.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include <bmds/vfs.h>
#include "fakeconsole.h"

using bmds::Base64Decoder;
using bmds::Base64Encoder;
using bmds::ByteSpan;
using bmds::Clipboard;
using bmds::ClipboardConversions;
using bmds::ClipboardConverter;
using bmds::ClipboardReader;
using bmds::ClipboardType;
using bmds::Error;
using bmds::FileHandle;
using bmds::Shell;
using bmds::Span;
using bmds::StringView;
using bmds::WaitForever;
using Selection = Clipboard::Selection;
namespace Vfs = bmds::Vfs;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        std::string normal(StringView type) {
                char buffer[ClipboardType::BufferBytes];
                auto [n, err] = ClipboardType::normalize(type, Span<char>(buffer, sizeof(buffer)));
                if (err.isError()) return "error";
                return str(n);
        }

        // Decodes @p text split into pieces of @p piece characters.
        std::string decode(const std::string &text, size_t piece, bool *ok = nullptr) {
                Base64Decoder        d;
                std::vector<uint8_t> out;
                for (size_t i = 0; i < text.size(); i += piece) {
                        const std::string part = text.substr(i, piece);
                        uint8_t           buf[64];
                        const size_t      n = d.put(StringView(part.data(), part.size()), buf);
                        out.insert(out.end(), buf, buf + n);
                }
                uint8_t tail[Base64Decoder::MaxFinish];
                auto [n, err] = d.finish(tail);
                if (ok != nullptr) *ok = err.isOk();
                out.insert(out.end(), tail, tail + n);
                return std::string(out.begin(), out.end());
        }

        std::string encode(const std::string &data, size_t piece) {
                Base64Encoder e;
                std::string   out;
                for (size_t i = 0; i < data.size(); i += piece) {
                        const std::string part = data.substr(i, piece);
                        char              buf[64];
                        const size_t      n =
                                e.put(ByteSpan(reinterpret_cast<const uint8_t *>(part.data()), part.size()), buf);
                        out.append(buf, n);
                }
                char tail[Base64Encoder::MaxFinish];
                out.append(tail, e.finish(tail));
                return out;
        }

        // Converts @p data with @p builtin in pieces of @p piece bytes.
        std::string convert(ClipboardConversions::Builtin builtin, const std::string &data, size_t piece) {
                ClipboardConverter c(builtin);
                std::string        out;
                for (size_t i = 0; i < data.size(); i += piece) {
                        const std::string    part = data.substr(i, piece);
                        std::vector<uint8_t> buf(ClipboardConverter::maxOutput(part.size()));
                        const size_t n = c.put(ByteSpan(reinterpret_cast<const uint8_t *>(part.data()), part.size()),
                                               buf.data());
                        REQUIRE(n <= buf.size());
                        out.append(reinterpret_cast<const char *>(buf.data()), n);
                }
                uint8_t      tail[ClipboardConverter::MaxFinish];
                const size_t n = c.finish(tail);
                out.append(reinterpret_cast<const char *>(tail), n);
                return out;
        }

        // Converts in every piece size, checking they agree.
        std::string convertAll(ClipboardConversions::Builtin builtin, const std::string &data) {
                const std::string whole = convert(builtin, data, data.size() + 1);
                for (size_t piece = 1; piece <= data.size(); ++piece) CHECK(convert(builtin, data, piece) == whole);
                return whole;
        }

        struct Reported {
                        std::vector<uint32_t> lines;
        };

        void report(void *arg, uint32_t line, StringView text) {
                (void)text;
                static_cast<Reported *>(arg)->lines.push_back(line);
                return;
        }

} // namespace

TEST_CASE("ClipboardType normalizes MIME types") {
        CHECK(normal("text/plain") == "text/plain");
        CHECK(normal("  Text/Plain ; Charset=UTF-8 ") == "text/plain;charset=utf-8");
        CHECK(normal("text/plain;charset=utf8") == "text/plain;charset=utf-8");
        CHECK(normal("text") == "text/plain;charset=utf-8");
        CHECK(normal("TEXT") == "text/plain;charset=utf-8");
        CHECK(normal("image/svg+xml") == "image/svg+xml");
        CHECK(normal("text/plain;") == "text/plain");
        // Other parameters' values keep their case; quoted values may hold ; and spaces.
        CHECK(normal("multipart/mixed; Boundary=AbC") == "multipart/mixed;boundary=AbC");
        CHECK(normal("a/b; x=\"1; 2\"; y=z") == "a/b;x=\"1; 2\";y=z");
        CHECK(normal("a/b;x=\"q\\\"r\"") == "a/b;x=\"q\\\"r\"");
        // Not types.
        CHECK(normal("") == "error");
        CHECK(normal("text") != normal("textual"));
        CHECK(normal("plain") == "error");
        CHECK(normal("text/") == "error");
        CHECK(normal("/plain") == "error");
        CHECK(normal("text plain/x") == "error");
        CHECK(normal("text/plain;charset") == "error");
        CHECK(normal("text/plain;charset=") == "error");
        CHECK(normal("a/b;x=\"open") == "error");
        // Too long.
        const std::string longType = "application/" + std::string(51, 'x');
        CHECK(normal(StringView(longType.data(), longType.size())) == longType); // 63 bytes
        const std::string tooLong = longType + "y";
        CHECK(normal(StringView(tooLong.data(), tooLong.size())) == "error");
        char small[8];
        CHECK(ClipboardType::normalize("text/plain", Span<char>(small, sizeof(small))).error() ==
              Error::BufferTooSmall);
}

TEST_CASE("ClipboardType compares and names files' types") {
        CHECK(ClipboardType::equal("text", "text/plain; charset=UTF-8"));
        CHECK(ClipboardType::equal("IMAGE/PNG", "image/png"));
        CHECK_FALSE(ClipboardType::equal("text/plain", "text/plain;charset=utf-8"));
        CHECK_FALSE(ClipboardType::equal("bad", "bad"));
        CHECK(ClipboardType::isValid("text/uri-list"));
        CHECK_FALSE(ClipboardType::isValid("uri-list"));
        CHECK(str(ClipboardType::fromFileName("/sd0/p0/shot.PNG")) == "image/png");
        CHECK(str(ClipboardType::fromFileName("notes.txt")) == "text/plain;charset=utf-8");
        CHECK(str(ClipboardType::fromFileName("a.tar/README")).empty());
        CHECK(str(ClipboardType::fromFileName("Makefile")).empty());
        CHECK(str(ClipboardType::fromFileName("x.unknown")).empty());
}

TEST_CASE("Base64 decodes in pieces") {
        const std::string text = "TWFueSBoYW5kcyBtYWtlIGxpZ2h0IHdvcmsu";
        for (size_t piece = 1; piece <= text.size(); ++piece)
                CHECK(decode(text, piece) == "Many hands make light work.");
        CHECK(decode("", 1) == "");
        CHECK(decode("Zg==", 1) == "f");
        CHECK(decode("Zm8=", 3) == "fo");
        CHECK(decode("Zm9v", 2) == "foo");
        // Without padding; with line breaks and spaces.
        CHECK(decode("Zg", 1) == "f");
        CHECK(decode("Zm8", 2) == "fo");
        CHECK(decode("Zm9v\r\nYm Fy", 3) == "foobar");
        // Every byte value.
        std::string all;
        for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));
        CHECK(decode(encode(all, 7), 5) == all);
        // Bad: a stray character, data after padding, a lone sextet, padding too early.
        bool ok = true;
        decode("Zm9v!", 2, &ok);
        CHECK_FALSE(ok);
        decode("Zg==Zg==", 3, &ok);
        CHECK_FALSE(ok);
        decode("Zm9vY", 1, &ok);
        CHECK_FALSE(ok);
        decode("Z===", 1, &ok);
        CHECK_FALSE(ok);
        decode("Zg=", 1, &ok);
        CHECK_FALSE(ok);
        // A failed decoder writes nothing more until it starts again.
        Base64Decoder d;
        uint8_t       out[8];
        CHECK(d.put("Zm9v*Zm9v", out) == 3);
        CHECK(d.isFailed());
        CHECK(d.put("Zm9v", out) == 0);
        CHECK(d.finish(out).error() == Error::CorruptData);
        CHECK_FALSE(d.isFailed());
        CHECK(d.put("Zm9v", out) == 3);
}

TEST_CASE("Base64 encodes in pieces") {
        for (size_t piece = 1; piece < 8; ++piece) {
                CHECK(encode("", piece) == "");
                CHECK(encode("f", piece) == "Zg==");
                CHECK(encode("fo", piece) == "Zm8=");
                CHECK(encode("foo", piece) == "Zm9v");
                CHECK(encode("foobar", piece) == "Zm9vYmFy");
                CHECK(encode("Many hands make light work.", piece) == "TWFueSBoYW5kcyBtYWtlIGxpZ2h0IHdvcmsu");
        }
        static_assert(Base64Encoder::maxOutput(1) == 4 && Base64Encoder::maxOutput(3) == 4);
        static_assert(Base64Decoder::maxOutput(1) == 3 && Base64Decoder::maxOutput(4) == 3);
}

TEST_CASE("ClipboardConversions finds the shortest way") {
        using Builtin = ClipboardConversions::Builtin;
        ClipboardConversions table;
        const char           config[] = "[Convert]\n"
                                        "text/html>text/plain;charset=utf-8=html2text -\n"
                                        "image/bmp>image/png = bmp2png\n"
                                        "image/x-foo>image/bmp=foo2bmp\n"
                                        "no-arrow=x\n"
                                        "bad>type/ok=x\n"
                                        "a/b>c/d=\n"
                                        "image/bmp>image/png=second\n"
                                        "[Other]\n"
                                        "x/y>z/w=ignored\n";
        Reported             reported;
        CHECK(table.read(config, report, &reported) == Error::Ok);
        CHECK(reported.lines == std::vector<uint32_t>{5, 6, 7});
        CHECK(table.addBuiltins() == Error::Ok);
        REQUIRE(table.count() == 8);
        CHECK(str(table.converter(0).command) == "html2text -");
        CHECK(str(table.converter(1).command) == "bmp2png");

        // As offered.
        StringView offered1[] = {"image/png", "text"};
        auto [p0, e0] = table.find(Span<const StringView>(offered1), "text/plain;charset=UTF-8");
        REQUIRE(e0.isOk());
        CHECK(p0.offered == 1);
        CHECK(p0.steps == 0);

        // One step: the earliest offered type that has a way wins.
        StringView offered2[] = {"application/x-nothing", "text/html", "text/uri-list"};
        auto [p1, e1] = table.find(Span<const StringView>(offered2), "text");
        REQUIRE(e1.isOk());
        CHECK(p1.offered == 1);
        CHECK(p1.steps == 1);
        CHECK(p1.converter[0] == 0);

        // Two steps.
        StringView offered3[] = {"image/x-foo"};
        auto [p2, e2] = table.find(Span<const StringView>(offered3), "image/png");
        REQUIRE(e2.isOk());
        CHECK(p2.steps == 2);
        CHECK(p2.converter[0] == 2);
        CHECK(p2.converter[1] == 1);

        // A shorter way from a later type beats a longer one from an earlier type.
        StringView offered4[] = {"text/html", "text/plain;charset=utf-8"};
        auto [p3, e3] = table.find(Span<const StringView>(offered4), "text/plain");
        REQUIRE(e3.isOk());
        CHECK(p3.offered == 1);
        CHECK(p3.steps == 1);
        CHECK(table.converter(p3.converter[0]).builtin == Builtin::TextToLatin1);

        // Two steps through a built-in one: HTML to text to ASCII.
        StringView offered5[] = {"text/html"};
        auto [p4, e4] = table.find(Span<const StringView>(offered5), "text/plain");
        REQUIRE(e4.isOk());
        CHECK(p4.steps == 2);

        // No way; not a type.
        StringView offered6[] = {"image/png"};
        CHECK(table.find(Span<const StringView>(offered6), "text").error() == Error::NotFound);
        CHECK(table.find(Span<const StringView>(offered6), "nope").error() == Error::Invalid);
        CHECK(table.find(Span<const StringView>(), "text").error() == Error::NotFound);
}

TEST_CASE("ClipboardConversions keeps the first of a pair and fills up") {
        ClipboardConversions table;
        CHECK(table.add("text/html", "text", "first") == Error::Ok);
        CHECK(table.add("TEXT/HTML", "text/plain;charset=utf-8", "second") == Error::Exists);
        CHECK(table.add("text/html", "text", "  ") == Error::Invalid);
        CHECK(table.addBuiltins() == Error::Ok);
        const size_t n = table.count();
        CHECK(table.addBuiltins() == Error::Ok); // already there
        CHECK(table.count() == n);
        std::vector<std::string> types;
        for (size_t i = table.count(); i < ClipboardConversions::MaxConverters; ++i)
                types.push_back("x/t" + std::to_string(i));
        for (const std::string &t : types) CHECK(table.add(StringView(t.data(), t.size()), "x/out", "c") == Error::Ok);
        CHECK(table.count() == ClipboardConversions::MaxConverters);
        CHECK(table.add("y/a", "y/b", "c") == Error::Full);
}

TEST_CASE("Built-in converters convert in pieces") {
        using Builtin = ClipboardConversions::Builtin;
        // Latin-1 and text.
        CHECK(convertAll(Builtin::Latin1ToText, "caf\xe9 \xff!") == "caf\xc3\xa9 \xc3\xbf!");
        CHECK(convertAll(Builtin::TextToLatin1, "caf\xc3\xa9 \xc3\xbf \xe2\x82\xac!") == "caf\xe9 \xff ?!");
        CHECK(convertAll(Builtin::TextToLatin1, "bad \xc3 x") == "bad ? x");
        CHECK(convertAll(Builtin::TextToLatin1, "cut \xe2\x82") == "cut ?");
        CHECK(convertAll(Builtin::None, "as is\n") == "as is\n");
        // URI lists: paths, %XX decoded, comments and blank lines dropped, CR LF, no LF at the end.
        CHECK(convertAll(Builtin::UriListToText, "file:///sd0/p0/a%20b.txt\r\n"
                                                 "# a comment\r\n"
                                                 "\r\n"
                                                 "file://host/boot/x%2y%41\r\n"
                                                 "https://example.com/%20\r\n"
                                                 "fil\r\n"
                                                 "file:/z\r\n") == "/sd0/p0/a b.txt\n"
                                                                   "/boot/x%2yA\n"
                                                                   "https://example.com/%20\n"
                                                                   "fil\n"
                                                                   "file:/z");
        CHECK(convertAll(Builtin::UriListToText, "file:///a\nfile:///b") == "/a\n/b");
        CHECK(convertAll(Builtin::UriListToText, "file:///p%4") == "/p%4");
        CHECK(convertAll(Builtin::UriListToText, "file:/") == "file:/");
        CHECK(convertAll(Builtin::UriListToText, "# only\n\n") == "");
        // A converter starts again after finish().
        ClipboardConverter c(Builtin::UriListToText);
        uint8_t            buf[64];
        const char         first[] = "file:///a\n";
        size_t             n = c.put(ByteSpan(reinterpret_cast<const uint8_t *>(first), sizeof(first) - 1), buf);
        n += c.finish(buf + n);
        CHECK(std::string(reinterpret_cast<char *>(buf), n) == "/a");
        const char second[] = "file:///b";
        n = c.put(ByteSpan(reinterpret_cast<const uint8_t *>(second), sizeof(second) - 1), buf);
        n += c.finish(buf + n);
        CHECK(std::string(reinterpret_cast<char *>(buf), n) == "/b");
}

namespace {

        // How the helpers below show an error.
        std::string e(Error err) {
                return "<" + std::string(err.desc()) + ">";
        }

        ByteSpan bytes(const std::string &s) {
                return ByteSpan(reinterpret_cast<const uint8_t *>(s.data()), s.size());
        }

        // Everything a handle gives, to its end.
        std::string drain(FileHandle::Ptr handle) {
                std::string out;
                for (;;) {
                        uint8_t buf[100];
                        auto [n, err] = handle->read(bmds::WritableByteSpan(buf, sizeof(buf)), 1000);
                        if (err.isError()) {
                                if (err != Error::EndOfFile) out += "<" + std::string(err.desc()) + ">";
                                return out;
                        }
                        out.append(reinterpret_cast<const char *>(buf), n);
                }
        }

        std::string request(Clipboard &c, Selection s, StringView type) {
                auto [h, err] = c.request(s, type, nullptr);
                if (err.isError()) return "<" + std::string(err.desc()) + ">";
                return drain(std::move(h));
        }

        // What a reader gives for @p wanted, or its error.
        std::string readAs(Clipboard &c, Selection s, StringView wanted) {
                auto        reader = std::make_unique<ClipboardReader>(c);
                const Error err = reader->open(s, wanted);
                if (err.isError()) return "<" + std::string(err.desc()) + ">";
                std::string out;
                for (;;) {
                        uint8_t buf[7]; // small: the converters' output is handed out in pieces
                        auto [n, rerr] = reader->read(bmds::WritableByteSpan(buf, sizeof(buf)), 1000);
                        if (rerr == Error::EndOfFile) return out;
                        if (rerr.isError()) return out + "<" + rerr.desc() + ">";
                        out.append(reinterpret_cast<const char *>(buf), n);
                }
        }

        // An owner that renders on request, recording what it was told.
        struct TestOwner {
                        std::vector<std::string> asked;
                        std::vector<uint32_t>    lost;
                        bool                     answer = true;

                        static void requested(void *arg, uint32_t serial, StringView type, FileHandle::Ptr write) {
                                (void)serial;
                                auto *self = static_cast<TestOwner *>(arg);
                                self->asked.push_back(std::string(type.data(), type.size()));
                                if (!self->answer) return; // dropped: the reader sees the end
                                const std::string data = "rendered " + self->asked.back();
                                (void)write->write(bytes(data), WaitForever); // under the pipe's 4 KB
                        }

                        static void lostIt(void *arg, uint32_t serial) {
                                static_cast<TestOwner *>(arg)->lost.push_back(serial);
                        }

                        Clipboard::Owner owner() {
                                Clipboard::Owner o;
                                o.requested = requested;
                                o.lost = lostIt;
                                o.arg = this;
                                o.name = "test owner, long name";
                                o.process = 7;
                                o.window = 3;
                                return o;
                        }
        };

} // namespace

TEST_CASE("Clipboard stores copies and hands them out") {
        Clipboard        c;
        const StringView text[] = {"text"};
        CHECK(request(c, Selection::Clipboard, "text") == e(Error::Empty));
        auto [serial, err] = c.copy(Selection::Clipboard, Span<const StringView>(text), bytes("hello"), "me", 5);
        REQUIRE(err.isOk());
        CHECK(serial != 0);
        CHECK(request(c, Selection::Clipboard, "TEXT/plain; charset=UTF-8") == "hello");
        CHECK(request(c, Selection::Clipboard, "image/png") == e(Error::NotFound));
        CHECK(request(c, Selection::Clipboard, "nope") == e(Error::Invalid));
        CHECK(request(c, Selection::Primary, "text") == e(Error::Empty));
        CHECK(c.storedBytes() == 5);
        CHECK(c.storedTextSize(Selection::Clipboard) == 5);

        Clipboard::Description d;
        c.describe(Selection::Clipboard, d);
        CHECK(d.serial == serial);
        CHECK(std::string(d.owner) == "me");
        CHECK(d.process == 5);
        CHECK(d.count == 1);
        CHECK(std::string(d.types[0].type) == "text/plain;charset=utf-8");
        CHECK(d.types[0].size == 5);

        // A reader keeps its bytes after a newer copy replaces them.
        auto [held, herr] = c.request(Selection::Clipboard, "text", nullptr);
        REQUIRE(herr.isOk());
        const StringView two[] = {"text/x-a", "text/x-b"};
        auto [serial2, err2] = c.copy(Selection::Clipboard, Span<const StringView>(two), bytes("abc"), "x", 0);
        REQUIRE(err2.isOk());
        CHECK(serial2 > serial);
        CHECK(drain(std::move(held)) == "hello");
        CHECK(c.storedBytes() == 3); // one block for both types
        CHECK(request(c, Selection::Clipboard, "text/x-b") == "abc");
        CHECK(c.storedTextSize(Selection::Clipboard) == 0);

        // Bad offers.
        const StringView dup[] = {"text", "text/plain;charset=utf8"};
        CHECK(c.copy(Selection::Clipboard, Span<const StringView>(dup), bytes("x"), "", 0).error() == Error::Invalid);
        const StringView bad[] = {"plain"};
        CHECK(c.copy(Selection::Clipboard, Span<const StringView>(bad), bytes("x"), "", 0).error() == Error::Invalid);
        CHECK(c.copy(Selection::Clipboard, Span<const StringView>(), bytes("x"), "", 0).error() == Error::Invalid);
        CHECK(request(c, Selection::Clipboard, "text/x-a") == "abc"); // still there

        // Clearing: only the offer named, or any.
        CHECK(c.clear(Selection::Clipboard, serial) == Error::InvalidState);
        CHECK(c.clear(Selection::Clipboard, serial2) == Error::Ok);
        CHECK(c.clear(Selection::Clipboard, 0) == Error::Ok);
        CHECK(c.storedBytes() == 0);
}

TEST_CASE("Clipboard keeps to its limit") {
        Clipboard c;
        Reported  reported;
        // [Limits] is sysconf's clipboard/limit now: reported, not taken.
        REQUIRE(c.configure("[Limits]\nBytes=10\n", report, &reported).isOk());
        CHECK(c.limit() == Clipboard::DefaultLimitBytes);
        CHECK(reported.lines == std::vector<uint32_t>{2});
        c.setLimit(10);
        const StringView text[] = {"text"};
        CHECK(c.copy(Selection::Clipboard, Span<const StringView>(text), bytes("0123456789"), "", 0).isOk());
        CHECK(c.copy(Selection::Primary, Span<const StringView>(text), bytes("x"), "", 0).error() == Error::NoSpace);
        // Replacing the clipboard frees its bytes first.
        CHECK(c.copy(Selection::Clipboard, Span<const StringView>(text), bytes("abcdefgh"), "", 0).isOk());
        CHECK(c.copy(Selection::Primary, Span<const StringView>(text), bytes("ij"), "", 0).isOk());
        CHECK(c.storedBytes() == 10);
        CHECK(c.copy(Selection::Primary, Span<const StringView>(text), bytes("ijk"), "", 0).error() == Error::NoSpace);
        CHECK(request(c, Selection::Primary, "text") == "ij");
}

TEST_CASE("Clipboard asks owners for types on request") {
        Clipboard        c;
        TestOwner        owner;
        const StringView types[] = {"text/html", "image/png"};
        auto [serial, err] = c.offer(Selection::Clipboard, Span<const StringView>(types), owner.owner());
        REQUIRE(err.isOk());
        CHECK(request(c, Selection::Clipboard, "IMAGE/PNG") == "rendered image/png");
        CHECK(owner.asked == std::vector<std::string>{"image/png"});
        owner.answer = false;
        CHECK(request(c, Selection::Clipboard, "text/html") == ""); // the end, nothing written

        Clipboard::Description d;
        c.describe(Selection::Clipboard, d);
        CHECK(std::string(d.owner) == "test owner, lon"); // MaxOwnerName
        CHECK(d.window == 3);
        CHECK(d.types[1].size == -1);

        // Storing one of them; a stale serial or a type not offered.
        CHECK(c.store(Selection::Clipboard, serial, "text/html", bytes("<b>")) == Error::Ok);
        CHECK(c.store(Selection::Clipboard, serial + 1, "text/html", bytes("<i>")) == Error::InvalidState);
        CHECK(c.store(Selection::Clipboard, serial, "text", bytes("x")) == Error::NotFound);
        owner.asked.clear();
        CHECK(request(c, Selection::Clipboard, "text/html") == "<b>");
        CHECK(owner.asked.empty());

        // The owner going: what it stored stays, the rest goes.
        CHECK(c.withdraw(Selection::Clipboard, serial + 1) == Error::InvalidState);
        CHECK(c.withdraw(Selection::Clipboard, serial) == Error::Ok);
        c.describe(Selection::Clipboard, d);
        CHECK(d.count == 1);
        CHECK(std::string(d.types[0].type) == "text/html");
        CHECK(request(c, Selection::Clipboard, "image/png") == e(Error::NotFound));
        CHECK(owner.lost.empty());

        // A newer offer tells the old owner; so does someone else clearing it, not the owner itself.
        auto [s2, e2] = c.offer(Selection::Primary, Span<const StringView>(types), owner.owner());
        REQUIRE(e2.isOk());
        const StringView text[] = {"text"};
        REQUIRE(c.copy(Selection::Primary, Span<const StringView>(text), bytes("new"), "", 0).isOk());
        CHECK(owner.lost == std::vector<uint32_t>{s2});
        auto [s3, e3] = c.offer(Selection::Primary, Span<const StringView>(types), owner.owner());
        REQUIRE(e3.isOk());
        CHECK(c.clear(Selection::Primary, s3) == Error::Ok);
        CHECK(owner.lost.size() == 1);
        auto [s4, e4] = c.offer(Selection::Primary, Span<const StringView>(types), owner.owner());
        REQUIRE(e4.isOk());
        CHECK(c.clear(Selection::Primary, 0) == Error::Ok);
        CHECK(owner.lost == std::vector<uint32_t>{s2, s4});

        // Withdrawing an offer with nothing stored empties the selection.
        auto [s5, e5] = c.offer(Selection::Primary, Span<const StringView>(types), owner.owner());
        REQUIRE(e5.isOk());
        CHECK(c.withdraw(Selection::Primary, s5) == Error::Ok);
        c.describe(Selection::Primary, d);
        CHECK(d.serial == 0);
}

TEST_CASE("ClipboardReader converts on the way") {
        Clipboard        c;
        const StringView uris[] = {"text/uri-list"};
        REQUIRE(c.copy(Selection::Clipboard, Span<const StringView>(uris),
                       bytes("# comment\r\nfile:///sd0/p0/a%20b\r\nfile:///boot/x\r\n"), "", 0)
                        .isOk());
        CHECK(readAs(c, Selection::Clipboard, "text") == "/sd0/p0/a b\n/boot/x");
        CHECK(readAs(c, Selection::Clipboard, "text/plain") == "/sd0/p0/a b\n/boot/x"); // two steps
        CHECK(readAs(c, Selection::Clipboard, "text/uri-list").size() == 49);           // as is
        CHECK(readAs(c, Selection::Clipboard, "image/png") == e(Error::NotFound));
        CHECK(readAs(c, Selection::Clipboard, "bad") == e(Error::Invalid));
        CHECK(readAs(c, Selection::Primary, "text") == e(Error::Empty));

        const StringView text[] = {"text"};
        REQUIRE(c.copy(Selection::Clipboard, Span<const StringView>(text), bytes("caf\xc3\xa9 \xe2\x82\xac"), "", 0)
                        .isOk());
        CHECK(readAs(c, Selection::Clipboard, "text/plain") == "caf\xe9 ?");

        // A way only a command takes: not yet.
        Clipboard html;
        Reported  reported;
        REQUIRE(html.configure("[Convert]\ntext/html>text=html2text\n", report, &reported).isOk());
        CHECK(reported.lines.empty());
        const StringView htmlType[] = {"text/html"};
        REQUIRE(html.copy(Selection::Clipboard, Span<const StringView>(htmlType), bytes("<b>x</b>"), "", 0).isOk());
        CHECK(readAs(html, Selection::Clipboard, "text") == e(Error::NotSupported));
}

TEST_CASE("/dev/clip0 is the clipboard's text as a file") {
        auto [c, cerr] = Clipboard::clipboard(0);
        REQUIRE(cerr.isOk());
        REQUIRE(c->clear(Selection::Clipboard, 0).isOk());
        REQUIRE(c->clear(Selection::Primary, 0).isOk());
        const Vfs::Context root;
        auto               write = [&](const char *path, uint8_t flags, const std::string &data) {
                auto [h, err] = Vfs::open(path, Vfs::Write | flags, root);
                REQUIRE(err.isOk());
                if (!data.empty()) REQUIRE(h->write(bytes(data), WaitForever).isOk());
        };
        auto read = [&](const char *path) {
                auto [h, err] = Vfs::open(path, Vfs::Read, root);
                REQUIRE(err.isOk());
                return drain(std::move(h));
        };

        CHECK(read("/dev/clip0") == ""); // empty: an empty file
        auto [info, ierr] = Vfs::stat("/dev/clip0", root);
        REQUIRE(ierr.isOk());
        CHECK(info.type == bmds::FileType::File);
        CHECK(info.size == 0);

        write("/dev/clip0", Vfs::Create | Vfs::Truncate, "one\n"); // `>`
        CHECK(read("/dev/clip0") == "one\n");
        CHECK(Vfs::stat("/dev/clip0", root).value().size == 4);
        Clipboard::Description d;
        c->describe(Selection::Clipboard, d);
        CHECK(std::string(d.owner) == "/dev/clip0");
        write("/dev/clip0", Vfs::Append, "two\n"); // `>>`
        CHECK(read("/dev/clip0") == "one\ntwo\n");
        CHECK(read("/dev/clip0p") == "");

        // vi's :w!: no O_TRUNC, written from the start, then cut to the length written.
        {
                auto [h, err] = Vfs::open("/dev/clip0", Vfs::Write | Vfs::Create, root);
                REQUIRE(err.isOk());
                REQUIRE(h->write(bytes("vi"), WaitForever).isOk());
                CHECK(h->stream().truncate(2) == Error::Ok);
                CHECK(read("/dev/clip0") == "one\ntwo\n"); // not until it closes
        }
        CHECK(read("/dev/clip0") == "vi");

        // vi's :r: the size from the open file (it seeks), then that many bytes.
        {
                auto [h, err] = Vfs::open("/dev/clip0", Vfs::Read, root);
                REQUIRE(err.isOk());
                CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::End).value() == 2);
                CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::Start).value() == 0);
                CHECK(drain(std::move(h)) == "vi");
        }

        // Opened for writing and left alone: unchanged; emptied: cleared.
        const uint32_t before = [&] {
                c->describe(Selection::Clipboard, d);
                return d.serial;
        }();
        write("/dev/clip0", 0, "");
        c->describe(Selection::Clipboard, d);
        CHECK(d.serial == before);
        write("/dev/clip0", Vfs::Truncate, "");
        c->describe(Selection::Clipboard, d);
        CHECK(d.serial == 0);

        // Text it converts to; none for what it can't.
        const StringView uris[] = {"text/uri-list"};
        REQUIRE(c->copy(Selection::Primary, Span<const StringView>(uris), bytes("file:///a\n"), "", 0).isOk());
        CHECK(read("/dev/clip0p") == "/a");
        CHECK(Vfs::stat("/dev/clip0p", root).value().size == 0); // not stored as text
        const StringView png[] = {"image/png"};
        REQUIRE(c->copy(Selection::Primary, Span<const StringView>(png), bytes("\x89PNG"), "", 0).isOk());
        CHECK(read("/dev/clip0p") == "");
        CHECK(Vfs::open("/dev/clip0", Vfs::Write | Vfs::Create | Vfs::Exclusive, root).error() == Error::Exists);
        CHECK(Vfs::remove("/dev/clip0", root) == Error::PermissionDenied);
        REQUIRE(c->clear(Selection::Primary, 0).isOk());
}

namespace {

        constexpr bmds::Command ClipCommands[] = {
                {"cat", "[file]...", "", bmds::TextCommands::cat},
                {"clip", bmds::ClipCommand::Usage, bmds::ClipCommand::Help, bmds::ClipCommand::run},
                {"echo", "[-neE] [word]...", "", bmds::TextCommands::echo},
        };

        struct ClipFixture {
                        ClipFixture() {
                                auto [c, err] = Clipboard::clipboard(0);
                                REQUIRE(err.isOk());
                                clipboard = c;
                                REQUIRE(c->clear(Selection::Clipboard, 0).isOk());
                                REQUIRE(c->clear(Selection::Primary, 0).isOk());
                                shell = std::make_unique<Shell>(con, Span<const bmds::Command>(ClipCommands), 1 << 20);
                                REQUIRE(shell->init().isOk());
                        }

                        ~ClipFixture() {
                                (void)clipboard->clear(Selection::Clipboard, 0);
                                (void)clipboard->clear(Selection::Primary, 0);
                        }

                        std::string out(const char *line, int expectStatus = Shell::ExitSuccess) {
                                CHECK_MESSAGE(shell->execute(line) == expectStatus, line);
                                return con.take();
                        }

                        Clipboard             *clipboard = nullptr;
                        FakeConsole            con;
                        std::unique_ptr<Shell> shell;
        };

} // namespace

TEST_CASE("clip copies, pastes, lists and clears") {
        ClipFixture f;
        CHECK(f.out("clip", 1) == "clip: the clipboard is empty\n");
        CHECK(f.out("clip list") == "Clipboard: empty\nPrimary: empty\n");
        CHECK(f.out("echo hello | clip copy") == "");
        CHECK(f.out("clip") == "hello\n");
        CHECK(f.out("clip paste -t text/plain") == "hello\n");
        CHECK(f.out("clip list") == "Clipboard: from clip\n"
                                    "  text/plain;charset=utf-8       6 bytes\n"
                                    "  also as: text/plain\n"
                                    "Primary: empty\n");
        CHECK(f.out("clip -t image/png", 1) ==
              "clip: the clipboard can't be read as image/png ('clip list' shows what it can)\n");

        // The primary selection; several types; a type by the file's name.
        CHECK(f.out("echo -n sel | clip copy -p -t text -ttext/x-mine") == "");
        CHECK(f.out("clip -p -t text/x-mine") == "sel");
        CHECK(f.out("clip list -p") == "Primary: from clip\n"
                                       "  text/plain;charset=utf-8       3 bytes\n"
                                       "  text/x-mine                    3 bytes\n"
                                       "  also as: text/plain\n");
        CHECK(f.out("clip") == "hello\n");
        CHECK(f.out("echo '<p>' > /dev/clip0p; clip copy /dev/clip0p; clip list -p") ==
              "Primary: from /dev/clip0p\n"
              "  text/plain;charset=utf-8       4 bytes\n"
              "  also as: text/plain\n");

        // The files and the command agree.
        CHECK(f.out("cat /dev/clip0") == "<p>\n");
        CHECK(f.out("echo again > /dev/clip0; clip") == "again\n");
        CHECK(f.out("clip clear; clip", 1) == "clip: the clipboard is empty\n");
        CHECK(f.out("clip clear -p; clip list") == "Clipboard: empty\nPrimary: empty\n");

        // Usage errors.
        CHECK(f.out("clip bogus", Shell::ExitUsage).find("usage: clip") == 0);
        CHECK(f.out("clip -x", Shell::ExitUsage).find("usage: clip") == 0);
        CHECK(f.out("clip -t", Shell::ExitUsage).find("usage: clip") == 0);
        CHECK(f.out("clip -t a/b -t c/d", Shell::ExitUsage).find("usage: clip") == 0);
        CHECK(f.out("clip list -t text", Shell::ExitUsage).find("usage: clip") == 0);
        CHECK(f.out("clip copy a b", Shell::ExitUsage).find("usage: clip") == 0);
        CHECK(f.out("clip copy -t nope", Shell::ExitUsage) ==
              "clip: not a type: nope\nusage: clip " + std::string(bmds::ClipCommand::Usage) + "\n");
        CHECK(f.out("clip copy /no/such", 1) == "clip: /no/such: No such file or directory\n");
}
