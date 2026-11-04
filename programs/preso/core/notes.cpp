/**
 * @file      notes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "notes.h"

namespace preso {

        namespace {

                constexpr int64_t Second = 1000000000;

                // Characters (not bytes) in @p text, for lining up columns.
                uint32_t columns(StringView text) {
                        uint32_t n = 0;
                        for (uint32_t i = 0; i < text.length(); i++) n += (uint8_t(text[i]) & 0xC0) != 0x80;
                        return n;
                }

                void appendTime(Buffer &out, int64_t ns) {
                        int64_t s = ns / Second;
                        if (s < 0) s = 0;
                        if (s >= 3600) {
                                out.appendf("%d:%02d:%02d", int(s / 3600), int(s / 60 % 60), int(s % 60));
                        } else {
                                out.appendf("%d:%02d", int(s / 60), int(s % 60));
                        }
                        return;
                }

                // A block's text as plain words: inlines' text, breaks as spaces.
                void appendPlain(Buffer &out, const Inline *inl) {
                        for (; inl != nullptr; inl = inl->next) {
                                if (inl->type == InlineType::Text || inl->type == InlineType::Code) {
                                        out.append(inl->text);
                                } else if (inl->type == InlineType::SoftBreak || inl->type == InlineType::LineBreak) {
                                        out.append(' ');
                                }
                        }
                        return;
                }

                // The first lines of a slide's content (not its title): up to @p lines, each cut to
                // @p width columns.
                void firstLines(Buffer &out, const Slide &s, uint32_t lines, uint32_t width) {
                        uint32_t written = 0;
                        auto     line = [&](StringView text) {
                                text = text.trimmed();
                                if (text.isEmpty() || written >= lines) return;
                                uint32_t cut = 0, cols = 0;
                                while (cut < text.length() && cols < width) {
                                        cut++;
                                        while (cut < text.length() && (uint8_t(text[cut]) & 0xC0) == 0x80) cut++;
                                        cols++;
                                }
                                out.append("    ");
                                out.append(text.left(cut));
                                if (cut < text.length()) out.append("...");
                                out.append('\n');
                                written++;
                        };
                        auto blocks = [&](auto &self, const Block *b, bool top) -> void {
                                for (; b != nullptr && written < lines; b = b->next) {
                                        if (top && b->type == BlockType::Heading && b == s.blocks)
                                                continue; // the title
                                        if (b->inlines != nullptr) {
                                                Buffer text;
                                                appendPlain(text, b->inlines);
                                                line(text.view());
                                        } else if (b->type == BlockType::Code) {
                                                forEachLine(b->text, [&](StringView l, uint32_t) { line(l); });
                                        }
                                        self(self, b->children, false);
                                }
                        };
                        blocks(blocks, s.blocks, true);
                        return;
                }

        } // namespace

        int64_t paceSeconds(const Deck &deck, uint32_t slide, int64_t talkNs) {
                // Where the talk should be: the duration shared evenly, this slide begun at its share.
                uint32_t count = deck.slideCount();
                int64_t  duration = deck.settings().durationSeconds;
                if (duration <= 0 || count == 0) return 0;
                int64_t due = duration * int64_t(slide - 1) / int64_t(count);
                return talkNs / Second - due;
        }

        void writeRule(Buffer &out, const Deck &deck, uint32_t slide, const NotesTime &time, uint32_t width) {
                const Slide *s = deck.slide(slide);
                Buffer       left, right;
                left.appendf("\xE2\x94\x80\xE2\x94\x80 %u/%u  ", unsigned(slide), unsigned(deck.slideCount()));
                if (s != nullptr && !s->title.isEmpty()) left.append(s->title);
                if (time.talk >= 0) {
                        appendTime(right, time.talk);
                        right.append("  slide ");
                        appendTime(right, time.slide);
                        if (deck.settings().durationSeconds > 0) {
                                int64_t pace = paceSeconds(deck, slide, time.talk);
                                right.append("  ");
                                if (pace > 15) {
                                        appendTime(right, pace * Second);
                                        right.append(" behind");
                                } else if (pace < -15) {
                                        appendTime(right, -pace * Second);
                                        right.append(" ahead");
                                } else {
                                        right.append("on time");
                                }
                        }
                }
                uint32_t used = columns(left.view()) + columns(right.view()) + 2;
                out.append(left.view());
                out.append(' ');
                for (uint32_t i = used; i < width; i++) out.append("\xE2\x94\x80");
                out.append(' ');
                out.append(right.view());
                return;
        }

        void writeNotes(Buffer &out, const Deck &deck, uint32_t slide, uint32_t step, uint32_t steps,
                        const NotesTime &time, uint32_t width) {
                const Slide *s = deck.slide(slide);
                writeRule(out, deck, slide, time, width);
                out.append('\n');
                if (steps > 0) out.appendf("step %u of %u\n", unsigned(step + 1), unsigned(steps + 1));
                out.append('\n');
                bool any = false;
                for (const Note *n = s != nullptr ? s->notes : nullptr; n != nullptr; n = n->next) {
                        forEachLine(n->text, [&](StringView l, uint32_t) {
                                out.append(l.trimmed());
                                out.append('\n');
                        });
                        any = true;
                }
                if (!any) out.append("(no notes)\n");
                const Slide *next = deck.slide(slide + 1);
                out.append('\n');
                if (next == nullptr) {
                        out.append("Next: the end\n");
                        return;
                }
                out.appendf("Next: %u  ", unsigned(slide + 1));
                out.append(next->title.isEmpty() ? StringView("(no title)") : next->title);
                out.append('\n');
                firstLines(out, *next, 3, width > 8 ? width - 8 : width);
                return;
        }

} // namespace preso
