/**
 * @file      string.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The script's `string`: a value type holding bytes (UTF-8 as it comes; length() counts
 * bytes, as C++'s std::string does), with concatenation of numbers, searching, splitting
 * and case. Also `array<T>` (AngelScript's add-on, registered here: split() returns one).
 */

#include <ctype.h>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

namespace angel {

        // ---- String

        String::String(const char *data, size_t length) {
                assign(data, length);
        }

        String::String(const char *text) {
                assign(text, strlen(text));
        }

        String::String(const String &other) {
                assign(other.data(), other.length());
        }

        String::~String() {
                free(_data);
        }

        String &String::operator=(const String &other) {
                if (this != &other) assign(other.data(), other.length());
                return *this;
        }

        bool String::reserve(size_t length) {
                if (length + 1 <= _capacity) return true;
                if (length >= 0x7fffffff) return false;
                size_t capacity = _capacity != 0 ? _capacity : 16;
                while (capacity < length + 1) capacity *= 2;
                char *grown = static_cast<char *>(realloc(_data, capacity));
                if (grown == nullptr) return false;
                _data = grown;
                _capacity = uint32_t(capacity);
                return true;
        }

        void String::assign(const char *data, size_t length) {
                if (!reserve(length)) {
                        raise("Out of memory for a string of %zu bytes", length);
                        return;
                }
                if (length != 0) memmove(_data, data, length);
                _length = uint32_t(length);
                if (_data != nullptr) _data[_length] = 0;
                return;
        }

        void String::append(const char *data, size_t length) {
                if (length == 0) return;
                if (!reserve(size_t(_length) + length)) {
                        raise("Out of memory for a string of %zu bytes", size_t(_length) + length);
                        return;
                }
                memmove(_data + _length, data, length);
                _length += uint32_t(length);
                _data[_length] = 0;
                return;
        }

        void String::adopt(char *data, size_t length) {
                free(_data);
                _data = data;
                _length = uint32_t(length);
                _capacity = data != nullptr ? uint32_t(length + 1) : 0;
                return;
        }

        namespace {

                asITypeInfo *arrayOfString = nullptr;

                // Literals: one String each, made when a script is compiled.
                class StringFactory : public asIStringFactory {
                        public:
                                const void *GetStringConstant(const char *data, asUINT length) override {
                                        return new String(data, length);
                                }
                                int ReleaseStringConstant(const void *str) override {
                                        delete static_cast<const String *>(str);
                                        return asSUCCESS;
                                }
                                int GetRawStringData(const void *str, char *data, asUINT *length) const override {
                                        const String *s = static_cast<const String *>(str);
                                        if (length != nullptr) *length = s->length();
                                        if (data != nullptr) memcpy(data, s->data(), s->length());
                                        return asSUCCESS;
                                }
                };

                StringFactory factory;

                String *self(asIScriptGeneric *gen) {
                        return static_cast<String *>(gen->GetObject());
                }

                const String &arg(asIScriptGeneric *gen, asUINT n) {
                        return *static_cast<const String *>(gen->GetArgAddress(n));
                }

                void returnString(asIScriptGeneric *gen, const char *data, size_t length) {
                        new (gen->GetAddressOfReturnLocation()) String(data, length);
                        return;
                }

                // Numbers as text, as print() and concatenation show them.
                void appendInt(Text &out, int64_t v) {
                        out.appendf("%lld", (long long)v);
                }
                void appendUInt(Text &out, uint64_t v) {
                        out.appendf("%llu", (unsigned long long)v);
                }

        } // namespace

        void appendFloat(Text &out, float v) {
                out.appendf("%.7g", double(v));
                return;
        }

        void appendDouble(Text &out, double v) {
                out.appendf("%.15g", v);
                return;
        }

        namespace {

                // ---- Behaviours and operators

                void construct(asIScriptGeneric *gen) {
                        new (gen->GetObject()) String();
                }
                void copyConstruct(asIScriptGeneric *gen) {
                        new (gen->GetObject()) String(arg(gen, 0));
                }
                void destruct(asIScriptGeneric *gen) {
                        self(gen)->~String();
                }
                void assign(asIScriptGeneric *gen) {
                        *self(gen) = arg(gen, 0);
                        gen->SetReturnAddress(self(gen));
                }
                void addAssign(asIScriptGeneric *gen) {
                        const String &other = arg(gen, 0);
                        if (&other == self(gen)) {
                                const String copy(other);
                                self(gen)->append(copy.data(), copy.length());
                        } else
                                self(gen)->append(other.data(), other.length());
                        gen->SetReturnAddress(self(gen));
                }

                // Appends the number argument 0 of @p gen (by the kind in the declaration).
                enum class Kind {
                        Int,
                        UInt,
                        Double,
                        Float,
                        Bool
                };
                void appendArg(asIScriptGeneric *gen, Kind kind, Text &out) {
                        switch (kind) {
                                case Kind::Int: appendInt(out, int64_t(gen->GetArgQWord(0))); break;
                                case Kind::UInt: appendUInt(out, gen->GetArgQWord(0)); break;
                                case Kind::Double: appendDouble(out, gen->GetArgDouble(0)); break;
                                case Kind::Float: appendFloat(out, gen->GetArgFloat(0)); break;
                                case Kind::Bool: out.append(gen->GetArgByte(0) != 0 ? "true" : "false"); break;
                        }
                        return;
                }
                template <Kind K> void addAssignNumber(asIScriptGeneric *gen) {
                        Text text;
                        appendArg(gen, K, text);
                        self(gen)->append(text.data(), text.length());
                        gen->SetReturnAddress(self(gen));
                }
                template <Kind K> void addNumber(asIScriptGeneric *gen) {
                        Text text;
                        text.append(self(gen)->data(), self(gen)->length());
                        appendArg(gen, K, text);
                        returnString(gen, text.data(), text.length());
                }
                template <Kind K> void addNumberReversed(asIScriptGeneric *gen) {
                        Text text;
                        appendArg(gen, K, text);
                        text.append(self(gen)->data(), self(gen)->length());
                        returnString(gen, text.data(), text.length());
                }
                void add(asIScriptGeneric *gen) {
                        const String &a = *self(gen), &b = arg(gen, 0);
                        String       *r = new (gen->GetAddressOfReturnLocation()) String();
                        r->append(a.data(), a.length());
                        r->append(b.data(), b.length());
                }
                int compare(const String &a, const String &b) {
                        const uint32_t n = a.length() < b.length() ? a.length() : b.length();
                        const int      c = memcmp(a.data(), b.data(), n);
                        if (c != 0) return c < 0 ? -1 : 1;
                        return a.length() < b.length() ? -1 : a.length() > b.length() ? 1 : 0;
                }
                void equals(asIScriptGeneric *gen) {
                        gen->SetReturnByte(compare(*self(gen), arg(gen, 0)) == 0);
                }
                void cmp(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(compare(*self(gen), arg(gen, 0))));
                }
                void index(asIScriptGeneric *gen) {
                        String        *s = self(gen);
                        const uint32_t i = gen->GetArgDWord(0);
                        if (i >= s->length()) {
                                raise("Index %u is past the string's end (its length is %u)", i, s->length());
                                return;
                        }
                        gen->SetReturnAddress(s->mutableData() + i);
                }

                // ---- Methods

                void length(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->length());
                }
                void isEmpty(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->length() == 0);
                }
                // A start and count cut to the string (a negative count: to the end).
                void clamp(const String &s, int start, int count, uint32_t &from, uint32_t &n) {
                        if (start < 0) start = 0;
                        from = uint32_t(start) > s.length() ? s.length() : uint32_t(start);
                        const uint32_t left = s.length() - from;
                        n = count < 0 || uint32_t(count) > left ? left : uint32_t(count);
                        return;
                }
                void substr(asIScriptGeneric *gen) {
                        const String &s = *self(gen);
                        uint32_t      from, n;
                        clamp(s, int(gen->GetArgDWord(0)), int(gen->GetArgDWord(1)), from, n);
                        returnString(gen, s.data() + from, n);
                }
                int findIn(const String &s, const String &what, int start) {
                        if (start < 0) start = 0;
                        if (uint32_t(start) > s.length() || what.length() > s.length() - uint32_t(start)) return -1;
                        if (what.length() == 0) return start;
                        for (uint32_t i = uint32_t(start); i + what.length() <= s.length(); ++i)
                                if (memcmp(s.data() + i, what.data(), what.length()) == 0) return int(i);
                        return -1;
                }
                void find(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(findIn(*self(gen), arg(gen, 0), int(gen->GetArgDWord(1)))));
                }
                void findLast(asIScriptGeneric *gen) {
                        const String &s = *self(gen), &what = arg(gen, 0);
                        int           start = int(gen->GetArgDWord(1));
                        int           found = -1;
                        if (what.length() <= s.length()) {
                                int last = int(s.length() - what.length());
                                if (start >= 0 && start < last) last = start;
                                for (int i = last; i >= 0; --i)
                                        if (memcmp(s.data() + i, what.data(), what.length()) == 0) {
                                                found = i;
                                                break;
                                        }
                        }
                        gen->SetReturnDWord(asDWORD(found));
                }
                void contains(asIScriptGeneric *gen) {
                        gen->SetReturnByte(findIn(*self(gen), arg(gen, 0), 0) >= 0);
                }
                void startsWith(asIScriptGeneric *gen) {
                        const String &s = *self(gen), &what = arg(gen, 0);
                        gen->SetReturnByte(what.length() <= s.length() &&
                                           memcmp(s.data(), what.data(), what.length()) == 0);
                }
                void endsWith(asIScriptGeneric *gen) {
                        const String &s = *self(gen), &what = arg(gen, 0);
                        gen->SetReturnByte(what.length() <= s.length() && memcmp(s.data() + s.length() - what.length(),
                                                                                 what.data(), what.length()) == 0);
                }
                void replace(asIScriptGeneric *gen) {
                        const String &s = *self(gen), &from = arg(gen, 0), &to = arg(gen, 1);
                        Text          out;
                        if (from.length() == 0) {
                                returnString(gen, s.data(), s.length());
                                return;
                        }
                        uint32_t at = 0;
                        for (;;) {
                                const int found = findIn(s, from, int(at));
                                if (found < 0) break;
                                out.append(s.data() + at, uint32_t(found) - at);
                                out.append(to.data(), to.length());
                                at = uint32_t(found) + from.length();
                        }
                        out.append(s.data() + at, s.length() - at);
                        returnString(gen, out.data(), out.length());
                }
                template <int (*Convert)(int)> void changeCase(asIScriptGeneric *gen) {
                        const String &s = *self(gen);
                        String       *r = new (gen->GetAddressOfReturnLocation()) String(s);
                        char         *p = r->mutableData();
                        for (uint32_t i = 0; i < r->length(); ++i)
                                p[i] = char(Convert(static_cast<unsigned char>(p[i])));
                }
                bool isBlank(char c) {
                        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
                }
                void trim(asIScriptGeneric *gen) {
                        const String &s = *self(gen);
                        uint32_t      from = 0, to = s.length();
                        while (from < to && isBlank(s.data()[from])) ++from;
                        while (to > from && isBlank(s.data()[to - 1])) --to;
                        returnString(gen, s.data() + from, to - from);
                }
                CScriptArray *newStringArray() {
                        return CScriptArray::Create(arrayOfString);
                }
                void pushString(CScriptArray *array, const char *data, size_t length) {
                        String piece(data, length);
                        array->InsertLast(&piece);
                        return;
                }
                void split(asIScriptGeneric *gen) {
                        const String &s = *self(gen), &delimiter = arg(gen, 0);
                        CScriptArray *array = newStringArray();
                        if (array == nullptr) return;
                        if (delimiter.length() == 0) {
                                // Each character (byte) on its own.
                                for (uint32_t i = 0; i < s.length(); ++i) pushString(array, s.data() + i, 1);
                        } else {
                                uint32_t at = 0;
                                for (;;) {
                                        const int found = findIn(s, delimiter, int(at));
                                        if (found < 0) break;
                                        pushString(array, s.data() + at, uint32_t(found) - at);
                                        at = uint32_t(found) + delimiter.length();
                                }
                                pushString(array, s.data() + at, s.length() - at);
                        }
                        gen->SetReturnAddress(array);
                }
                void splitWords(asIScriptGeneric *gen) {
                        const String &s = *self(gen);
                        CScriptArray *array = newStringArray();
                        if (array == nullptr) return;
                        uint32_t i = 0;
                        while (i < s.length()) {
                                while (i < s.length() && isBlank(s.data()[i])) ++i;
                                const uint32_t start = i;
                                while (i < s.length() && !isBlank(s.data()[i])) ++i;
                                if (i > start) pushString(array, s.data() + start, i - start);
                        }
                        gen->SetReturnAddress(array);
                }
                void insert(asIScriptGeneric *gen) {
                        String        *s = self(gen);
                        const uint32_t at = gen->GetArgDWord(0);
                        const String  &what = arg(gen, 1);
                        if (at > s->length()) {
                                raise("Position %u is past the string's end (its length is %u)", at, s->length());
                                return;
                        }
                        Text out;
                        out.append(s->data(), at);
                        out.append(what.data(), what.length());
                        out.append(s->data() + at, s->length() - at);
                        s->assign(out.data(), out.length());
                }
                void erase(asIScriptGeneric *gen) {
                        String  *s = self(gen);
                        uint32_t from, n;
                        clamp(*s, int(gen->GetArgDWord(0)), int(gen->GetArgDWord(1)), from, n);
                        Text out;
                        out.append(s->data(), from);
                        out.append(s->data() + from + n, s->length() - from - n);
                        s->assign(out.data(), out.length());
                }

                // ---- Global functions

                void join(asIScriptGeneric *gen) {
                        const CScriptArray *array = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        const String       &delimiter = arg(gen, 1);
                        Text                out;
                        for (asUINT i = 0; i < array->GetSize(); ++i) {
                                if (i != 0) out.append(delimiter.data(), delimiter.length());
                                const String *piece = static_cast<const String *>(array->At(i));
                                out.append(piece->data(), piece->length());
                        }
                        returnString(gen, out.data(), out.length());
                }
                void parseInt(asIScriptGeneric *gen) {
                        const String  &s = arg(gen, 0);
                        const uint32_t base = gen->GetArgDWord(1);
                        if (base == 1 || base > 36) {
                                raise("parseInt: base %u isn't 2 to 36 (or 0: as written)", base);
                                return;
                        }
                        gen->SetReturnQWord(asQWORD(strtoll(s.cStr(), nullptr, int(base))));
                }
                void parseFloat(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(strtof(arg(gen, 0).cStr(), nullptr));
                }
                void parseDouble(asIScriptGeneric *gen) {
                        gen->SetReturnDouble(strtod(arg(gen, 0).cStr(), nullptr));
                }
                void formatFloat(asIScriptGeneric *gen) {
                        const double   value = gen->GetArgDouble(0);
                        const uint32_t decimals = gen->GetArgDWord(1);
                        Text           out;
                        out.appendf("%.*f", int(decimals > 30 ? 30 : decimals), value);
                        returnString(gen, out.data(), out.length());
                }
                void chr(asIScriptGeneric *gen) {
                        uint32_t c = gen->GetArgDWord(0);
                        char     b[4];
                        size_t   n;
                        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
                        if (c < 0x80) {
                                b[0] = char(c);
                                n = 1;
                        } else if (c < 0x800) {
                                b[0] = char(0xc0 | (c >> 6));
                                b[1] = char(0x80 | (c & 0x3f));
                                n = 2;
                        } else if (c < 0x10000) {
                                b[0] = char(0xe0 | (c >> 12));
                                b[1] = char(0x80 | ((c >> 6) & 0x3f));
                                b[2] = char(0x80 | (c & 0x3f));
                                n = 3;
                        } else {
                                b[0] = char(0xf0 | (c >> 18));
                                b[1] = char(0x80 | ((c >> 12) & 0x3f));
                                b[2] = char(0x80 | ((c >> 6) & 0x3f));
                                b[3] = char(0x80 | (c & 0x3f));
                                n = 4;
                        }
                        returnString(gen, b, n);
                }
                void ord(asIScriptGeneric *gen) {
                        const String        &s = arg(gen, 0);
                        const unsigned char *p = reinterpret_cast<const unsigned char *>(s.data());
                        const uint32_t       n = s.length();
                        uint32_t             c = 0;
                        if (n == 0)
                                c = 0;
                        else if (p[0] < 0x80)
                                c = p[0];
                        else if ((p[0] & 0xe0) == 0xc0 && n >= 2)
                                c = (uint32_t(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
                        else if ((p[0] & 0xf0) == 0xe0 && n >= 3)
                                c = (uint32_t(p[0] & 0x0f) << 12) | (uint32_t(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
                        else if ((p[0] & 0xf8) == 0xf0 && n >= 4)
                                c = (uint32_t(p[0] & 0x07) << 18) | (uint32_t(p[1] & 0x3f) << 12) |
                                    (uint32_t(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
                        else
                                c = p[0];
                        gen->SetReturnDWord(c);
                }

        } // namespace

        asITypeInfo *stringArrayType(asIScriptEngine *engine) {
                if (arrayOfString == nullptr) arrayOfString = engine->GetTypeInfoByDecl("array<string>");
                return arrayOfString;
        }

        void registerString(asIScriptEngine *engine) {
                check(engine->RegisterObjectType("string", sizeof(String), asOBJ_VALUE | asOBJ_APP_CLASS_CDAK),
                      "string");
                check(engine->RegisterStringFactory("string", &factory), "the string factory");
                const char *S = "string";
                behaviour(engine, S, asBEHAVE_CONSTRUCT, "void f()", construct, "An empty string.");
                behaviour(engine, S, asBEHAVE_CONSTRUCT, "void f(const string &in)", copyConstruct, "A copy.");
                behaviour(engine, S, asBEHAVE_DESTRUCT, "void f()", destruct, nullptr);
                method(engine, S, "string &opAssign(const string &in)", assign, nullptr);
                method(engine, S, "string &opAddAssign(const string &in)", addAssign, "Appends a string.");
                method(engine, S, "string &opAddAssign(int64)", addAssignNumber<Kind::Int>, "Appends a number.");
                method(engine, S, "string &opAddAssign(uint64)", addAssignNumber<Kind::UInt>, "Appends a number.");
                method(engine, S, "string &opAddAssign(double)", addAssignNumber<Kind::Double>, "Appends a number.");
                method(engine, S, "string &opAddAssign(float)", addAssignNumber<Kind::Float>, "Appends a number.");
                method(engine, S, "string &opAddAssign(bool)", addAssignNumber<Kind::Bool>, "Appends true or false.");
                method(engine, S, "string opAdd(const string &in) const", add, "The two joined.");
                method(engine, S, "string opAdd(int64) const", addNumber<Kind::Int>, "The string and a number.");
                method(engine, S, "string opAdd_r(int64) const", addNumberReversed<Kind::Int>,
                       "A number and the string.");
                method(engine, S, "string opAdd(uint64) const", addNumber<Kind::UInt>, "The string and a number.");
                method(engine, S, "string opAdd_r(uint64) const", addNumberReversed<Kind::UInt>,
                       "A number and the string.");
                method(engine, S, "string opAdd(double) const", addNumber<Kind::Double>, "The string and a number.");
                method(engine, S, "string opAdd_r(double) const", addNumberReversed<Kind::Double>,
                       "A number and the string.");
                method(engine, S, "string opAdd(float) const", addNumber<Kind::Float>, "The string and a number.");
                method(engine, S, "string opAdd_r(float) const", addNumberReversed<Kind::Float>,
                       "A number and the string.");
                method(engine, S, "string opAdd(bool) const", addNumber<Kind::Bool>, "The string and true or false.");
                method(engine, S, "string opAdd_r(bool) const", addNumberReversed<Kind::Bool>,
                       "true or false and the string.");
                method(engine, S, "bool opEquals(const string &in) const", equals, "The same bytes.");
                method(engine, S, "int opCmp(const string &in) const", cmp, "Ordered byte by byte.");
                method(engine, S, "uint8 &opIndex(uint)", index, "The byte at a position (from 0).");
                method(engine, S, "const uint8 &opIndex(uint) const", index, "The byte at a position (from 0).");
                method(engine, S, "uint length() const", length, "The number of bytes.");
                method(engine, S, "bool isEmpty() const", isEmpty, "No bytes at all.");
                method(engine, S, "string substr(int start, int count = -1) const", substr,
                       "Part of it: count bytes from start (-1: to the end).");
                method(engine, S, "int find(const string &in, int start = 0) const", find,
                       "Where a string first appears from start, or -1.");
                method(engine, S, "int findLast(const string &in, int start = -1) const", findLast,
                       "Where a string last appears at or before start (-1: anywhere), or -1.");
                method(engine, S, "bool contains(const string &in) const", contains, "The string appears in it.");
                method(engine, S, "bool startsWith(const string &in) const", startsWith, "It begins with the string.");
                method(engine, S, "bool endsWith(const string &in) const", endsWith, "It ends with the string.");
                method(engine, S, "string replace(const string &in from, const string &in to) const", replace,
                       "Every from replaced by to.");
                method(engine, S, "string toUpper() const", changeCase<toupper>, "In capitals (ASCII letters).");
                method(engine, S, "string toLower() const", changeCase<tolower>, "In small letters (ASCII letters).");
                method(engine, S, "string trim() const", trim, "Without spaces, tabs and newlines at either end.");
                method(engine, S, "void insert(uint pos, const string &in)", insert, "Puts a string in at a position.");
                method(engine, S, "void erase(uint pos, int count = -1)", erase,
                       "Takes count bytes out from pos (-1: to the end).");

                RegisterScriptArray(engine, true);
                stringArrayType(engine);
                method(engine, S, "array<string>@ split(const string &in delimiter) const", split,
                       "The pieces between each delimiter (an empty delimiter: each byte).");
                method(engine, S, "array<string>@ split() const", splitWords,
                       "The words: the pieces between spaces, tabs and newlines.");
                def(engine, "string join(const array<string> &in, const string &in delimiter)", join,
                    "The strings joined, the delimiter between each.");
                def(engine, "int64 parseInt(const string &in, uint base = 10)", parseInt,
                    "The whole number the text begins with (0 if none).");
                def(engine, "float parseFloat(const string &in)", parseFloat,
                    "The number the text begins with (0 if none).");
                def(engine, "double parseDouble(const string &in)", parseDouble,
                    "The number the text begins with, in double precision (0 if none).");
                def(engine, "string formatFloat(double value, uint decimals)", formatFloat,
                    "A number with so many decimal places (formatFloat(3.14159, 2) is \"3.14\").");
                def(engine, "string chr(uint code)", chr, "The character with a Unicode code (chr(65) is \"A\").");
                def(engine, "uint ord(const string &in)", ord,
                    "The Unicode code of the first character (ord(\"A\") is 65).");
                return;
        }

} // namespace angel
