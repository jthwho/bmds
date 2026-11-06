/**
 * @file      textsyntax.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Languages as data and the scanner that colours them (devplan/editor.md §5).
 */

#include <bmds/textsyntax.h>

#include <cstring>

BMDS_NAMESPACE_BEGIN

namespace {

        // ---- The languages' words (sorted: the host tests check)

        constexpr const char *AngelKeywords[] = {
                "abstract", "and",      "break",     "case",     "cast",      "catch",     "class",    "const",
                "continue", "default",  "do",        "else",     "enum",      "explicit",  "external", "final",
                "for",      "foreach",  "from",      "funcdef",  "function",  "get",       "if",       "import",
                "in",       "inout",    "interface", "is",       "mixin",     "namespace", "not",      "or",
                "out",      "override", "private",   "property", "protected", "return",    "set",      "shared",
                "super",    "switch",   "try",       "typedef",  "while",     "xor"};
        constexpr const char *AngelTypes[] = {"array",  "auto",   "bool",   "double", "float",  "int",
                                              "int16",  "int32",  "int64",  "int8",   "string", "uint",
                                              "uint16", "uint32", "uint64", "uint8",  "void"};
        constexpr const char *AngelConstants[] = {"false", "null", "this", "true"};

        constexpr const char *CKeywords[] = {"_Alignas",       "_Alignof",      "_Atomic",  "_Generic", "_Noreturn",
                                             "_Static_assert", "_Thread_local", "auto",     "break",    "case",
                                             "const",          "continue",      "default",  "do",       "else",
                                             "enum",           "extern",        "for",      "goto",     "if",
                                             "inline",         "register",      "restrict", "return",   "sizeof",
                                             "static",         "struct",        "switch",   "typedef",  "union",
                                             "volatile",       "while"};
        constexpr const char *CTypes[] = {
                "FILE",     "_Bool",    "bool",     "char",    "double",    "float",    "int",    "int16_t", "int32_t",
                "int64_t",  "int8_t",   "intptr_t", "long",    "ptrdiff_t", "short",    "signed", "size_t",  "ssize_t",
                "uint16_t", "uint32_t", "uint64_t", "uint8_t", "uintptr_t", "unsigned", "void",   "wchar_t"};
        constexpr const char *CConstants[] = {"EOF", "NULL", "false", "stderr", "stdin", "stdout", "true"};

        constexpr const char *CppKeywords[] = {"_Alignas",
                                               "_Static_assert",
                                               "alignas",
                                               "alignof",
                                               "and",
                                               "asm",
                                               "auto",
                                               "break",
                                               "case",
                                               "catch",
                                               "class",
                                               "co_await",
                                               "co_return",
                                               "co_yield",
                                               "concept",
                                               "const",
                                               "const_cast",
                                               "consteval",
                                               "constexpr",
                                               "constinit",
                                               "continue",
                                               "decltype",
                                               "default",
                                               "delete",
                                               "do",
                                               "dynamic_cast",
                                               "else",
                                               "enum",
                                               "explicit",
                                               "export",
                                               "extern",
                                               "final",
                                               "for",
                                               "friend",
                                               "goto",
                                               "if",
                                               "inline",
                                               "mutable",
                                               "namespace",
                                               "new",
                                               "noexcept",
                                               "not",
                                               "operator",
                                               "or",
                                               "override",
                                               "private",
                                               "protected",
                                               "public",
                                               "register",
                                               "reinterpret_cast",
                                               "requires",
                                               "return",
                                               "sizeof",
                                               "static",
                                               "static_assert",
                                               "static_cast",
                                               "struct",
                                               "switch",
                                               "template",
                                               "throw",
                                               "try",
                                               "typedef",
                                               "typeid",
                                               "typename",
                                               "union",
                                               "using",
                                               "virtual",
                                               "volatile",
                                               "while",
                                               "xor"};
        constexpr const char *CppTypes[] = {"FILE",      "bool",     "char",     "char16_t",  "char32_t", "char8_t",
                                            "double",    "float",    "int",      "int16_t",   "int32_t",  "int64_t",
                                            "int8_t",    "intptr_t", "long",     "ptrdiff_t", "short",    "signed",
                                            "size_t",    "ssize_t",  "uint16_t", "uint32_t",  "uint64_t", "uint8_t",
                                            "uintptr_t", "unsigned", "void",     "wchar_t"};
        constexpr const char *CppConstants[] = {"EOF",   "NULL",   "false", "nullptr", "stderr",
                                                "stdin", "stdout", "this",  "true"};

        constexpr const char *ShellKeywords[] = {"case",     "do", "done", "elif",   "else", "esac", "fi",    "for",
                                                 "function", "if", "in",   "select", "then", "time", "until", "while"};
        constexpr const char *ShellBuiltins[] = {
                "alias", "bg",     "break", "cd",    "continue", "echo",  "eval",    "exec",     "exit",   "export",
                "fg",    "jobs",   "kill",  "local", "printf",   "pwd",   "read",    "readonly", "return", "set",
                "shift", "source", "test",  "trap",  "ulimit",   "umask", "unalias", "unset",    "wait"};

        constexpr const char *JsonConstants[] = {"false", "null", "true"};

        constexpr const char *MakeKeywords[] = {"define", "else",  "endef",   "endif",    "export",   "ifdef", "ifeq",
                                                "ifndef", "ifneq", "include", "override", "unexport", "vpath"};

        // ---- File names and interpreters

        constexpr const char *AngelPatterns[] = {"*.as", nullptr};
        constexpr const char *AngelShebangs[] = {"angel", nullptr};
        constexpr const char *CPatterns[] = {"*.c", "*.h", nullptr};
        constexpr const char *CppPatterns[] = {"*.cpp", "*.cc", "*.cxx", "*.hpp", "*.hh", "*.hxx", "*.ipp", nullptr};
        constexpr const char *ShellPatterns[] = {"*.sh", "*.bash", "profile", ".profile", "*.profile", nullptr};
        constexpr const char *ShellShebangs[] = {"sh", "bash", "ash", "hush", "dash", "ksh", nullptr};
        constexpr const char *IniPatterns[] = {"*.ini",  "*.conf",    "*.cfg",  "*.desktop",
                                               "*.list", "*.service", "*.toml", nullptr};
        constexpr const char *JsonPatterns[] = {"*.json", nullptr};
        constexpr const char *MakePatterns[] = {"Makefile", "makefile", "GNUmakefile", "*.mk", nullptr};
        constexpr const char *MarkdownPatterns[] = {"*.md", "*.markdown", nullptr};
        constexpr const char *DiffPatterns[] = {"*.diff", "*.patch", nullptr};

        // ---- Helpers

        bool isDigit(char c) {
                return c >= '0' && c <= '9';
        }

        bool isHexDigit(char c) {
                return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        bool isWordStart(char c) {
                const auto u = static_cast<uint8_t>(c);
                return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
        }

        bool isWord(char c) {
                return isWordStart(c) || isDigit(c);
        }

        bool isSpace(char c) {
                return c == ' ' || c == '\t';
        }

        bool isOperator(char c) {
                return std::strchr("+-*/%=<>!&|^~?:@()[]{}", c) != nullptr && c != 0;
        }

        bool startsAt(StringView line, size_t i, const char *token) {
                if (token == nullptr) return false;
                const size_t n = std::strlen(token);
                return n != 0 && i + n <= line.size() && std::memcmp(line.data() + i, token, n) == 0;
        }

        char lower(char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        int compare(StringView a, const char *b, bool ignoreCase) {
                const size_t bn = std::strlen(b);
                const size_t n = a.size() < bn ? a.size() : bn;
                for (size_t i = 0; i < n; ++i) {
                        const char x = ignoreCase ? lower(a[i]) : a[i];
                        const char y = ignoreCase ? lower(b[i]) : b[i];
                        if (x != y) return static_cast<uint8_t>(x) < static_cast<uint8_t>(y) ? -1 : 1;
                }
                return a.size() < bn ? -1 : (a.size() > bn ? 1 : 0);
        }

        // Spans collected: a new one merged into the last when it follows it in the same style.
        struct Spans {
                        StyleSpan *out;
                        size_t     max;
                        size_t    &count;

                        void add(size_t start, size_t end, SyntaxStyle style) {
                                if (end <= start || style == SyntaxStyle::Normal) return;
                                if (count > 0 && out[count - 1].style == style && out[count - 1].end == start) {
                                        out[count - 1].end = uint32_t(end);
                                        return;
                                }
                                if (count < max) out[count++] = StyleSpan{uint32_t(start), uint32_t(end), style};
                                return;
                        }
        };

        // A number's end from @p i: C's forms (hex, binary, a fraction, an exponent, suffixes).
        size_t numberEnd(StringView line, size_t i) {
                const size_t n = line.size();
                if (line[i] == '0' && i + 1 < n && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
                        i += 2;
                        while (i < n && (isHexDigit(line[i]) || line[i] == '\'')) ++i;
                } else if (line[i] == '0' && i + 1 < n && (line[i + 1] == 'b' || line[i + 1] == 'B')) {
                        i += 2;
                        while (i < n && (line[i] == '0' || line[i] == '1' || line[i] == '\'')) ++i;
                } else {
                        while (i < n && (isDigit(line[i]) || line[i] == '\'')) ++i;
                        if (i < n && line[i] == '.') {
                                ++i;
                                while (i < n && isDigit(line[i])) ++i;
                        }
                        if (i < n && (line[i] == 'e' || line[i] == 'E')) {
                                size_t j = i + 1;
                                if (j < n && (line[j] == '+' || line[j] == '-')) ++j;
                                if (j < n && isDigit(line[j])) {
                                        i = j;
                                        while (i < n && isDigit(line[i])) ++i;
                                }
                        }
                }
                while (i < n && std::strchr("uUlLfFdD", line[i]) != nullptr && line[i] != 0) ++i;
                return i;
        }

        // The end of a quoted run from its opening quote at @p i (past its closing quote, or the
        // line's end); @p closed says which.
        size_t quotedEnd(StringView line, size_t i, char quote, char escape, bool &closed) {
                const size_t n = line.size();
                size_t       j = i + 1;
                closed = false;
                while (j < n) {
                        if (escape != 0 && line[j] == escape && j + 1 < n) {
                                j += 2;
                                continue;
                        }
                        if (line[j] == quote) {
                                closed = true;
                                return j + 1;
                        }
                        ++j;
                }
                return n;
        }

        // ---- The table's scanner

        uint16_t tableScan(const Syntax &syn, StringView line, uint16_t state, Spans &spans) {
                const size_t n = line.size();
                size_t       i = 0;
                // A comment or a long string going on from the line before.
                if (state == SyntaxState::BlockComment || state == SyntaxState::LongString) {
                        const char *close = state == SyntaxState::BlockComment ? syn.blockEnd : syn.longString;
                        const auto  style =
                                state == SyntaxState::BlockComment ? SyntaxStyle::Comment : SyntaxStyle::String;
                        const size_t at = close != nullptr ? line.indexOf(StringView(close)) : StringView::NPos;
                        if (at == StringView::NPos) {
                                spans.add(0, n, style);
                                return state;
                        }
                        i = at + std::strlen(close);
                        spans.add(0, i, style);
                }
                // INI: a section, a comment, or a key and its value.
                if ((syn.flags & Syntax::IniSections) != 0) {
                        while (i < n && isSpace(line[i])) ++i;
                        if (i >= n) return SyntaxState::Normal;
                        if (line[i] == '#' || line[i] == ';') {
                                spans.add(i, n, SyntaxStyle::Comment);
                        } else if (line[i] == '[') {
                                const size_t close = line.indexOf(']', i);
                                spans.add(i, close == StringView::NPos ? n : close + 1, SyntaxStyle::Keyword);
                        } else {
                                const size_t eq = line.indexOf('=', i);
                                const size_t colon = line.indexOf(':', i);
                                const size_t sep = eq < colon ? eq : colon;
                                if (sep != StringView::NPos) {
                                        size_t keyEnd = sep;
                                        while (keyEnd > i && isSpace(line[keyEnd - 1])) --keyEnd;
                                        spans.add(i, keyEnd, SyntaxStyle::Type);
                                        spans.add(sep, sep + 1, SyntaxStyle::Operator);
                                }
                        }
                        return SyntaxState::Normal;
                }
                // A script's `#!` line, in a language whose `#` is a directive: a comment.
                if (i == 0 && syn.preprocessor == '#' && line.startsWith("#!")) {
                        spans.add(0, n, SyntaxStyle::Comment);
                        return SyntaxState::Normal;
                }
                bool   lineStart = i == 0;
                size_t directiveEnd = StringView::NPos; // after `#include`: <file> is a string
                while (i < n) {
                        const char c = line[i];
                        if (isSpace(c)) {
                                ++i;
                                continue;
                        }
                        const bool first = lineStart;
                        lineStart = false;
                        if (syn.preprocessor != 0 && first && c == syn.preprocessor) {
                                size_t j = i + 1;
                                while (j < n && isSpace(line[j])) ++j;
                                while (j < n && isWord(line[j])) ++j;
                                spans.add(i, j, SyntaxStyle::Preprocessor);
                                directiveEnd = j;
                                i = j;
                                continue;
                        }
                        if (c == '<' && directiveEnd != StringView::NPos) {
                                const size_t close = line.indexOf('>', i);
                                if (close != StringView::NPos) {
                                        spans.add(i, close + 1, SyntaxStyle::String);
                                        i = close + 1;
                                        continue;
                                }
                        }
                        if (startsAt(line, i, syn.lineComment) &&
                            ((syn.flags & Syntax::HashComment) == 0 || i == 0 || isSpace(line[i - 1]))) {
                                spans.add(i, n, SyntaxStyle::Comment);
                                return SyntaxState::Normal;
                        }
                        if (startsAt(line, i, syn.blockStart)) {
                                const size_t from = i + std::strlen(syn.blockStart);
                                const size_t at = line.indexOf(StringView(syn.blockEnd), from);
                                if (at == StringView::NPos) {
                                        spans.add(i, n, SyntaxStyle::Comment);
                                        return SyntaxState::BlockComment;
                                }
                                const size_t end = at + std::strlen(syn.blockEnd);
                                spans.add(i, end, SyntaxStyle::Comment);
                                i = end;
                                continue;
                        }
                        if (startsAt(line, i, syn.longString)) {
                                const size_t from = i + std::strlen(syn.longString);
                                const size_t at = line.indexOf(StringView(syn.longString), from);
                                if (at == StringView::NPos) {
                                        spans.add(i, n, SyntaxStyle::String);
                                        return SyntaxState::LongString;
                                }
                                const size_t end = at + std::strlen(syn.longString);
                                spans.add(i, end, SyntaxStyle::String);
                                i = end;
                                continue;
                        }
                        const bool dq = c == '"' && (syn.quotes & Syntax::DoubleQuoted) != 0;
                        const bool sq = c == '\'' && (syn.quotes & (Syntax::SingleString | Syntax::SingleChar)) != 0;
                        const bool bq = c == '`' && (syn.quotes & Syntax::BackQuoted) != 0;
                        if (dq || sq || bq) {
                                // The shell's single quotes take no escapes.
                                const char   escape = (sq && (syn.quotes & Syntax::SingleString) != 0) ? 0 : syn.escape;
                                bool         closed;
                                const size_t end = quotedEnd(line, i, c, escape, closed);
                                SyntaxStyle  style = sq && (syn.quotes & Syntax::SingleChar) != 0
                                                             ? SyntaxStyle::Character
                                                             : SyntaxStyle::String;
                                if (dq && (syn.flags & Syntax::JsonKeys) != 0) {
                                        size_t j = end;
                                        while (j < n && isSpace(line[j])) ++j;
                                        if (j < n && line[j] == ':') style = SyntaxStyle::Type;
                                }
                                spans.add(i, end, style);
                                i = end;
                                continue;
                        }
                        if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(line[i + 1]))) {
                                // A number, unless it ends a word (`int8`) -- words are taken whole below.
                                const size_t end = (syn.flags & Syntax::CNumbers) != 0 ? numberEnd(line, i) : [&] {
                                        size_t j = i;
                                        while (j < n && isDigit(line[j])) ++j;
                                        return j;
                                }();
                                if (end < n && isWordStart(line[end]) && (syn.flags & Syntax::CNumbers) == 0) {
                                        size_t j = end;
                                        while (j < n && isWord(line[j])) ++j;
                                        i = j;
                                        continue;
                                }
                                spans.add(i, end, SyntaxStyle::Number);
                                i = end;
                                continue;
                        }
                        if (c == '$' && (syn.flags & Syntax::ShellVariables) != 0) {
                                size_t j = i + 1;
                                if (j < n && line[j] == '{') {
                                        const size_t close = line.indexOf('}', j);
                                        j = close == StringView::NPos ? n : close + 1;
                                } else if (j < n && line[j] == '(') {
                                        j += 1;
                                } else if (j < n && (isWordStart(line[j]) || isDigit(line[j]))) {
                                        while (j < n && isWord(line[j])) ++j;
                                } else if (j < n && std::strchr("?#@*!$-", line[j]) != nullptr && line[j] != 0) {
                                        ++j;
                                }
                                spans.add(i, j, SyntaxStyle::Constant);
                                i = j;
                                continue;
                        }
                        if (isWordStart(c)) {
                                size_t j = i;
                                while (j < n && isWord(line[j])) ++j;
                                const StringView word = line.mid(i, j - i);
                                SyntaxStyle      style = SyntaxStyle::Normal;
                                if (syn.contains(syn.keywords, word))
                                        style = SyntaxStyle::Keyword;
                                else if (syn.contains(syn.types, word))
                                        style = SyntaxStyle::Type;
                                else if (syn.contains(syn.constants, word))
                                        style = SyntaxStyle::Constant;
                                else if ((syn.flags & Syntax::Functions) != 0) {
                                        size_t k = j;
                                        while (k < n && isSpace(line[k])) ++k;
                                        if (k < n && line[k] == '(') style = SyntaxStyle::Function;
                                }
                                spans.add(i, j, style);
                                i = j;
                                continue;
                        }
                        if (isOperator(c)) spans.add(i, i + 1, SyntaxStyle::Operator);
                        ++i;
                }
                return SyntaxState::Normal;
        }

        // ---- Languages a table doesn't fit

        // Markdown's fenced code (devplan/editor.md §5a): in its fence, a line's state is
        // FenceBit | tilde << 14 | (length - 3, at most 3) << 12 | (language + 1) << 8 | the
        // language's own state (0: plain code). Fences are the only states with the top bit.
        constexpr uint16_t FenceBit = 0x8000;
        constexpr uint16_t FenceTilde = 0x4000;
        constexpr uint16_t InnerStates = 0xFF;
        constexpr uint16_t HtmlCommentState = SyntaxState::FirstOwnState; // in a `<!-- -->` (Preso's notes)

        size_t        languageIndex(const Syntax *syntax); // in All, defined with it
        const Syntax *languageAt(size_t index);

        // The fence starting at @p i (``` or ~~~, three or more), its length; 0 if none.
        size_t fenceAt(StringView line, size_t i) {
                if (i >= line.size() || (line[i] != '`' && line[i] != '~')) return 0;
                size_t j = i;
                while (j < line.size() && line[j] == line[i]) ++j;
                return j - i >= 3 ? j - i : 0;
        }

        // A Markdown line's inline part from @p i: code, emphasis, links, comments.
        uint16_t inlineScan(StringView line, size_t i, Spans &spans) {
                const size_t n = line.size();
                while (i < n) {
                        const char c = line[i];
                        if (c == '<' && startsAt(line, i, "<!--")) {
                                const size_t close = line.indexOf(StringView("-->"), i + 4);
                                if (close == StringView::NPos) {
                                        spans.add(i, n, SyntaxStyle::Comment);
                                        return HtmlCommentState;
                                }
                                spans.add(i, close + 3, SyntaxStyle::Comment);
                                i = close + 3;
                        } else if (c == '`') {
                                const size_t close = line.indexOf('`', i + 1);
                                const size_t end = close == StringView::NPos ? n : close + 1;
                                spans.add(i, end, SyntaxStyle::String);
                                i = end;
                        } else if ((c == '*' || c == '_') && i + 1 < n && !isSpace(line[i + 1])) {
                                const bool   strong = i + 1 < n && line[i + 1] == c;
                                const char   mark[3] = {c, strong ? c : '\0', '\0'};
                                const size_t from = i + (strong ? 2 : 1);
                                const size_t close = line.indexOf(StringView(mark), from);
                                if (close == StringView::NPos || close == from) {
                                        ++i;
                                        continue;
                                }
                                const size_t end = close + (strong ? 2 : 1);
                                spans.add(i, end, SyntaxStyle::Emphasis);
                                i = end;
                        } else if (c == '[') {
                                const size_t close = line.indexOf(']', i + 1);
                                if (close == StringView::NPos) {
                                        ++i;
                                        continue;
                                }
                                spans.add(i, close + 1, SyntaxStyle::Keyword);
                                i = close + 1;
                                if (i < n && line[i] == '(') {
                                        const size_t paren = line.indexOf(')', i);
                                        const size_t end = paren == StringView::NPos ? n : paren + 1;
                                        spans.add(i, end, SyntaxStyle::Constant);
                                        i = end;
                                }
                        } else {
                                ++i;
                        }
                }
                return SyntaxState::Normal;
        }

        // Markdown: headings, quotes, list marks, fenced code (in its language) and inline code,
        // emphasis, links, HTML comments (a deck's notes and directives) across lines.
        uint16_t markdownScan(const Syntax &self, StringView line, uint16_t state, StyleSpan *out, size_t max,
                              size_t &count) {
                count = 0;
                Spans        spans{out, max, count};
                const size_t n = line.size();
                size_t       i = 0;
                while (i < n && isSpace(line[i])) ++i;
                const size_t fence = i <= 3 ? fenceAt(line, i) : 0;
                if ((state & FenceBit) != 0) {
                        // Closed by the same character, at least as long, and nothing after but spaces.
                        const char   mark = (state & FenceTilde) != 0 ? '~' : '`';
                        const size_t opened = 3 + ((state >> 12) & 3);
                        size_t       end = i + fence;
                        while (end < n && isSpace(line[end])) ++end;
                        if (fence >= opened && line[i] == mark && end == n) {
                                spans.add(0, n, SyntaxStyle::Preprocessor);
                                return SyntaxState::Normal;
                        }
                        const Syntax *inner = languageAt(((state >> 8) & 0xF) - 1);
                        if (inner == nullptr) {
                                spans.add(0, n, SyntaxStyle::String);
                                return state;
                        }
                        const uint16_t next = inner->scan(line, state & InnerStates, out, max, count);
                        return uint16_t((state & ~InnerStates) | (next <= InnerStates ? next : 0));
                }
                if (state == HtmlCommentState) {
                        const size_t close = line.indexOf(StringView("-->"));
                        if (close == StringView::NPos) {
                                spans.add(0, n, SyntaxStyle::Comment);
                                return HtmlCommentState;
                        }
                        spans.add(0, close + 3, SyntaxStyle::Comment);
                        return inlineScan(line, close + 3, spans);
                }
                // A backtick fence's info string has no backticks (CommonMark).
                if (fence != 0 && (line[i] == '~' || line.indexOf('`', i + fence) == StringView::NPos)) {
                        spans.add(0, n, SyntaxStyle::Preprocessor);
                        // The info string's first word names the language (`angel run`: angel).
                        size_t from = i + fence;
                        while (from < n && isSpace(line[from])) ++from;
                        size_t to = from;
                        while (to < n && !isSpace(line[to]) && line[to] != '{' && line[to] != ',') ++to;
                        const Syntax *inner = Syntax::forName(line.mid(from, to - from));
                        const size_t  index = inner != nullptr && inner != &self ? languageIndex(inner) + 1 : 0;
                        return uint16_t(FenceBit | (line[i] == '~' ? FenceTilde : 0) |
                                        (fence - 3 < 3 ? fence - 3 : 3) << 12 | index << 8);
                }
                if (i < n && line[i] == '#') {
                        spans.add(0, n, SyntaxStyle::Heading);
                        return SyntaxState::Normal;
                }
                if (i < n && line[i] == '>') {
                        spans.add(0, n, SyntaxStyle::Comment);
                        return SyntaxState::Normal;
                }
                if (i + 2 < n && (line.mid(i, 3) == "---" || line.mid(i, 3) == "***" || line.mid(i, 3) == "___")) {
                        size_t j = i;
                        while (j < n && (line[j] == line[i] || isSpace(line[j]))) ++j;
                        if (j == n) {
                                spans.add(i, n, SyntaxStyle::Operator);
                                return SyntaxState::Normal;
                        }
                }
                // A list's mark: -, * or + and a space; or digits, a dot and a space.
                if (i + 1 < n && (line[i] == '-' || line[i] == '*' || line[i] == '+') && line[i + 1] == ' ') {
                        spans.add(i, i + 1, SyntaxStyle::Keyword);
                        i += 2;
                } else {
                        size_t j = i;
                        while (j < n && isDigit(line[j])) ++j;
                        if (j > i && j + 1 < n && (line[j] == '.' || line[j] == ')') && line[j + 1] == ' ') {
                                spans.add(i, j + 1, SyntaxStyle::Keyword);
                                i = j + 2;
                        }
                }
                return inlineScan(line, i, spans);
        }

        // A diff: the files' lines, hunks, lines added and removed.
        uint16_t diffScan(const Syntax &, StringView line, uint16_t, StyleSpan *out, size_t max, size_t &count) {
                count = 0;
                Spans spans{out, max, count};
                if (line.startsWith("+++") || line.startsWith("---") || line.startsWith("diff ") ||
                    line.startsWith("index "))
                        spans.add(0, line.size(), SyntaxStyle::Keyword);
                else if (line.startsWith("@@"))
                        spans.add(0, line.size(), SyntaxStyle::Preprocessor);
                else if (line.startsWith('+'))
                        spans.add(0, line.size(), SyntaxStyle::Added);
                else if (line.startsWith('-'))
                        spans.add(0, line.size(), SyntaxStyle::Removed);
                return SyntaxState::Normal;
        }

        const Syntax &shellSyntax();

        // A Makefile: recipes (a tab first) as the shell's; elsewhere the table's words and
        // comments, and a target's name before its `:` or a variable's before its `=`.
        uint16_t makeScan(const Syntax &syn, StringView line, uint16_t state, StyleSpan *out, size_t max,
                          size_t &count) {
                count = 0;
                if (line.startsWith('\t')) return shellSyntax().scan(line, state, out, max, count);
                Syntax table = syn;
                table.scanner = nullptr;
                Spans          spans{out, max, count};
                const uint16_t end = tableScan(table, line, state, spans);
                // The name: up to `:` (not `:=`), or to `=`, `:=`, `?=`, `+=`, `!=`.
                const size_t n = line.size();
                size_t       i = 0;
                while (i < n && isSpace(line[i])) ++i;
                size_t j = i;
                while (j < n && line[j] != 0 && (isWord(line[j]) || std::strchr("./-%$(){}", line[j]) != nullptr)) ++j;
                size_t k = j;
                while (k < n && isSpace(line[k])) ++k;
                SyntaxStyle style = SyntaxStyle::Normal;
                if (k < n && line[k] == ':' && !(k + 1 < n && line[k + 1] == '='))
                        style = SyntaxStyle::Function;
                else if (k < n && (line[k] == '=' || (k + 1 < n && line[k + 1] == '=' && line[k] != 0 &&
                                                      std::strchr(":?+!", line[k]) != nullptr)))
                        style = SyntaxStyle::Type;
                if (j == i || style == SyntaxStyle::Normal || syn.contains(syn.keywords, line.mid(i, j - i)))
                        return end;
                // The name's span first; those the table gave inside it go.
                size_t w = 0;
                for (size_t r = 0; r < count; ++r)
                        if (out[r].start >= j) out[w++] = out[r];
                count = w;
                if (count == max && count > 0) --count;
                for (size_t r = count; r > 0; --r) out[r] = out[r - 1];
                out[0] = StyleSpan{uint32_t(i), uint32_t(j), style};
                ++count;
                return end;
        }

        // ---- The languages

        constexpr Syntax AngelScript = [] {
                Syntax s;
                s.id = "angelscript";
                s.name = "AngelScript";
                s.patterns = AngelPatterns;
                s.shebangs = AngelShebangs;
                s.lineComment = "//";
                s.blockStart = "/*";
                s.blockEnd = "*/";
                s.longString = "\"\"\"";
                s.quotes = Syntax::DoubleQuoted | Syntax::SingleChar;
                s.flags = Syntax::CNumbers | Syntax::Functions;
                s.preprocessor = '#';
                s.keywords = AngelKeywords;
                s.types = AngelTypes;
                s.constants = AngelConstants;
                return s;
        }();

        constexpr Syntax C = [] {
                Syntax s;
                s.id = "c";
                s.name = "C";
                s.patterns = CPatterns;
                s.lineComment = "//";
                s.blockStart = "/*";
                s.blockEnd = "*/";
                s.quotes = Syntax::DoubleQuoted | Syntax::SingleChar;
                s.flags = Syntax::CNumbers | Syntax::Functions;
                s.preprocessor = '#';
                s.keywords = CKeywords;
                s.types = CTypes;
                s.constants = CConstants;
                return s;
        }();

        constexpr Syntax Cpp = [] {
                Syntax s = C;
                s.id = "cpp";
                s.name = "C++";
                s.patterns = CppPatterns;
                s.keywords = CppKeywords;
                s.types = CppTypes;
                s.constants = CppConstants;
                return s;
        }();

        constexpr Syntax Shell = [] {
                Syntax s;
                s.id = "shell";
                s.name = "Shell";
                s.patterns = ShellPatterns;
                s.shebangs = ShellShebangs;
                s.lineComment = "#";
                s.quotes = Syntax::DoubleQuoted | Syntax::SingleString | Syntax::BackQuoted;
                s.flags = Syntax::HashComment | Syntax::ShellVariables;
                s.keywords = ShellKeywords;
                s.types = ShellBuiltins;
                s.indent = 4;
                return s;
        }();

        constexpr Syntax Ini = [] {
                Syntax s;
                s.id = "ini";
                s.name = "INI";
                s.patterns = IniPatterns;
                s.flags = Syntax::IniSections;
                s.indent = 4;
                return s;
        }();

        constexpr Syntax Json = [] {
                Syntax s;
                s.id = "json";
                s.name = "JSON";
                s.patterns = JsonPatterns;
                s.quotes = Syntax::DoubleQuoted;
                s.flags = Syntax::CNumbers | Syntax::JsonKeys;
                s.constants = JsonConstants;
                s.indent = 4;
                return s;
        }();

        constexpr Syntax Make = [] {
                Syntax s;
                s.id = "make";
                s.name = "Makefile";
                s.patterns = MakePatterns;
                s.lineComment = "#";
                s.quotes = Syntax::DoubleQuoted | Syntax::SingleString;
                s.flags = Syntax::HashComment | Syntax::ShellVariables;
                s.keywords = MakeKeywords;
                s.scanner = makeScan;
                s.tabs = true;
                return s;
        }();

        constexpr Syntax Markdown = [] {
                Syntax s;
                s.id = "markdown";
                s.name = "Markdown";
                s.patterns = MarkdownPatterns;
                s.scanner = markdownScan;
                s.indent = 4;
                return s;
        }();

        constexpr Syntax Diff = [] {
                Syntax s;
                s.id = "diff";
                s.name = "Diff";
                s.patterns = DiffPatterns;
                s.scanner = diffScan;
                return s;
        }();

        const Syntax &shellSyntax() {
                return Shell;
        }

        constexpr const Syntax *All[] = {&AngelScript, &C, &Cpp, &Shell, &Ini, &Json, &Make, &Markdown, &Diff};
        static_assert(sizeof(All) / sizeof(All[0]) < 15, "a fence's state has 4 bits for its language");

        size_t languageIndex(const Syntax *syntax) {
                for (size_t i = 0; i < sizeof(All) / sizeof(All[0]); ++i)
                        if (All[i] == syntax) return i;
                return size_t(-1);
        }

        const Syntax *languageAt(size_t index) {
                return index < sizeof(All) / sizeof(All[0]) ? All[index] : nullptr;
        }

        // Names code blocks use that no id, extension or interpreter gives.
        struct Alias {
                        const char *name;
                        const char *id;
        };
        constexpr Alias Aliases[] = {{"c++", "cpp"},
                                     {"console", "shell"},
                                     {"shellscript", "shell"},
                                     {"makefile", "make"},
                                     {"patch", "diff"}};

        constexpr size_t MaxName = 64; // longer: no language's

        bool matches(StringView name, const char *pattern) {
                const StringView p(pattern);
                if (p.startsWith('*')) {
                        const StringView tail = p.mid(1);
                        return name.size() > tail.size() && name.right(tail.size()).equalsIgnoreCase(tail);
                }
                return name == p;
        }

} // namespace

uint16_t Syntax::scan(StringView line, uint16_t state, StyleSpan *spans, size_t max, size_t &count) const {
        if (scanner != nullptr) return scanner(*this, line, state, spans, max, count);
        count = 0;
        Spans s{spans, max, count};
        return tableScan(*this, line, state, s);
}

bool Syntax::contains(Span<const char *const> list, StringView word) const {
        const bool ignoreCase = (flags & CaseInsensitive) != 0;
        size_t     lo = 0, hi = list.size();
        while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                const int    c = compare(word, list[mid], ignoreCase);
                if (c == 0) return true;
                if (c < 0)
                        hi = mid;
                else
                        lo = mid + 1;
        }
        return false;
}

Span<const Syntax *const> Syntax::all() {
        return Span<const Syntax *const>(All);
}

const Syntax *Syntax::find(StringView id) {
        for (const Syntax *s : All)
                if (StringView(s->id) == id) return s;
        return nullptr;
}

const Syntax *Syntax::forName(StringView name) {
        if (name.isEmpty() || name.size() > MaxName) return nullptr;
        if (const Syntax *s = find(name)) return s;
        for (const Syntax *s : All)
                if (name.equalsIgnoreCase(StringView(s->name))) return s;
        for (const Alias &a : Aliases)
                if (name.equalsIgnoreCase(StringView(a.name))) return find(StringView(a.id));
        char buffer[MaxName + 3];
        // An extension (`as`, `py`): a file of that name's.
        if (name.indexOf('.') == StringView::NPos && name.indexOf('/') == StringView::NPos) {
                buffer[0] = 'x';
                buffer[1] = '.';
                std::memcpy(buffer + 2, name.data(), name.size());
                if (const Syntax *s = forFile(StringView(buffer, name.size() + 2), StringView())) return s;
        }
        // A file name (`Makefile`, `talk.md`), then an interpreter (`bash`, `angel`).
        if (const Syntax *s = forFile(name, StringView())) return s;
        buffer[0] = '#';
        buffer[1] = '!';
        std::memcpy(buffer + 2, name.data(), name.size());
        return forFile(StringView(), StringView(buffer, name.size() + 2));
}

const Syntax *Syntax::forFile(StringView path, StringView firstLine) {
        const size_t     slash = path.lastIndexOf('/');
        const StringView name = slash == StringView::NPos ? path : path.mid(slash + 1);
        for (const Syntax *s : All)
                for (const char *const *p = s->patterns; p != nullptr && *p != nullptr; ++p)
                        if (matches(name, *p)) return s;
        // `#!/rom/bin/angel` or `#!/usr/bin/env sh`: the interpreter's name, the last word before
        // its arguments.
        if (!firstLine.startsWith("#!")) return nullptr;
        StringView rest = firstLine.mid(2).trimmed();
        size_t     end = 0;
        while (end < rest.size() && !isSpace(rest[end])) ++end;
        StringView program = rest.left(end);
        if (program.endsWith("/env")) {
                rest = rest.mid(end).trimmed();
                end = 0;
                while (end < rest.size() && !isSpace(rest[end])) ++end;
                program = rest.left(end);
        }
        const size_t last = program.lastIndexOf('/');
        if (last != StringView::NPos) program = program.mid(last + 1);
        for (const Syntax *s : All)
                for (const char *const *p = s->shebangs; p != nullptr && *p != nullptr; ++p)
                        if (program == StringView(*p)) return s;
        return nullptr;
}

BMDS_NAMESPACE_END
