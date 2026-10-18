/**
 * @file      json.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * JSON (devplan/json.md): JsonReader against JSONTestSuite, whole and a byte at a time; tokens
 * and values; JsonWriter's output, its errors, and the reader reading it back.
 */

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <bmds/jsonreader.h>
#include <bmds/jsonwriter.h>

using bmds::Error;
using bmds::JsonReader;
using bmds::JsonWriter;
using bmds::StringView;
using Token = JsonReader::Token;

namespace {

        const std::string SuiteDir = std::string(BMDS_ROOT_DIR) + "/thirdparty/jsontestsuite/test_parsing";

        std::string readFile(const std::string &path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        std::vector<std::string> suiteFiles() {
                std::vector<std::string> names;
                DIR                     *dir = opendir(SuiteDir.c_str());
                REQUIRE(dir != nullptr);
                while (dirent *e = readdir(dir)) {
                        const std::string name = e->d_name;
                        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) names.push_back(name);
                }
                closedir(dir);
                std::sort(names.begin(), names.end());
                return names;
        }

        // Reads a document to its end: Ok, or the error.
        Error readAll(JsonReader &json) {
                for (;;) {
                        const Token t = json.next();
                        if (t == Token::End) return Error::Ok;
                        if (t == Token::Error) return json.error();
                }
        }

        Error readWhole(const std::string &text) {
                static char scratch[64 * 1024];
                JsonReader  json(StringView(text.data(), text.size()), scratch, sizeof(scratch));
                return readAll(json);
        }

        // A source handing out at most `step` bytes a call.
        struct Feed {
                        const std::string *text;
                        size_t             pos = 0;
                        size_t             step = 1;
        };

        bmds::Result<size_t> feed(void *arg, char *buf, size_t bytes) {
                Feed        *f = static_cast<Feed *>(arg);
                const size_t n = std::min({bytes, f->step, f->text->size() - f->pos});
                std::copy_n(f->text->data() + f->pos, n, buf);
                f->pos += n;
                return bmds::Result<size_t>(n);
        }

        Error readStreamed(const std::string &text, size_t step) {
                static char scratch[64 * 1024];
                char        buffer[7];
                Feed        f{&text, 0, step};
                JsonReader  json(feed, &f, buffer, sizeof(buffer), scratch, sizeof(scratch));
                return readAll(json);
        }

        // The tokens of a document as text, for comparing: values with their text.
        std::string tokens(const std::string &text) {
                char        scratch[256];
                JsonReader  json(StringView(text.data(), text.size()), scratch, sizeof(scratch));
                std::string out;
                for (;;) {
                        const Token t = json.next();
                        switch (t) {
                                case Token::BeginObject: out += "{ "; break;
                                case Token::EndObject: out += "} "; break;
                                case Token::BeginArray: out += "[ "; break;
                                case Token::EndArray: out += "] "; break;
                                case Token::Key:
                                        out += "K:" + std::string(json.text().data(), json.text().size()) + " ";
                                        break;
                                case Token::String:
                                        out += "S:" + std::string(json.text().data(), json.text().size()) + " ";
                                        break;
                                case Token::Number:
                                        out += "N:" + std::string(json.text().data(), json.text().size()) + " ";
                                        break;
                                case Token::True: out += "true "; break;
                                case Token::False: out += "false "; break;
                                case Token::Null: out += "null "; break;
                                case Token::End: return out + "end";
                                case Token::Error: return out + "error:" + json.error().name();
                        }
                }
        }

        // The implementation-defined documents JsonReader accepts (numbers past a double's or an
        // integer's range: the caller's conversion says so); it rejects the rest (invalid UTF-8,
        // lone surrogates, a byte-order mark, UTF-16, 500 levels deep).
        const std::set<std::string> AcceptedI = {
                "i_number_double_huge_neg_exp.json", "i_number_huge_exp.json",
                "i_number_neg_int_huge_exp.json",    "i_number_pos_double_huge_exp.json",
                "i_number_real_neg_overflow.json",   "i_number_real_pos_overflow.json",
                "i_number_real_underflow.json",      "i_number_too_big_neg_int.json",
                "i_number_too_big_pos_int.json",     "i_number_very_big_negative_int.json",
        };

} // namespace

TEST_CASE("JsonReader: JSONTestSuite, whole and a byte at a time") {
        const std::vector<std::string> names = suiteFiles();
        CHECK(names.size() == 318);
        for (const std::string &name : names) {
                CAPTURE(name);
                const std::string text = readFile(SuiteDir + "/" + name);
                const Error       whole = readWhole(text);
                if (name[0] == 'y') {
                        CHECK(whole.isOk());
                } else if (name[0] == 'n') {
                        CHECK(whole.isError());
                } else {
                        CHECK(whole.isOk() == (AcceptedI.count(name) == 1));
                }
                // Streamed in pieces of 1 and 3 bytes through a 7-byte buffer: the same outcome.
                CHECK(readStreamed(text, 1) == whole);
                CHECK(readStreamed(text, 3) == whole);
        }
}

TEST_CASE("JsonReader: tokens and decoded strings") {
        CHECK(tokens(R"({"a": [1, -2.5e3, true, false, null], "b": {}, "c": []})") ==
              "{ K:a [ N:1 N:-2.5e3 true false null ] K:b { } K:c [ ] } end");
        CHECK(tokens(" \t\r\n\"top\" \n") == "S:top end");
        CHECK(tokens(R"(["\"\\\/\b\f\n\r\t"])") == "[ S:\"\\/\b\f\n\r\t ] end");
        // \u escapes into UTF-8, a surrogate pair into one character.
        CHECK(tokens(R"(["\u00e9\u20ac\ud83d\ude00"])") == "[ S:\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 ] end");
        CHECK(tokens("[\"\xc3\xa9\"]") == "[ S:\xc3\xa9 ] end");
        // A NUL inside a string is kept.
        char       scratch[16];
        JsonReader json(StringView(R"("a\u0000b")"), scratch, sizeof(scratch));
        CHECK(json.next() == Token::String);
        CHECK(json.text().size() == 3);
        CHECK(json.text()[1] == '\0');
}

TEST_CASE("JsonReader: errors and where they are") {
        CHECK(tokens("[1,]") == "[ N:1 error:CorruptData");
        CHECK(tokens("{\"a\" 1}") == "{ K:a error:CorruptData");
        CHECK(tokens("[01]") == "[ N:0 error:CorruptData");
        CHECK(tokens("[\"\\ud800\"]") == "[ error:CorruptData");
        CHECK(tokens("[\"\xc0\xaf\"]") == "[ error:CorruptData");     // overlong
        CHECK(tokens("[\"\xed\xa0\x80\"]") == "[ error:CorruptData"); // a surrogate in UTF-8
        CHECK(tokens("1 2") == "N:1 error:CorruptData");
        CHECK(tokens("") == "error:CorruptData");
        char       scratch[64];
        JsonReader json(StringView("{\n  \"a\": tru\n}"), scratch, sizeof(scratch));
        CHECK(json.next() == Token::BeginObject);
        CHECK(json.next() == Token::Key);
        CHECK(json.next() == Token::Error);
        CHECK(json.line() == 2);
        CHECK(json.column() == 11);         // the newline after `tru`
        CHECK(json.next() == Token::Error); // it stays failed
        // Too long for the scratch; too deep.
        char       small[4];
        JsonReader longString(StringView("\"abcd\""), small, sizeof(small));
        CHECK(longString.next() == Token::Error);
        CHECK(longString.error() == Error::BufferTooSmall);
        const std::string deep = std::string(65, '[') + std::string(65, ']');
        CHECK(readWhole(deep) == Error::OutOfRange);
        const std::string ok = std::string(64, '[') + std::string(64, ']');
        CHECK(readWhole(ok).isOk());
}

TEST_CASE("JsonReader: numbers, exact and otherwise") {
        char scratch[64];
        struct Case {
                        const char *text;
                        Error       asInt;
                        int64_t     value;
        };
        const Case cases[] = {
                {"0", Error::Ok, 0},
                {"-0", Error::Ok, 0},
                {"9223372036854775807", Error::Ok, INT64_MAX},
                {"-9223372036854775808", Error::Ok, INT64_MIN},
                {"9223372036854775808", Error::OutOfRange, 0},
                {"-9223372036854775809", Error::OutOfRange, 0},
                {"1.5", Error::Invalid, 0},
                {"1e3", Error::Invalid, 0},
        };
        for (const Case &c : cases) {
                CAPTURE(c.text);
                JsonReader json(StringView(c.text), scratch, sizeof(scratch));
                REQUIRE(json.next() == Token::Number);
                int64_t v = 0;
                CHECK(json.toInt64(v) == c.asInt);
                if (c.asInt.isOk()) CHECK(v == c.value);
        }
        JsonReader big(StringView("18446744073709551615"), scratch, sizeof(scratch));
        REQUIRE(big.next() == Token::Number);
        uint64_t u = 0;
        CHECK(big.toUint64(u).isOk());
        CHECK(u == UINT64_MAX);
        JsonReader negative(StringView("-1"), scratch, sizeof(scratch));
        REQUIRE(negative.next() == Token::Number);
        CHECK(negative.toUint64(u) == Error::OutOfRange);
        JsonReader real(StringView("-2.5e-3"), scratch, sizeof(scratch));
        REQUIRE(real.next() == Token::Number);
        double d = 0;
        float  f = 0;
        CHECK(real.toDouble(d).isOk());
        CHECK(d == -2.5e-3);
        CHECK(real.toFloat(f).isOk());
        CHECK(f == -2.5e-3f);
        // A string is not a number.
        JsonReader text(StringView("\"12\""), scratch, sizeof(scratch));
        REQUIRE(text.next() == Token::String);
        int64_t v = 0;
        CHECK(text.toInt64(v) == Error::Invalid);
}

TEST_CASE("JsonReader: skip passes over a value") {
        char       scratch[64];
        JsonReader json(StringView(R"({"skip": {"a": [1, {"b": 2}], "c": "d"}, "keep": 3})"), scratch, sizeof(scratch));
        CHECK(json.next() == Token::BeginObject);
        CHECK(json.next() == Token::Key);
        CHECK(json.next() == Token::BeginObject);
        CHECK(json.skip().isOk());
        CHECK(json.depth() == 1);
        CHECK(json.next() == Token::Key);
        CHECK(json.text() == StringView("keep"));
        CHECK(json.next() == Token::Number);
        CHECK(json.skip().isOk()); // nothing to pass over
        CHECK(json.next() == Token::EndObject);
        CHECK(json.next() == Token::End);
        CHECK(json.next() == Token::End);
}

TEST_CASE("JsonWriter: compact and indented") {
        bmds::String out(bmds::MemSpace::Psram);
        JsonWriter   json(out);
        json.beginObject();
        json.member("format", 1);
        json.member("name", "storage.sd.read");
        json.member("noisy", false);
        json.member("median", 21.3);
        json.key("empty");
        json.beginArray();
        json.endArray();
        json.key("list");
        json.beginArray();
        json.value(-1);
        json.value(UINT64_MAX);
        json.value(INT64_MIN);
        json.null();
        json.value(std::nan(""));
        json.endArray();
        json.endObject();
        CHECK(json.finish().isOk());
        CHECK(std::string(out.data(), out.size()) ==
              R"({"format":1,"name":"storage.sd.read","noisy":false,"median":21.3,"empty":[],)"
              R"("list":[-1,18446744073709551615,-9223372036854775808,null,null]})");

        bmds::String pretty(bmds::MemSpace::Psram);
        JsonWriter   indented(pretty);
        indented.setIndent(2);
        indented.beginObject();
        indented.member("a", 1);
        indented.key("b");
        indented.beginArray();
        indented.value(true);
        indented.beginObject();
        indented.endObject();
        indented.endArray();
        indented.endObject();
        CHECK(indented.finish().isOk());
        CHECK(std::string(pretty.data(), pretty.size()) == "{\n  \"a\": 1,\n  \"b\": [\n    true,\n    {}\n  ]\n}");
}

TEST_CASE("JsonWriter: strings escaped, and read back the same") {
        const std::string tricky = std::string("q\"b\\s/\b\f\n\r\t\x01\x1f") + '\0' + "\xc3\xa9\xf0\x9f\x98\x80";
        bmds::String      out(bmds::MemSpace::Psram);
        JsonWriter        json(out);
        json.beginArray();
        json.value(StringView(tricky.data(), tricky.size()));
        json.value(0.1);
        json.value(1e300);
        json.value(-0.0);
        json.value(123456789.125);
        json.endArray();
        REQUIRE(json.finish().isOk());
        CHECK(std::string(out.data(), out.size()) ==
              "[\"q\\\"b\\\\s/"
              "\\b\\f\\n\\r\\t\\u0001\\u001f\\u0000\xc3\xa9\xf0\x9f\x98\x80\",0.1,1e+300,-0,123456789.125]");
        char       scratch[64];
        JsonReader back(StringView(out.data(), out.size()), scratch, sizeof(scratch));
        CHECK(back.next() == Token::BeginArray);
        REQUIRE(back.next() == Token::String);
        CHECK(std::string(back.text().data(), back.text().size()) == tricky);
        for (double expected : {0.1, 1e300, -0.0, 123456789.125}) {
                REQUIRE(back.next() == Token::Number);
                double d = 1;
                CHECK(back.toDouble(d).isOk());
                CHECK(d == expected);
        }
        CHECK(back.next() == Token::EndArray);
        CHECK(back.next() == Token::End);
}

TEST_CASE("JsonWriter: misuse and the sink's errors stop the writing") {
        bmds::String out(bmds::MemSpace::Psram);
        {
                JsonWriter json(out);
                json.beginObject();
                json.value(1); // a value where a key belongs
                CHECK(json.finish() == Error::InvalidState);
        }
        {
                JsonWriter json(out);
                json.beginArray();
                json.endObject(); // the wrong end
                CHECK(json.finish() == Error::InvalidState);
        }
        {
                JsonWriter json(out);
                json.beginArray();
                CHECK(json.finish() == Error::Invalid); // left open
        }
        {
                JsonWriter json(out);
                json.value(1);
                json.value(2); // two values at the top
                CHECK(json.finish() == Error::InvalidState);
        }
        {
                JsonWriter json(out);
                for (size_t i = 0; i < JsonWriter::MaxDepth; ++i) json.beginArray();
                CHECK(json.error().isOk());
                json.beginArray();
                CHECK(json.error() == Error::OutOfRange);
        }
        // A sink that fails after the first buffer: the error stands, nothing more is written.
        struct Sink {
                        size_t calls = 0;
                        size_t bytes = 0;
        } sink;
        JsonWriter json(
                [](void *arg, StringView bytes) -> Error {
                        Sink *s = static_cast<Sink *>(arg);
                        ++s->calls;
                        s->bytes += bytes.size();
                        return s->calls == 1 ? Error::Ok : Error::NoSpace;
                },
                &sink);
        json.beginArray();
        for (int i = 0; i < 200; ++i) json.value("some text that fills the buffer");
        json.endArray();
        CHECK(json.finish() == Error::NoSpace);
        CHECK(sink.calls == 2);
        CHECK(sink.bytes == 256);
}
