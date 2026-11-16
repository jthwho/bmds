/**
 * @file      chatmodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ChatView's model (devplan/chatview.md §4 step 1) fuzzed: any bytes parsed as a line (runs
 * covering the text in order, links inside it, the gutter in it), wrapped at several widths
 * (every byte but a break's spaces placed once, in order, each piece in one run), points hit
 * inside the text, the plain text made, and lines kept by ChatLines.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <bmds/assert.h>
#include <bmds/chatmodel.h>
#include <bmds/utf8.h>

using bmds::StringView;
namespace ChatText = bmds::ChatText;

// The platform's assert hook (the kernel's on the target): a failed one is a finding.
[[noreturn]] void bmds::Detail::assertFailed(const char *expr, const char *file, int line) {
        std::fprintf(stderr, "BMDS_ASSERT(%s) failed at %s:%d\n", expr, file, line);
        std::abort();
}

namespace {

        // 6 pixels a character, bold 7, monospace 8: styles measure differently.
        uint32_t charWidth(uint8_t flags) {
                return (flags & ChatText::Monospace) != 0 ? 8 : (flags & ChatText::Bold) != 0 ? 7 : 6;
        }

        uint32_t width(void *, StringView text, uint8_t flags) {
                uint32_t chars = 0;
                for (char c : text)
                        if (!bmds::Utf8::isContinuation(uint8_t(c))) ++chars;
                return chars * charWidth(flags);
        }

        size_t fitting(void *, StringView text, uint8_t flags, uint32_t most) {
                size_t   i = 0;
                uint32_t used = 0;
                while (i < text.size()) {
                        size_t n = 1;
                        while (i + n < text.size() && bmds::Utf8::isContinuation(uint8_t(text[i + n]))) ++n;
                        if (used + charWidth(flags) > most) break;
                        used += charWidth(flags);
                        i += n;
                }
                return i;
        }

        void check(bool ok) {
                if (!ok) std::abort();
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        const StringView line(reinterpret_cast<const char *>(data), size);
        ChatText::Parsed p;
        if (ChatText::parse(line, p).isError()) return 0;
        const StringView t = p.text.view();
        check(t.size() <= size);
        check(p.gutter == ChatText::NoGutter || p.gutter <= t.size());
        uint32_t at = 0;
        for (size_t r = 0; r < p.runCount; ++r) {
                check(p.runs[r].start == at && p.runs[r].length > 0);
                check(p.runs[r].link == ChatText::linkAt(p, p.runs[r].start) || p.runCount == ChatText::MaxRuns);
                at += p.runs[r].length;
        }
        check(at == t.size());
        for (size_t l = 0; l < p.linkCount; ++l) {
                check(p.links[l].length > 0 && p.links[l].start + p.links[l].length <= t.size());
                if (l > 0) check(p.links[l].start >= p.links[l - 1].start + p.links[l - 1].length);
        }
        bmds::String plain;
        check(ChatText::plain(line, plain).isOk());

        const ChatText::Measure m{width, fitting, nullptr};
        static ChatText::Row    rows[4096];
        static ChatText::Piece  pieces[8192];
        for (uint32_t w : {0u, 7u, 40u, 333u}) {
                for (uint32_t textX : {0u, 30u}) {
                        ChatText::Layout l{rows, 4096, pieces, 8192};
                        const size_t     n = ChatText::wrap(p, m, w, textX, &l);
                        check(n >= 1 && n == ChatText::wrap(p, m, w, textX, nullptr));
                        if (l.rowCount != n) continue; // more rows than kept: counted only
                        // In order; each in one run; only spaces left out.
                        uint32_t next = 0;
                        for (size_t k = 0; k < l.pieceCount; ++k) {
                                const ChatText::Piece &pc = pieces[k];
                                check(pc.length > 0 && pc.start >= next);
                                for (uint32_t b = next; b < pc.start; ++b) check(t[b] == ' ');
                                const ChatText::Run &run = p.runs[pc.run];
                                check(pc.start >= run.start && pc.start + pc.length <= run.start + run.length);
                                next = pc.start + pc.length;
                        }
                        for (uint32_t b = next; b < t.size(); ++b) check(t[b] == ' ');
                        for (size_t r = 0; r < l.rowCount; ++r)
                                for (int32_t x : {-5, 0, 13, 1000}) check(ChatText::hit(p, m, l, r, x) <= t.size());
                }
        }

        bmds::ChatLines lines(4);
        for (int i = 0; i < 6; ++i) check(lines.append(line, uint32_t(i)).isOk());
        uint32_t within = 0;
        check(lines.has(lines.lineAtRow(size, within)));
        return 0;
}
