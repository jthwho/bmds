/**
 * @file      syntaxcalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SDK's code colouring (devplan/preso.md §9): syntax_find and syntax_scan (slots 133-134),
 * the editor's languages (bmds_core's textsyntax) for any program that shows code.
 */

#include <cstdint>
#include <cstring>
#include <bmds/errno.h>
#include <bmds/sys.h>
#include <bmds/textsyntax.h>
#include "structcopy.h"

using bmds::Span;
using bmds::StringView;
using bmds::StyleSpan;
using bmds::Syntax;
using bmds::SyntaxStyle;
using bmds::detail::copyIn;

namespace {

        static_assert(BMDS_SYNTAX_NORMAL == uint32_t(SyntaxStyle::Normal) &&
                              BMDS_SYNTAX_KEYWORD == uint32_t(SyntaxStyle::Keyword) &&
                              BMDS_SYNTAX_TYPE == uint32_t(SyntaxStyle::Type) &&
                              BMDS_SYNTAX_CONSTANT == uint32_t(SyntaxStyle::Constant) &&
                              BMDS_SYNTAX_NUMBER == uint32_t(SyntaxStyle::Number) &&
                              BMDS_SYNTAX_STRING == uint32_t(SyntaxStyle::String) &&
                              BMDS_SYNTAX_CHARACTER == uint32_t(SyntaxStyle::Character) &&
                              BMDS_SYNTAX_COMMENT == uint32_t(SyntaxStyle::Comment) &&
                              BMDS_SYNTAX_PREPROCESSOR == uint32_t(SyntaxStyle::Preprocessor) &&
                              BMDS_SYNTAX_OPERATOR == uint32_t(SyntaxStyle::Operator) &&
                              BMDS_SYNTAX_FUNCTION == uint32_t(SyntaxStyle::Function) &&
                              BMDS_SYNTAX_HEADING == uint32_t(SyntaxStyle::Heading) &&
                              BMDS_SYNTAX_EMPHASIS == uint32_t(SyntaxStyle::Emphasis) &&
                              BMDS_SYNTAX_ADDED == uint32_t(SyntaxStyle::Added) &&
                              BMDS_SYNTAX_REMOVED == uint32_t(SyntaxStyle::Removed) &&
                              BMDS_SYNTAX_ERROR == uint32_t(SyntaxStyle::Error) && uint32_t(SyntaxStyle::Count) == 16,
                      "the SDK's styles are the editor's");

        constexpr uint32_t MaxName = 64;   // longer: no language's
        constexpr size_t   MaxSpans = 128; // a call's (on the caller's stack: 1.5 KB)

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

        bool equalsIgnoringCase(StringView a, StringView b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i) {
                        char x = a[i], y = b[i];
                        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
                        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
                        if (x != y) return false;
                }
                return true;
        }

        const Syntax *lookUp(StringView name) {
                if (const Syntax *s = Syntax::find(name)) return s;
                for (const Syntax *s : Syntax::all())
                        if (equalsIgnoringCase(name, StringView(s->name))) return s;
                for (const Alias &a : Aliases)
                        if (equalsIgnoringCase(name, StringView(a.name))) return Syntax::find(StringView(a.id));
                char buffer[MaxName + 3];
                // An extension (`as`, `py`): a file of that name's.
                if (name.indexOf('.') == StringView::NPos && name.indexOf('/') == StringView::NPos) {
                        buffer[0] = 'x';
                        buffer[1] = '.';
                        std::memcpy(buffer + 2, name.data(), name.size());
                        if (const Syntax *s = Syntax::forFile(StringView(buffer, name.size() + 2), StringView()))
                                return s;
                }
                // A file name (`Makefile`, `talk.md`), then an interpreter (`bash`, `angel`).
                if (const Syntax *s = Syntax::forFile(name, StringView())) return s;
                buffer[0] = '#';
                buffer[1] = '!';
                std::memcpy(buffer + 2, name.data(), name.size());
                return Syntax::forFile(StringView(), StringView(buffer, name.size() + 2));
        }

} // namespace

extern "C" {

int32_t bmds_k_syntax_find(const char *name, uint32_t length) {
        if (name == nullptr && length > 0) return -BMDS_EFAULT;
        if (length == 0 || length > MaxName) return -BMDS_ENOENT;
        const Syntax *s = lookUp(StringView(name, length));
        if (s == nullptr) return -BMDS_ENOENT;
        Span<const Syntax *const> all = Syntax::all();
        for (size_t i = 0; i < all.size(); ++i)
                if (all[i] == s) return int32_t(i);
        return -BMDS_ENOENT;
}

int32_t bmds_k_syntax_scan(bmds_syntax_scan_t *scan) {
        bmds_syntax_scan_t s;
        int32_t            r = copyIn(scan, s);
        if (r < 0) return r;
        if (s.size < sizeof(bmds_syntax_scan_t)) return -BMDS_EINVAL;
        Span<const Syntax *const> all = Syntax::all();
        if (s.language < 0 || size_t(s.language) >= all.size() || s.state > 0xFFFFu) return -BMDS_EINVAL;
        if (s.length > 0 && s.line == nullptr) return -BMDS_EFAULT;
        if (s.max_spans > 0 && s.spans == nullptr) return -BMDS_EFAULT;
        StyleSpan spans[MaxSpans];
        size_t    count = 0;
        size_t    max = s.max_spans < MaxSpans ? s.max_spans : MaxSpans;
        uint16_t  state =
                all[size_t(s.language)]->scan(StringView(s.line, s.length), uint16_t(s.state), spans, max, count);
        for (size_t i = 0; i < count; ++i)
                s.spans[i] = bmds_syntax_span_t{spans[i].start, spans[i].end, uint32_t(spans[i].style)};
        scan->state = state;
        scan->count = uint32_t(count);
        return 0;
}

} // extern "C"
