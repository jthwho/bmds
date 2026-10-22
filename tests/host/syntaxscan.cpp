/**
 * @file      syntaxscan.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Languages as data and their scanner (devplan/editor.md §5).
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <bmds/textsyntax.h>

using bmds::Span;
using bmds::StringView;
using bmds::StyleSpan;
using bmds::Syntax;
using bmds::SyntaxStyle;
namespace SyntaxState = bmds::SyntaxState;

namespace {

        const char *name(SyntaxStyle s) {
                static const char *names[] = {"normal",   "keyword", "type",    "constant", "number",   "string",
                                              "char",     "comment", "preproc", "operator", "function", "heading",
                                              "emphasis", "added",   "removed", "error"};
                return names[static_cast<int>(s)];
        }

        // The line's styled pieces, "text:style" each, space-separated (operators left out unless
        // @p operators, to keep the expectations short).
        std::string styled(const Syntax &syn, const char *line, uint16_t &state, bool operators = false) {
                StyleSpan spans[64];
                size_t    count = 0;
                state = syn.scan(StringView(line), state, spans, 64, count);
                std::string out;
                uint32_t    last = 0;
                for (size_t i = 0; i < count; ++i) {
                        CHECK(spans[i].start >= last); // in order, not overlapping
                        CHECK(spans[i].end > spans[i].start);
                        last = spans[i].end;
                        if (spans[i].style == SyntaxStyle::Operator && !operators) continue;
                        if (!out.empty()) out += ' ';
                        out += std::string(line + spans[i].start, spans[i].end - spans[i].start);
                        out += ':';
                        out += name(spans[i].style);
                }
                return out;
        }

        std::string styled(const char *id, const char *line, bool operators = false) {
                const Syntax *syn = Syntax::find(id);
                REQUIRE(syn != nullptr);
                uint16_t state = SyntaxState::Normal;
                return styled(*syn, line, state, operators);
        }

        void checkSorted(Span<const char *const> list, const char *what) {
                for (size_t i = 1; i < list.size(); ++i) {
                        INFO(what << ": " << list[i - 1] << " before " << list[i]);
                        CHECK(std::strcmp(list[i - 1], list[i]) < 0);
                }
        }

} // namespace

TEST_CASE("Syntax: every language's words are sorted, and found") {
        for (const Syntax *s : Syntax::all()) {
                checkSorted(s->keywords, s->id);
                checkSorted(s->types, s->id);
                checkSorted(s->constants, s->id);
                for (const char *w : s->keywords) CHECK(s->contains(s->keywords, w));
                CHECK(Syntax::find(s->id) == s);
        }
        CHECK(Syntax::find("nothing") == nullptr);
}

TEST_CASE("Syntax: a file's language from its name or its #! line") {
        const auto id = [](const char *path, const char *first = "") {
                const Syntax *s = Syntax::forFile(path, first);
                return std::string(s != nullptr ? s->id : "plain");
        };
        CHECK(id("/home/scripts/game.as") == "angelscript");
        CHECK(id("main.c") == "c");
        CHECK(id("x.H") == "c"); // extensions in any case
        CHECK(id("widget.hpp") == "cpp");
        CHECK(id("/src/Makefile") == "make");
        CHECK(id("rules.mk") == "make");
        CHECK(id("/cfg/applications/edit.desktop") == "ini");
        CHECK(id("/cfg/config/mimeapps.list") == "ini");
        CHECK(id("README.md") == "markdown");
        CHECK(id("fix.patch") == "diff");
        CHECK(id("data.json") == "json");
        CHECK(id("/cfg/profile") == "shell");
        CHECK(id("notes.txt") == "plain");
        CHECK(id("hello", "#!/rom/bin/angel") == "angelscript");
        CHECK(id("hello", "#!/usr/bin/env sh") == "shell");
        CHECK(id("hello", "#! /bin/sh -e") == "shell");
        CHECK(id("hello", "#!/usr/bin/python3") == "plain");
        CHECK(id("hello", "no shebang") == "plain");
        CHECK(id(".as") == "plain"); // nothing before the extension
}

TEST_CASE("Syntax: AngelScript") {
        CHECK(styled("angelscript", "int x = 0x1F; // set") == "int:type 0x1F:number // set:comment");
        CHECK(styled("angelscript", "if (a) return true;") == "if:keyword return:keyword true:constant");
        CHECK(styled("angelscript", "print(\"a\\\"b\" + 'c');") == "print:function \"a\\\"b\":string 'c':char");
        CHECK(styled("angelscript", "float f = 1.5e3f + 2.5d + .5;") ==
              "float:type 1.5e3f:number 2.5d:number .5:number");
        CHECK(styled("angelscript", "int8 v = x8 + 8;") == "int8:type 8:number"); // digits in words
        CHECK(styled("angelscript", "#include \"lib.as\"") == "#include:preproc \"lib.as\":string");
        CHECK(styled("angelscript", "#!/rom/bin/angel") == "#!/rom/bin/angel:comment");
        CHECK(styled("angelscript", "a = b + 1;", true) == "=:operator +:operator 1:number");

        // A block comment and a long string across lines.
        const Syntax *s = Syntax::find("angelscript");
        uint16_t      state = SyntaxState::Normal;
        CHECK(styled(*s, "x = 1; /* start", state) == "1:number /* start:comment");
        CHECK(state == SyntaxState::BlockComment);
        CHECK(styled(*s, "still in it", state) == "still in it:comment");
        CHECK(styled(*s, "end */ y = 2;", state) == "end */:comment 2:number");
        CHECK(state == SyntaxState::Normal);
        CHECK(styled(*s, "string t = \"\"\"first", state) == "string:type \"\"\"first:string");
        CHECK(state == SyntaxState::LongString);
        CHECK(styled(*s, "last\"\"\"; int z;", state) == "last\"\"\":string int:type");
        CHECK(state == SyntaxState::Normal);
        // An unclosed string ends with its line.
        CHECK(styled("angelscript", "s = \"open") == "\"open:string");
}

TEST_CASE("Syntax: C and C++") {
        CHECK(styled("c", "#include <stdio.h>") == "#include:preproc <stdio.h>:string");
        CHECK(styled("c", "#define N 10") == "#define:preproc 10:number");
        CHECK(styled("c", "static size_t n = sizeof(int);") == "static:keyword size_t:type sizeof:keyword int:type");
        CHECK(styled("c", "return NULL;") == "return:keyword NULL:constant");
        CHECK(styled("c", "x = a < b;") == ""); // `<` outside #include isn't a string
        CHECK(styled("cpp", "template <typename T> class A : public B {};") ==
              "template:keyword typename:keyword class:keyword public:keyword");
        CHECK(styled("cpp", "auto p = nullptr; // c") == "auto:keyword nullptr:constant // c:comment");
        CHECK(styled("c", "uint32_t v = 0b1010u;") == "uint32_t:type 0b1010u:number");
}

TEST_CASE("Syntax: shell") {
        CHECK(styled("shell", "if [ -f \"$HOME/x\" ]; then echo ${A} $1 # done") ==
              "if:keyword \"$HOME/x\":string then:keyword echo:type ${A}:constant $1:constant # done:comment");
        CHECK(styled("shell", "echo a#b 'it\\' $(pwd)") == "echo:type 'it\\':string $(:constant pwd:type");
        CHECK(styled("shell", "#!/bin/sh") == "#!/bin/sh:comment");
        CHECK(styled("shell", "x=`date`") == "`date`:string");
}

TEST_CASE("Syntax: INI and JSON") {
        CHECK(styled("ini", "[Desktop Entry]") == "[Desktop Entry]:keyword");
        CHECK(styled("ini", "Name = Edit", true) == "Name:type =:operator");
        CHECK(styled("ini", "  # a note") == "# a note:comment");
        CHECK(styled("ini", "; another") == "; another:comment");
        CHECK(styled("ini", "plain words") == "");
        CHECK(styled("json", "{\"key\": \"value\", \"n\": -1.5e2, \"t\": true, \"z\": null}") ==
              "\"key\":type \"value\":string \"n\":type 1.5e2:number \"t\":type true:constant \"z\":type "
              "null:constant");
}

TEST_CASE("Syntax: Markdown, diffs and Makefiles") {
        const Syntax *md = Syntax::find("markdown");
        uint16_t      state = SyntaxState::Normal;
        CHECK(styled(*md, "# Title", state) == "# Title:heading");
        CHECK(styled(*md, "- item with `code` and *em* or **strong**", state) ==
              "-:keyword `code`:string *em*:emphasis **strong**:emphasis");
        CHECK(styled(*md, "1. a [link](http://x) here", state) == "1.:keyword [link]:keyword (http://x):constant");
        CHECK(styled(*md, "> quoted", state) == "> quoted:comment");
        CHECK(styled(*md, "a * b * c", state) == ""); // spaces after the marks: not emphasis
        CHECK(styled(*md, "```c", state) == "```c:preproc");
        CHECK(styled(*md, "int x; # not a heading", state) == "int x; # not a heading:string");
        CHECK(styled(*md, "```", state) == "```:preproc");
        CHECK(state == SyntaxState::Normal);
        CHECK(styled(*md, "---", state, true) == "---:operator");

        CHECK(styled("diff", "--- a/x") == "--- a/x:keyword");
        CHECK(styled("diff", "@@ -1,2 +1,3 @@") == "@@ -1,2 +1,3 @@:preproc");
        CHECK(styled("diff", "+added") == "+added:added");
        CHECK(styled("diff", "-gone") == "-gone:removed");
        CHECK(styled("diff", " same") == "");

        CHECK(styled("make", "all: edit $(OBJS) # build") == "all:function $(:constant # build:comment");
        CHECK(styled("make", "CFLAGS := -O2") == "CFLAGS:type");
        CHECK(styled("make", "ifeq ($(X),1)") == "ifeq:keyword $(:constant 1:number");
        CHECK(styled("make", "\techo \"$@\" > out") == "echo:type \"$@\":string");
}

TEST_CASE("Syntax: the span limit is kept") {
        const Syntax *s = Syntax::find("angelscript");
        StyleSpan     spans[3];
        size_t        count = 0;
        (void)s->scan("int a; int b; int c; int d;", SyntaxState::Normal, spans, 3, count);
        CHECK(count == 3);
}
