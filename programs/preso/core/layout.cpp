/**
 * @file      layout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Laying out a slide (devplan/preso.md §6): blocks top to bottom in the content's box, text
 * broken into lines at spaces (a word wider than a line at characters), each line's pieces on
 * one baseline. What doesn't fit is cut at the slide's edge, and reported.
 */

#include "layout.h"
#include <string.h>
#include "effects.h"
#include <stdio.h>
#include <stdlib.h>

namespace preso {

        namespace {

                constexpr uint8_t DecoUnderline = 0x1;
                constexpr uint8_t DecoStrike = 0x2;
                constexpr uint8_t CodeBox = 0x4;

                constexpr uint8_t AlignLeft = 0;
                constexpr uint8_t AlignCenter = 1;
                constexpr uint8_t AlignRight = 2;

                constexpr int32_t  DefaultImageWidth = 480; // a picture whose size can't be told (no decoder)
                constexpr int32_t  DefaultImageHeight = 270;
                constexpr uint32_t MaxCodeSpans = 64;

                bool onlySpaces(const char *from, const char *to) {
                        for (const char *p = from; p < to; p++) {
                                if (*p != ' ') return false;
                        }
                        return true;
                }

        } // namespace

        struct Layouter::Piece {
                        enum Kind : uint8_t {
                                Text,
                                Break,
                                Box
                        };

                        Kind         kind = Text;
                        uint8_t      deco = 0;
                        bool         space = false; ///< A space before it.
                        StringView   text;
                        FontKey      font;
                        Color        color = 0;
                        const Image *image = nullptr;
                        int32_t      width = 0;
                        int32_t      height = 0; ///< A box's.
                        int32_t      x = 0;      ///< Placed: from the line's start.
        };

        struct Layouter::Flow {
                        Arena       *arena = nullptr;
                        const Slide *slide = nullptr;
                        bool         report = false;
                        DrawOp      *first = nullptr;
                        DrawOp     **tail = nullptr;
                        int32_t      x0 = 0;
                        int32_t      width = 0;
                        int32_t      y = 0;
                        int32_t      bottom = 0; ///< The content box's.
                        int32_t      gap = 0;    ///< Between blocks.
                        uint16_t     fragment = 0;
                        uint16_t    *steps = nullptr; ///< Each fragment's step (0: not met yet), by its number.
                        uint16_t     lastStep = 0;    ///< The last step given (a fragment's, a beat's).
                        uint32_t     scripts = 0;     ///< Regions laid out so far.
                        uint32_t     listDepth = 0;
                        Color        color = 0;        ///< Body text's.
                        Color        headingColor = 0; ///< 0: the theme's.
                        bool         center = false;
                        bool         reportedWidth = false;
        };

        Layouter::Layouter(Deck &deck, ThemeSet &themes, FontCache &fonts, Platform &platform, ImageCache *images)
            : _deck(deck), _themes(themes), _theme(&themes.base()), _fonts(fonts), _platform(platform), _images(images),
              _width(themes.base().width), _height(themes.base().height) {
                const Theme &theme = themes.base();
                StringView   size = deck.settings().size;
                if (size.equals("4:3")) {
                        _width = theme.height * 4 / 3;
                } else if (!size.isEmpty() && !size.equals("16:9")) {
                        int32_t x = size.indexOf('x');
                        if (x > 0) {
                                _width = size.left(uint32_t(x)).toInt(theme.width);
                                _height = size.mid(uint32_t(x) + 1).toInt(theme.height);
                        }
                }
                // The screen's limits (a BMDS surface's), a multiple of 4 across.
                if (_width > 1280) _width = 1280;
                if (_height > 720) _height = 720;
                _width &= ~3;
        }

        Layouter::~Layouter() {
                free(_pieces);
        }

        bool Layouter::addPiece(const Piece &p) {
                if (_pieceCount == _pieceCapacity) {
                        uint32_t capacity = _pieceCapacity == 0 ? 64 : _pieceCapacity * 2;
                        auto    *grown = static_cast<Piece *>(realloc(_pieces, capacity * sizeof(Piece)));
                        if (grown == nullptr) {
                                _outOfMemory = true;
                                return false;
                        }
                        _pieces = grown;
                        _pieceCapacity = capacity;
                }
                _pieces[_pieceCount++] = p;
                return true;
        }

        uint16_t Layouter::stepOf(Flow &flow) {
                // Fragments and beats take the slide's steps in the document's order: a fragment's
                // is the one after the last given when it is first met (a region before it has
                // taken its beats' steps).
                uint16_t f = flow.fragment;
                if (f == 0 || flow.steps == nullptr || f > flow.slide->fragments) return 0;
                if (flow.steps[f] == 0) flow.steps[f] = ++flow.lastStep;
                return flow.steps[f];
        }

        DrawOp *Layouter::op(Flow &flow, OpType type) {
                DrawOp *o = flow.arena->make<DrawOp>();
                if (o == nullptr) {
                        _outOfMemory = true;
                        return nullptr;
                }
                o->type = type;
                o->fragment = stepOf(flow);
                *flow.tail = o;
                flow.tail = &o->next;
                return o;
        }

        namespace {

                int32_t codePadding(const FontKey &font) {
                        return int32_t(font.size) / 5;
                }

        } // namespace

        int32_t Layouter::pieceWidth(Piece &p) {
                if (p.kind == Piece::Text) {
                        p.width = _fonts.width(p.font, p.text);
                        if ((p.deco & CodeBox) != 0) p.width += 2 * codePadding(p.font);
                }
                return p.width;
        }

        // ---- Inlines into pieces: words, with their font, colour and decoration.

        void Layouter::gather(const Inline *inl, const TextStyle &style, Color color) {
                _pieceCount = 0;
                bool space = false;
                for (; inl != nullptr; inl = inl->next) {
                        FontKey font = style.font;
                        if ((inl->style & InlineStyle::Bold) != 0) font.style |= FontKey::Bold;
                        if ((inl->style & InlineStyle::Italic) != 0) font.style |= FontKey::Italic;
                        Color   c = (inl->style & InlineStyle::Link) != 0 ? _theme->link : color;
                        uint8_t deco = uint8_t(((inl->style & InlineStyle::Link) != 0 ? DecoUnderline : 0) |
                                               ((inl->style & InlineStyle::Strike) != 0 ? DecoStrike : 0));
                        switch (inl->type) {
                                case InlineType::SoftBreak: space = true; break;
                                case InlineType::LineBreak: {
                                        Piece p;
                                        p.kind = Piece::Break;
                                        p.font = font;
                                        addPiece(p);
                                        space = false;
                                        break;
                                }
                                case InlineType::Image: {
                                        if (inl->image == nullptr) break;
                                        const Image &image = *inl->image;
                                        Piece        p;
                                        p.kind = Piece::Box;
                                        p.image = &image;
                                        p.space = space;
                                        p.font = font;
                                        // Its own size, or what `w:` and `h:` say (the other kept in proportion).
                                        int32_t nw = 0, nh = 0;
                                        naturalSize(image, &nw, &nh);
                                        int32_t w = image.width, h = image.height;
                                        if (w == 0 && h == 0) {
                                                w = int32_t(int64_t(nw) * image.scale / 100);
                                                h = int32_t(int64_t(nh) * image.scale / 100);
                                        } else if (h == 0) {
                                                h = int32_t(int64_t(w) * nh / nw);
                                        } else if (w == 0) {
                                                w = int32_t(int64_t(h) * nw / nh);
                                        }
                                        if (w < 1) w = 1;
                                        if (h < 1) h = 1;
                                        p.width = w;
                                        p.height = h;
                                        addPiece(p);
                                        space = false;
                                        break;
                                }
                                case InlineType::Code: {
                                        // One piece: code isn't broken at its spaces.
                                        Piece p;
                                        p.text = inl->text;
                                        p.font = _theme->code.font;
                                        p.font.size = uint16_t(style.font.size * 9 / 10);
                                        p.color = (inl->style & InlineStyle::Link) != 0 ? _theme->link
                                                                                        : _theme->code.color;
                                        p.deco = uint8_t(deco | CodeBox);
                                        p.space = space;
                                        addPiece(p);
                                        // Its box's padding, either side, is part of its width (pieceWidth()).
                                        space = false;
                                        break;
                                }
                                case InlineType::Text: {
                                        StringView t = inl->text;
                                        uint32_t   i = 0;
                                        while (i < t.length()) {
                                                if (isSpace(t[i])) {
                                                        space = true;
                                                        i++;
                                                        continue;
                                                }
                                                uint32_t start = i;
                                                while (i < t.length() && !isSpace(t[i])) i++;
                                                Piece p;
                                                p.text = t.mid(start, i - start);
                                                p.font = font;
                                                p.color = c;
                                                p.deco = deco;
                                                p.space = space;
                                                addPiece(p);
                                                space = false;
                                        }
                                        break;
                                }
                        }
                }
                for (uint32_t i = 0; i < _pieceCount; i++) pieceWidth(_pieces[i]);
                return;
        }

        // ---- Pieces into lines

        int32_t Layouter::lines(Flow &flow, int32_t x0, int32_t width, int32_t y, uint8_t align, bool emit) {
                FontKey spaceFont;
                int32_t spaceWidth = -1;
                auto    spaceOf = [&](const FontKey &font) {
                        if (spaceWidth < 0 || !(font == spaceFont)) {
                                spaceFont = font;
                                spaceWidth = _fonts.width(font, " ");
                        }
                        return spaceWidth;
                };
                uint32_t i = 0;
                if (_pieceCount == 0) return y;
                while (i < _pieceCount) {
                        uint32_t start = i;
                        int32_t  x = 0;
                        bool     broke = false;
                        while (i < _pieceCount) {
                                Piece &p = _pieces[i];
                                if (p.kind == Piece::Break) {
                                        i++;
                                        broke = true;
                                        break;
                                }
                                int32_t sw = p.space && x > 0 ? spaceOf(p.font) : 0;
                                if (x > 0 && x + sw + p.width > width) break;
                                if (x == 0 && p.kind == Piece::Text && p.width > width && p.text.length() > 1) {
                                        // A word wider than the line: what fits, the rest a piece after it.
                                        uint32_t n = _fonts.fit(p.font, p.text, width);
                                        if (n == 0) {
                                                n = 1;
                                                while (n < p.text.length() && (uint8_t(p.text[n]) & 0xC0) == 0x80) n++;
                                        }
                                        if (n < p.text.length()) {
                                                Piece rest = p;
                                                rest.text = p.text.mid(n);
                                                rest.space = false;
                                                if (!addPiece(rest)) return y;
                                                Piece &q = _pieces[i]; // the array may have moved
                                                memmove(&_pieces[i + 2], &_pieces[i + 1],
                                                        (_pieceCount - i - 2) * sizeof(Piece));
                                                _pieces[i + 1] = rest;
                                                q.text = q.text.left(n);
                                                pieceWidth(q);
                                                pieceWidth(_pieces[i + 1]);
                                        }
                                }
                                Piece &placed = _pieces[i];
                                if (placed.kind == Piece::Box && placed.width > width) {
                                        placed.height = placed.height * width / placed.width;
                                        placed.width = width;
                                }
                                placed.x = x + sw;
                                x = placed.x + placed.width;
                                i++;
                        }
                        uint32_t end = broke ? i - 1 : i;
                        // The line's height: its tallest piece's.
                        int32_t ascent = 0, descent = 0, height = 0;
                        for (uint32_t k = start; k < end; k++) {
                                const Piece &p = _pieces[k];
                                if (p.kind == Piece::Box) {
                                        if (p.height > ascent) ascent = p.height;
                                        continue;
                                }
                                FontMetrics m = _fonts.metrics(p.font);
                                if (m.ascent > ascent) ascent = m.ascent;
                                if (m.descent > descent) descent = m.descent;
                                if (m.lineHeight > height) height = m.lineHeight;
                        }
                        if (ascent + descent > height) height = ascent + descent;
                        if (end == start) {
                                // An empty line (a break after a break): a line of the first piece's font.
                                FontMetrics m = _fonts.metrics(_pieces[start < _pieceCount ? start : 0].font);
                                ascent = m.ascent;
                                height = m.lineHeight;
                        }
                        int32_t offset = align == AlignCenter ? (width - x) / 2 : align == AlignRight ? width - x : 0;
                        if (offset < 0) offset = 0;
                        if (emit) {
                                // Words are measured one by one (each rounded up); a run of them is
                                // drawn as one, and measured again: what follows it moves by the
                                // difference, so a styled word isn't a space too far.
                                DrawOp *last = nullptr;
                                DrawOp *lastLine = nullptr; // an underline or strike to carry on over a space
                                uint8_t lastDeco = 0;
                                int32_t shift = 0;
                                for (uint32_t k = start; k < end; k++) {
                                        const Piece &p = _pieces[k];
                                        int32_t      px = x0 + offset + p.x + shift;
                                        if (p.kind == Piece::Box) {
                                                imageBox(flow, *p.image, px, y + ascent - p.height, p.width, p.height,
                                                         false);
                                                last = lastLine = nullptr;
                                                continue;
                                        }
                                        FontMetrics m = _fonts.metrics(p.font);
                                        int32_t     top = y + ascent - m.ascent;
                                        int32_t     textX = px;
                                        int32_t     textWidth = p.width;
                                        if ((p.deco & CodeBox) != 0) {
                                                DrawOp *box = op(flow, OpType::Fill);
                                                if (box != nullptr) {
                                                        box->x = px;
                                                        box->y = top;
                                                        box->width = p.width;
                                                        box->height = m.ascent + m.descent;
                                                        box->color = _theme->inlineCodeBackground;
                                                }
                                                textX = px + codePadding(p.font);
                                                textWidth = p.width - 2 * codePadding(p.font);
                                                last = nullptr;
                                        }
                                        // Words of one style with only spaces between them: one run.
                                        if (last != nullptr && p.deco == 0 && last->font == p.font &&
                                            last->color == p.color &&
                                            last->text.data() + last->text.length() <= p.text.data() &&
                                            onlySpaces(last->text.data() + last->text.length(), p.text.data())) {
                                                last->text = StringView(
                                                        last->text.data(),
                                                        uint32_t(p.text.data() + p.text.length() - last->text.data()));
                                                last->width = _fonts.width(last->font, last->text);
                                                shift += last->x + last->width - (px + p.width);
                                                continue;
                                        }
                                        DrawOp *t = op(flow, OpType::Text);
                                        if (t == nullptr) return y;
                                        t->x = textX;
                                        t->y = top;
                                        t->width = textWidth;
                                        t->height = m.lineHeight;
                                        t->font = p.font;
                                        t->color = p.color;
                                        t->text = p.text;
                                        last = p.deco == 0 ? t : nullptr;
                                        uint8_t lined = uint8_t(p.deco & (DecoUnderline | DecoStrike));
                                        if (lined != 0 && lastLine != nullptr && lined == lastDeco && p.space &&
                                            lastLine->color == p.color) {
                                                // The line before carries on, over the space, to this word's end.
                                                lastLine->width = px + p.width - lastLine->x;
                                        } else if (lined != 0) {
                                                DrawOp *line = op(flow, OpType::Fill);
                                                if (line != nullptr) {
                                                        int32_t thick = int32_t(p.font.size) / 16 + 1;
                                                        line->x = px;
                                                        line->width = p.width;
                                                        line->height = thick;
                                                        line->y = (p.deco & DecoStrike) != 0 ? top + m.ascent * 2 / 3
                                                                                             : top + m.ascent + thick;
                                                        line->color = p.color;
                                                }
                                                lastLine = line;
                                        } else {
                                                lastLine = nullptr;
                                        }
                                        lastDeco = lined;
                                }
                        }
                        y += height;
                }
                return y;
        }

        bool Layouter::naturalSize(const Image &image, int32_t *w, int32_t *h) {
                if (_images != nullptr && !image.isScript() && _images->size(image.path, w, h) == Error::Ok && *w > 0 &&
                    *h > 0) {
                        return true;
                }
                *w = DefaultImageWidth;
                *h = DefaultImageHeight;
                return false;
        }

        void Layouter::placeholder(Flow &flow, const Image &image, int32_t x, int32_t y, int32_t w, int32_t h) {
                // A marked box, its file named in it: a picture missing, or a script region (step 6).
                DrawOp *frame = op(flow, OpType::Frame);
                if (frame == nullptr) return;
                frame->x = x;
                frame->y = y;
                frame->width = w;
                frame->height = h;
                frame->alpha = 2;
                frame->color = _theme->placeholder;
                const TextStyle &label = _theme->header;
                StringView       name = image.isScript() ? image.script : image.path;
                int32_t          room = w - 16;
                if (room <= 0 || h < label.font.size + 8) return;
                uint32_t n = _fonts.fit(label.font, name, room);
                DrawOp  *t = op(flow, OpType::Text);
                if (t == nullptr) return;
                t->x = x + 8;
                t->y = y + 6;
                t->font = label.font;
                t->color = _theme->placeholder;
                t->text = name.left(n);
                t->width = _fonts.width(label.font, t->text);
                return;
        }

        bool Layouter::scriptOp(Flow &flow, ScriptPlace &place, const Image *image, const Rect &r) {
                // A region its script draws in, its beats the steps after the last given (the
                // fragments after it come later). False without scripts (or memory).
                if (_scripts == nullptr || r.width <= 0 || r.height <= 0) return false;
                place.slide = flow.slide;
                place.image = image;
                place.theme = _theme;
                place.width = r.width;
                place.height = r.height;
                place.index = flow.scripts++;
                ScriptRegion *region = _scripts->open(place);
                if (region == nullptr) return false;
                DrawOp *o = op(flow, OpType::Script);
                if (o == nullptr) return true;
                o->x = r.x;
                o->y = r.y;
                o->width = r.width;
                o->height = r.height;
                o->clip = r;
                o->image = image;
                o->script = region;
                uint32_t beats = region->beats();
                if (beats > 0xFFFFu - flow.lastStep) beats = 0xFFFFu - flow.lastStep;
                o->beat = uint16_t(flow.lastStep + 1);
                flow.lastStep = uint16_t(flow.lastStep + beats);
                return true;
        }

        void Layouter::scriptBox(Flow &flow, const Image &image, int32_t x, int32_t y, int32_t w, int32_t h) {
                // A region its script draws in; without scripts (or memory), a box naming it.
                ScriptPlace place;
                if (!scriptOp(flow, place, &image, Rect{x, y, w, h})) placeholder(flow, image, x, y, w, h);
                return;
        }

        void Layouter::imageBox(Flow &flow, const Image &image, int32_t x, int32_t y, int32_t w, int32_t h,
                                bool background) {
                if (image.isScript()) {
                        scriptBox(flow, image, x, y, w, h);
                        return;
                }
                if (_images == nullptr || w <= 0 || h <= 0) {
                        placeholder(flow, image, x, y, w, h);
                        return;
                }
                auto say = [&](const char *why) {
                        if (flow.report) {
                                _deck.report(Severity::Warning, image.line, "slide %u: image `%.*s` %s",
                                             unsigned(flow.slide->number), int(image.path.length()), image.path.data(),
                                             why);
                        }
                };
                if (image.path.startsWith("http://") || image.path.startsWith("https://")) {
                        say("is on the web: pictures are read from files, not fetched");
                        placeholder(flow, image, x, y, w, h);
                        return;
                }
                int32_t nw = 0, nh = 0;
                Error   e = _images->size(image.path, &nw, &nh);
                if (e == Error::NotFound) say("isn't there");
                if (e == Error::Invalid || e == Error::Io || e == Error::TooBig)
                        say("isn't a picture Preso reads (PNG, JPEG, BMP)");
                if (e != Error::Ok || nw <= 0 || nh <= 0) {
                        // Error::NotSupported: a file, but no decoder here (the headless platform): its box.
                        placeholder(flow, image, x, y, w, h);
                        return;
                }
                Rect area{x, y, w, h};
                Rect dest = area;
                if (background) {
                        // Marp's sizes: `cover` (the default), `contain`, `auto` (its own), a percentage, or w:/h:.
                        int64_t dw = 0, dh = 0;
                        if (image.width != 0 || image.height != 0) {
                                dw = image.width != 0 ? image.width : int64_t(image.height) * nw / nh;
                                dh = image.height != 0 ? image.height : int64_t(image.width) * nh / nw;
                        } else if (image.fit == ImageFit::Natural) {
                                dw = nw;
                                dh = nh;
                        } else if (image.fit == ImageFit::Scale) {
                                dw = int64_t(nw) * image.scale / 100;
                                dh = int64_t(nh) * image.scale / 100;
                        } else {
                                // Cover: the larger of the two scales; contain: the smaller.
                                bool wider = int64_t(w) * nh > int64_t(h) * nw; // the area is wider than the picture
                                bool cover = image.fit != ImageFit::Contain;
                                if (wider == cover) {
                                        dw = w;
                                        dh = int64_t(w) * nh / nw;
                                } else {
                                        dh = h;
                                        dw = int64_t(h) * nw / nh;
                                }
                        }
                        // Only the part inside its area is ever scaled (ImageCache::get's window).
                        if (dw < 1) dw = 1;
                        if (dh < 1) dh = 1;
                        if (dw > 65536) dw = 65536;
                        if (dh > 65536) dh = 65536;
                        dest = Rect{x + (w - int32_t(dw)) / 2, y + (h - int32_t(dh)) / 2, int32_t(dw), int32_t(dh)};
                }
                // A picture far bigger than drawn decodes in more memory than it needs (a board has
                // little): said, with the size it would best be.
                int64_t drawn = int64_t(dest.width) * dest.height;
                if (int64_t(nw) * nh > 2 * drawn && int64_t(nw) * nh > 1000000 && flow.report) {
                        _deck.report(Severity::Warning, image.line,
                                     "slide %u: image `%.*s` is %dx%d but drawn %dx%d: a copy that size decodes in "
                                     "less memory, and better",
                                     unsigned(flow.slide->number), int(image.path.length()), image.path.data(), int(nw),
                                     int(nh), int(dest.width), int(dest.height));
                }
                DrawOp *o = op(flow, OpType::Image);
                if (o == nullptr) return;
                o->x = dest.x;
                o->y = dest.y;
                o->width = dest.width;
                o->height = dest.height;
                o->clip = area;
                o->image = &image;
                o->alpha = uint8_t(((image.flags & ImageFlag::Grayscale) != 0 ? ScaleFilter::Grayscale : 0) |
                                   ((image.flags & ImageFlag::Sepia) != 0 ? ScaleFilter::Sepia : 0));
                return;
        }

        void Layouter::backgrounds(Flow &flow, const Slide &slide) {
                // `backgroundImage: url(...)`: the whole slide, covered.
                StringView url = slide.settings.backgroundImage.trimmed();
                if (!url.isEmpty()) {
                        if (url.startsWith("url(") && url.endsWith(")")) url = url.mid(4, url.length() - 5).trimmed();
                        if (url.length() >= 2 && (url[0] == '"' || url[0] == '\'') && url[url.length() - 1] == url[0]) {
                                url = url.mid(1, url.length() - 2);
                        }
                        Image *image = flow.arena->make<Image>();
                        if (image != nullptr) {
                                image->path = url;
                                image->line = slide.line;
                                image->flags = ImageFlag::Background;
                                imageBox(flow, *image, 0, 0, _width, _height, true);
                        }
                }
                // `bg` pictures: the slide, or with one `left` or `right`, its share; several share
                // it side by side, or with `vertical`, one above another.
                const Image *split = nullptr;
                uint32_t     count = 0;
                bool         vertical = false;
                for (const Image *bg = slide.backgrounds; bg != nullptr; bg = bg->next) {
                        if (split == nullptr && bg->side != ImageSide::None) split = bg;
                        if ((bg->flags & ImageFlag::Vertical) != 0) vertical = true;
                        count++;
                }
                if (count == 0) return;
                Rect area{0, 0, _width, _height};
                if (split != nullptr) {
                        int32_t share = _width * (split->share != 0 ? split->share : 50) / 100;
                        area = Rect{split->side == ImageSide::Left ? 0 : _width - share, 0, share, _height};
                        if (split->side == ImageSide::Left) {
                                flow.x0 = share + _theme->marginLeft;
                                flow.width = _width - share - _theme->marginLeft - _theme->marginRight;
                        } else {
                                flow.width = _width - share - _theme->marginLeft - _theme->marginRight;
                        }
                }
                uint32_t i = 0;
                for (const Image *bg = slide.backgrounds; bg != nullptr; bg = bg->next, i++) {
                        Rect tile = area;
                        if (vertical) {
                                tile.y = area.y + int32_t(int64_t(area.height) * i / count);
                                tile.height = area.y + int32_t(int64_t(area.height) * (i + 1) / count) - tile.y;
                        } else {
                                tile.x = area.x + int32_t(int64_t(area.width) * i / count);
                                tile.width = area.x + int32_t(int64_t(area.width) * (i + 1) / count) - tile.x;
                        }
                        imageBox(flow, *bg, tile.x, tile.y, tile.width, tile.height, true);
                }
                return;
        }

        // ---- Decorations

        DrawOp *Layouter::insertOp(Flow &flow, DrawOp **at, OpType type) {
                // Behind what was laid out after @p at (a heading's band behind its text).
                DrawOp *o = flow.arena->make<DrawOp>();
                if (o == nullptr) {
                        _outOfMemory = true;
                        return nullptr;
                }
                o->type = type;
                o->fragment = stepOf(flow);
                o->next = *at;
                *at = o;
                if (flow.tail == at) flow.tail = &o->next;
                return o;
        }

        void Layouter::frame(Flow &flow, const Rect &r, Frame kind, Color color) {
                int32_t lw = lineWidth();
                auto    fill = [&](int32_t x, int32_t y, int32_t w, int32_t h, Color c) {
                        DrawOp *o = op(flow, OpType::Fill);
                        if (o == nullptr) return;
                        o->x = x;
                        o->y = y;
                        o->width = w;
                        o->height = h;
                        o->color = c;
                };
                auto outline = [&](const Rect &b, Color c) {
                        DrawOp *o = op(flow, OpType::Frame);
                        if (o == nullptr) return;
                        o->x = b.x;
                        o->y = b.y;
                        o->width = b.width;
                        o->height = b.height;
                        o->alpha = uint8_t(lw);
                        o->color = c;
                };
                switch (kind) {
                        case Frame::None: break;
                        case Frame::Single: outline(r, color); break;
                        case Frame::Double:
                                // As box drawing's ═: two lines a line apart.
                                outline(r, color);
                                outline(Rect{r.x + 2 * lw, r.y + 2 * lw, r.width - 4 * lw, r.height - 4 * lw}, color);
                                break;
                        case Frame::Raised:
                        case Frame::Sunken: {
                                // The toolkit's bevel: light from the top left.
                                Color light = 0xFFFFFF, dark = 0x808080, darker = 0x404040;
                                if (kind == Frame::Sunken) {
                                        Color t = light;
                                        light = darker;
                                        darker = t;
                                        dark = 0xDFDFDF;
                                }
                                fill(r.x, r.y, r.width, lw, light);
                                fill(r.x, r.y, lw, r.height, light);
                                fill(r.x, r.y + r.height - lw, r.width, lw, darker);
                                fill(r.x + r.width - lw, r.y, lw, r.height, darker);
                                fill(r.x + lw, r.y + r.height - 2 * lw, r.width - 2 * lw, lw, dark);
                                fill(r.x + r.width - 2 * lw, r.y + lw, lw, r.height - 2 * lw, dark);
                                break;
                        }
                }
                return;
        }

        void Layouter::heading(Flow &flow, const Block &b) {
                uint32_t            level = b.level >= 1 && b.level <= 3 ? b.level : 3;
                const HeadingStyle &h = _theme->heading[level - 1];
                bool                boxed = h.background != NoColor || h.frame != Frame::None;
                // A band or a frame gives the heading room inside it.
                int32_t  pad = boxed ? int32_t(h.text.font.size) / 4 + 2 * lineWidth() : 0;
                DrawOp **before = flow.tail;
                int32_t  top = flow.y;
                flow.y += pad;
                flow.x0 += pad;
                flow.width -= 2 * pad;
                paragraph(flow, b, h.text, b.has(BlockFlag::Fit));
                flow.x0 -= pad;
                flow.width += 2 * pad;
                flow.y += pad;
                Rect box{flow.x0, top, flow.width, flow.y - top};
                if (h.background != NoColor) {
                        // Behind the text: put before it in the list.
                        DrawOp *band = insertOp(flow, before, OpType::Fill);
                        if (band != nullptr) {
                                band->x = h.bar ? 0 : box.x;
                                band->y = box.y;
                                band->width = h.bar ? _width : box.width;
                                band->height = box.height;
                                band->color = h.background;
                        }
                }
                frame(flow, box, h.frame, h.frameColor != NoColor ? h.frameColor : h.text.color);
                if (h.underline == Underline::None) return;
                Color   color = h.underlineColor != NoColor ? h.underlineColor : _theme->accent;
                int32_t thick = int32_t(h.text.font.size) / 16 + 1;
                auto    rule = [&](int32_t y) {
                        DrawOp *o = op(flow, OpType::Fill);
                        if (o == nullptr) return;
                        o->x = flow.x0;
                        o->y = y;
                        o->width = flow.width;
                        o->height = thick;
                        o->color = color;
                };
                if (h.underline == Underline::Line) {
                        flow.y += thick;
                        rule(flow.y);
                        flow.y += thick;
                } else if (h.underline == Underline::Double) {
                        flow.y += thick;
                        rule(flow.y);
                        rule(flow.y + 2 * thick);
                        flow.y += 3 * thick;
                } else {
                        // Block shades across the content, fading out: a quarter each of █ ▓ ▒ ░.
                        FontKey font = _theme->body.font;
                        int32_t cell = _fonts.width(font, "\xE2\x96\x88");
                        if (cell <= 0) return;
                        uint32_t cells = uint32_t(flow.width / cell);
                        char    *text = static_cast<char *>(flow.arena->allocate(cells * 3 + 1, 1));
                        if (text == nullptr) return;
                        static const char *const shades[4] = {"\xE2\x96\x88", "\xE2\x96\x93", "\xE2\x96\x92",
                                                              "\xE2\x96\x91"};
                        for (uint32_t i = 0; i < cells; i++) memcpy(text + i * 3, shades[i * 4 / cells], 3);
                        DrawOp *t = op(flow, OpType::Text);
                        if (t == nullptr) return;
                        FontMetrics m = _fonts.metrics(font);
                        t->x = flow.x0;
                        t->y = flow.y;
                        t->font = font;
                        t->color = color;
                        t->text = StringView(text, cells * 3);
                        t->width = int32_t(cells) * cell;
                        t->height = m.lineHeight;
                        flow.y += m.lineHeight;
                }
                return;
        }

        // ---- Blocks

        void Layouter::paragraph(Flow &flow, const Block &b, const TextStyle &style, bool fit) {
                Color color = style.color;
                if (b.type == BlockType::Heading && flow.headingColor != 0) color = flow.headingColor;
                if (b.type != BlockType::Heading && flow.color != 0) color = flow.color;
                gather(b.inlines, style, color);
                if (fit && _pieceCount > 0) {
                        // Marp's `fit`: the heading's size scaled to fill the line.
                        lines(flow, 0, INT32_MAX / 2, 0, AlignLeft, false);
                        int32_t natural = 0;
                        for (uint32_t i = 0; i < _pieceCount; i++) {
                                if (_pieces[i].x + _pieces[i].width > natural)
                                        natural = _pieces[i].x + _pieces[i].width;
                        }
                        if (natural > 0) {
                                int32_t size = int32_t(style.font.size) * flow.width / natural;
                                if (size > 255) size = 255;
                                if (size < 8) size = 8;
                                TextStyle scaled = style;
                                scaled.font.size = uint16_t(size);
                                gather(b.inlines, scaled, color);
                        }
                }
                flow.y = lines(flow, flow.x0, flow.width, flow.y, flow.center ? AlignCenter : AlignLeft, true);
                return;
        }

        void Layouter::list(Flow &flow, const Block &b) {
                const TextStyle &body = _theme->body;
                FontMetrics      m = _fonts.metrics(body.font);
                uint32_t         number = b.start;
                int32_t          savedGap = flow.gap;
                uint16_t         savedFragment = flow.fragment;
                // Nested deep, the indent stops before the text has no room.
                int32_t indent = flow.width - _theme->listIndent >= 4 * _theme->listIndent ? _theme->listIndent : 0;
                flow.listDepth++;
                flow.x0 += indent;
                flow.width -= indent;
                bool first = true;
                for (const Block *item = b.children; item != nullptr; item = item->next) {
                        if (!first) flow.y += b.has(BlockFlag::Tight) ? m.lineHeight / 6 : savedGap;
                        first = false;
                        if (item->fragment != 0) flow.fragment = item->fragment;
                        // The marker: on the item's first line, ending a little before its text.
                        char       text[16];
                        StringView marker;
                        if (b.has(BlockFlag::Ordered)) {
                                int n = snprintf(text, sizeof text, "%u.", unsigned(number++));
                                marker = flow.arena->copy(StringView(text, uint32_t(n)));
                        } else {
                                marker = StringView(_theme->bullets[(flow.listDepth - 1) % 3]);
                        }
                        DrawOp *mark = op(flow, OpType::Text);
                        if (mark != nullptr) {
                                mark->font = body.font;
                                mark->text = marker;
                                mark->width = _fonts.width(body.font, marker);
                                mark->x = flow.x0 - mark->width - indent / 4;
                                mark->y = flow.y;
                                Color bullet = _theme->bulletColor != NoColor ? _theme->bulletColor : _theme->accent;
                                mark->color = b.has(BlockFlag::Ordered) ? (flow.color != 0 ? flow.color : body.color)
                                                                        : bullet;
                                mark->height = m.lineHeight;
                        }
                        flow.gap = b.has(BlockFlag::Tight) ? m.lineHeight / 6 : savedGap;
                        blocks(flow, item->children);
                        flow.gap = savedGap;
                        flow.fragment = savedFragment;
                }
                flow.x0 -= indent;
                flow.width += indent;
                flow.listDepth--;
                return;
        }

        void Layouter::quote(Flow &flow, const Block &b) {
                int32_t top = flow.y;
                Color   savedColor = flow.color;
                flow.color = _theme->quoteColor;
                flow.x0 += _theme->quoteIndent;
                flow.width -= _theme->quoteIndent;
                blocks(flow, b.children);
                flow.x0 -= _theme->quoteIndent;
                flow.width += _theme->quoteIndent;
                flow.color = savedColor;
                DrawOp *bar = op(flow, OpType::Fill);
                if (bar != nullptr) {
                        bar->x = flow.x0;
                        bar->y = top;
                        bar->width = 5;
                        bar->height = flow.y - top;
                        bar->color = _theme->quoteBar;
                }
                return;
        }

        void Layouter::code(Flow &flow, const Block &b) {
                const TextStyle &style = _theme->code;
                FontMetrics      m = _fonts.metrics(style.font);
                int32_t          pad = _theme->codePadding;
                // `angel run`: the code beside its pane (45% of the width), or above it (`below`).
                bool    runs = b.has(BlockFlag::Run) && _scripts != nullptr;
                bool    below = b.has(BlockFlag::RunBelow);
                int32_t savedWidth = flow.width;
                int32_t paneHeight = 6 * m.lineHeight + 2 * pad;
                if (runs && !below) flow.width = savedWidth * 55 / 100 - pad / 2;
                uint32_t         count = 1;
                for (uint32_t i = 0; i < b.text.length(); i++) {
                        if (b.text[i] == '\n') count++;
                }
                DrawOp *box = op(flow, OpType::Fill);
                if (box == nullptr) return;
                box->x = flow.x0;
                box->y = flow.y;
                box->width = flow.width;
                box->height = int32_t(count) * m.lineHeight + 2 * pad;
                box->color = _theme->codeBackground;
                int32_t  syntax = b.info.isEmpty() ? -1 : _platform.findSyntax(b.info);
                int32_t  tab = _fonts.width(style.font, "    ");
                uint32_t state = 0;
                int32_t  y = flow.y + pad;
                int32_t  right = flow.x0 + flow.width - pad;
                bool     cut = false;
                forEachLine(b.text, [&](StringView line, uint32_t) {
                        SyntaxSpan spans[MaxCodeSpans];
                        uint32_t   n = syntax >= 0 ? _platform.colorLine(syntax, line, &state, spans, MaxCodeSpans) : 0;
                        int32_t    x = flow.x0 + pad;
                        uint32_t   at = 0;
                        uint32_t   span = 0;
                        while (at < line.length()) {
                                // The next run: a span, or the plain text before one.
                                uint32_t end = line.length();
                                uint8_t  styleIndex = 0;
                                if (span < n && spans[span].start <= at) {
                                        end = spans[span].end < line.length() ? spans[span].end : line.length();
                                        styleIndex = spans[span].style;
                                        span++;
                                } else if (span < n) {
                                        end = spans[span].start;
                                }
                                if (end <= at) continue; // a span overlapping the one before: past it

                                Color color = styleIndex < 16 && _theme->syntax[styleIndex] != 0
                                                      ? _theme->syntax[styleIndex]
                                                      : style.color;
                                // Tabs to the next stop of four spaces.
                                StringView run = line.mid(at, end - at);
                                uint32_t   k = 0;
                                while (k < run.length()) {
                                        if (run[k] == '\t') {
                                                int32_t col = x - (flow.x0 + pad);
                                                x = flow.x0 + pad + (tab > 0 ? (col / tab + 1) * tab : col);
                                                k++;
                                                continue;
                                        }
                                        uint32_t start = k;
                                        while (k < run.length() && run[k] != '\t') k++;
                                        StringView piece = run.mid(start, k - start);
                                        DrawOp    *t = op(flow, OpType::Text);
                                        if (t == nullptr) return;
                                        t->x = x;
                                        t->y = y;
                                        t->font = style.font;
                                        t->color = color;
                                        t->text = piece;
                                        t->width = _fonts.width(style.font, piece);
                                        t->height = m.lineHeight;
                                        x += t->width;
                                }
                                at = end;
                        }
                        if (x > right) cut = true;
                        y += m.lineHeight;
                });
                if (cut && flow.report) {
                        _deck.report(Severity::Warning, b.line, "slide %u: a code line is wider than the slide: cut",
                                     unsigned(flow.slide->number));
                }
                if (runs && !below && box->height < paneHeight) box->height = paneHeight;
                frame(flow, Rect{box->x, box->y, box->width, box->height}, _theme->codeFrame, style.color);
                flow.y = box->y + box->height;
                flow.width = savedWidth;
                if (runs) {
                        // The pane: the block's region on the code's background. Its first beat (the
                        // step after the code appears) runs the script.
                        Rect    pane = below ? Rect{flow.x0, flow.y + pad, savedWidth, paneHeight}
                                             : Rect{box->x + box->width + pad, box->y, savedWidth - box->width - pad,
                                                 box->height};
                        DrawOp *back = op(flow, OpType::Fill);
                        if (back != nullptr) {
                                back->x = pane.x;
                                back->y = pane.y;
                                back->width = pane.width;
                                back->height = pane.height;
                                back->color = _theme->codeBackground;
                        }
                        ScriptPlace place;
                        place.source = b.text;
                        place.firstLine = b.line; // its first line of code
                        place.gated = true;
                        place.console = true;
                        Rect inner{pane.x + pad, pane.y + pad, pane.width - 2 * pad, pane.height - 2 * pad};
                        (void)scriptOp(flow, place, nullptr, inner);
                        frame(flow, pane, _theme->codeFrame, style.color);
                        if (pane.y + pane.height > flow.y) flow.y = pane.y + pane.height;
                }
                return;
        }

        void Layouter::table(Flow &flow, const Block &b) {
                constexpr uint32_t MaxColumns = 16;
                const TextStyle   &body = _theme->body;
                int32_t            padX = 14, padY = 8;
                int32_t            natural[MaxColumns] = {}, minimum[MaxColumns] = {}, widths[MaxColumns] = {};
                uint32_t           columns = 0;
                TextStyle          cellStyle = body;
                cellStyle.font.size = uint16_t(body.font.size * 5 / 6);
                Color color = flow.color != 0 ? flow.color : body.color;
                // The columns' widths: as wide as their text, or shared out to fit.
                bool tooWide = false;
                for (const Block *row = b.children; row != nullptr; row = row->next) {
                        uint32_t c = 0;
                        for (const Block *cell = row->children; cell != nullptr; cell = cell->next, c++) {
                                if (c == MaxColumns) {
                                        tooWide = true;
                                        break;
                                }
                                TextStyle s = cellStyle;
                                if (cell->has(BlockFlag::Header)) s.font.style |= FontKey::Bold;
                                gather(cell->inlines, s, color);
                                lines(flow, 0, INT32_MAX / 2, 0, AlignLeft, false);
                                int32_t w = 0, widest = 0;
                                for (uint32_t i = 0; i < _pieceCount; i++) {
                                        if (_pieces[i].x + _pieces[i].width > w) w = _pieces[i].x + _pieces[i].width;
                                        if (_pieces[i].width > widest) widest = _pieces[i].width;
                                }
                                if (w + 2 * padX > natural[c]) natural[c] = w + 2 * padX;
                                if (widest + 2 * padX > minimum[c]) minimum[c] = widest + 2 * padX;
                        }
                        if (c > columns) columns = c;
                }
                if (columns == 0) return;
                if (tooWide && flow.report) {
                        _deck.report(Severity::Warning, b.line,
                                     "slide %u: a table has more than %u columns: the rest aren't shown",
                                     unsigned(flow.slide->number), unsigned(MaxColumns));
                }
                int32_t totalNatural = 0, totalMinimum = 0;
                for (uint32_t c = 0; c < columns; c++) {
                        totalNatural += natural[c];
                        totalMinimum += minimum[c];
                }
                for (uint32_t c = 0; c < columns; c++) {
                        if (totalNatural <= flow.width) {
                                widths[c] = natural[c];
                        } else if (totalMinimum < flow.width && totalNatural > totalMinimum) {
                                widths[c] = minimum[c] +
                                            int32_t(int64_t(natural[c] - minimum[c]) * (flow.width - totalMinimum) /
                                                    (totalNatural - totalMinimum));
                        } else {
                                widths[c] = int32_t(int64_t(minimum[c]) * flow.width /
                                                    (totalMinimum > 0 ? totalMinimum : 1));
                        }
                }
                if (totalMinimum > flow.width && flow.report) {
                        _deck.report(Severity::Warning, b.line, "slide %u: a table's words are wider than the slide",
                                     unsigned(flow.slide->number));
                }
                int32_t tableWidth = 0;
                for (uint32_t c = 0; c < columns; c++) tableWidth += widths[c];
                int32_t left = flow.center ? flow.x0 + (flow.width - tableWidth) / 2 : flow.x0;
                for (const Block *row = b.children; row != nullptr; row = row->next) {
                        // The row's height: its tallest cell's.
                        int32_t  height = 0;
                        uint32_t c = 0;
                        for (const Block *cell = row->children; cell != nullptr && c < columns;
                             cell = cell->next, c++) {
                                TextStyle s = cellStyle;
                                if (cell->has(BlockFlag::Header)) s.font.style |= FontKey::Bold;
                                gather(cell->inlines, s, color);
                                int32_t h = lines(flow, 0, widths[c] - 2 * padX, 0, AlignLeft, false);
                                if (h > height) height = h;
                        }
                        if (height == 0) height = _fonts.metrics(cellStyle.font).lineHeight;
                        height += 2 * padY;
                        if (row->has(BlockFlag::Header)) {
                                DrawOp *bg = op(flow, OpType::Fill);
                                if (bg != nullptr) {
                                        bg->x = left;
                                        bg->y = flow.y;
                                        bg->width = tableWidth;
                                        bg->height = height;
                                        bg->color = _theme->tableHeaderBackground;
                                }
                        }
                        int32_t x = left;
                        c = 0;
                        for (const Block *cell = row->children; cell != nullptr && c < columns;
                             cell = cell->next, c++) {
                                TextStyle s = cellStyle;
                                if (cell->has(BlockFlag::Header)) s.font.style |= FontKey::Bold;
                                gather(cell->inlines, s, color);
                                uint8_t align = cell->align == Align::Center  ? AlignCenter
                                                : cell->align == Align::Right ? AlignRight
                                                                              : AlignLeft;
                                lines(flow, x + padX, widths[c] - 2 * padX, flow.y + padY, align, true);
                                x += widths[c];
                        }
                        flow.y += height;
                        DrawOp *rule = op(flow, OpType::Fill);
                        if (rule != nullptr) {
                                rule->x = left;
                                rule->y = flow.y - 1;
                                rule->width = tableWidth;
                                rule->height = row->has(BlockFlag::Header) ? 2 : 1;
                                rule->color = _theme->rule;
                        }
                }
                return;
        }

        void Layouter::block(Flow &flow, const Block &b) {
                switch (b.type) {
                        case BlockType::Paragraph: paragraph(flow, b, _theme->body, false); break;
                        case BlockType::Heading: heading(flow, b); break;
                        case BlockType::List: list(flow, b); break;
                        case BlockType::Quote: quote(flow, b); break;
                        case BlockType::Code: code(flow, b); break;
                        case BlockType::Table: table(flow, b); break;
                        case BlockType::ListItem:
                        case BlockType::TableRow:
                        case BlockType::TableCell: blocks(flow, b.children); break;
                }
                return;
        }

        void Layouter::blocks(Flow &flow, const Block *first) {
                for (const Block *b = first; b != nullptr && !_outOfMemory; b = b->next) {
                        if (b != first) flow.y += flow.gap;
                        block(flow, *b);
                }
                return;
        }

        SlideLayout *Layouter::layout(const Slide &slide, Arena &arena, bool report) {
                _outOfMemory = false;
                SlideLayout *result = arena.make<SlideLayout>();
                if (result == nullptr) return nullptr;
                result->slide = &slide;
                _theme = &_themes.forClasses(slide.settings.className);
                result->theme = _theme;
                result->background = _theme->background;
                const SlideSettings &s = slide.settings;
                Color                c = 0;
                if (!s.backgroundColor.isEmpty()) {
                        if (parseColor(s.backgroundColor, &c)) {
                                result->background = c;
                        } else if (report) {
                                _deck.report(Severity::Warning, slide.line,
                                             "slide %u: backgroundColor `%.*s` isn't a colour", unsigned(slide.number),
                                             int(s.backgroundColor.length()), s.backgroundColor.data());
                        }
                }
                Flow flow;
                flow.arena = &arena;
                flow.slide = &slide;
                flow.report = report;
                flow.tail = &flow.first;
                flow.x0 = _theme->marginLeft;
                flow.width = _width - _theme->marginLeft - _theme->marginRight;
                flow.y = _theme->marginTop;
                flow.bottom = _height - _theme->marginBottom;
                flow.gap = _fonts.metrics(_theme->body.font).lineHeight * _theme->blockGap16 / 16;
                if (slide.fragments > 0) {
                        flow.steps = static_cast<uint16_t *>(
                                arena.allocate((uint32_t(slide.fragments) + 1) * sizeof(uint16_t), alignof(uint16_t)));
                        if (flow.steps == nullptr) return nullptr;
                        for (uint32_t i = 0; i <= slide.fragments; i++) flow.steps[i] = 0;
                }
                if (!s.color.isEmpty()) {
                        if (parseColor(s.color, &c)) {
                                // 0 means "the theme's" here: black is a hair off it.
                                flow.color = flow.headingColor = c == 0 ? 0x000001 : c;
                        } else if (report) {
                                _deck.report(Severity::Warning, slide.line, "slide %u: color `%.*s` isn't a colour",
                                             unsigned(slide.number), int(s.color.length()), s.color.data());
                        }
                }
                // The theme's (its class's), unless the slide says.
                bool middle = s.align.equals("middle") || (_theme->middle && !s.align.equals("top"));
                flow.center = _theme->center;

                // A theme's pattern, tiled behind everything (its own size).
                if (_theme->pattern != nullptr && _images != nullptr) {
                        int32_t pw = 0, ph = 0;
                        Image  *image = flow.arena->make<Image>();
                        if (image != nullptr) image->path = StringView(_theme->pattern);
                        if (image != nullptr && _images->size(image->path, &pw, &ph) == Error::Ok && pw > 0 && ph > 0) {
                                DrawOp *o = op(flow, OpType::Pattern);
                                if (o != nullptr) {
                                        o->width = pw;
                                        o->height = ph;
                                        o->clip = Rect{0, 0, _width, _height};
                                        o->image = image;
                                }
                        } else if (image != nullptr && report && slide.number == 1) {
                                _deck.report(Severity::Warning, 0, "the theme's pattern `%s` can't be read",
                                             _theme->pattern);
                        }
                }
                backgrounds(flow, slide);

                DrawOp **contentStart = flow.tail;
                blocks(flow, slide.blocks);
                result->contentBottom = flow.y;
                if (middle && flow.y < flow.bottom) {
                        int32_t shift = (flow.bottom - flow.y) / 2;
                        for (DrawOp *o = *contentStart; o != nullptr; o = o->next) o->y += shift;
                        result->contentBottom += shift;
                }
                if (flow.y > flow.bottom) {
                        result->overflows = true;
                        if (report) {
                                _deck.report(Severity::Warning, slide.line,
                                             "slide %u: its content runs %d pixels past the bottom margin (cut at the "
                                             "slide's edge)",
                                             unsigned(slide.number), int(flow.y - flow.bottom));
                        }
                }

                // The header, the footer, the page number: outside the content, never shifted.
                flow.fragment = 0;
                const TextStyle &h = _theme->header;
                FontMetrics      hm = _fonts.metrics(h.font);
                int32_t          footY = _height - (_theme->marginBottom + hm.lineHeight) / 2;
                int32_t          headY = (_theme->marginTop - hm.lineHeight) / 2;
                if (_theme->headerBackground != NoColor) {
                        // A bar behind them, across the slide (a status line).
                        auto bar = [&](int32_t y) {
                                DrawOp *o = op(flow, OpType::Fill);
                                if (o == nullptr) return;
                                o->x = 0;
                                o->y = y - hm.lineHeight / 4;
                                o->width = _width;
                                o->height = hm.lineHeight + hm.lineHeight / 2;
                                o->color = _theme->headerBackground;
                        };
                        if (!s.header.isEmpty()) bar(headY);
                        if (!s.footer.isEmpty() || s.paginate) bar(footY);
                }
                auto line = [&](StringView text, int32_t x, int32_t y, bool right) {
                        if (text.isEmpty()) return;
                        DrawOp *t = op(flow, OpType::Text);
                        if (t == nullptr) return;
                        t->font = h.font;
                        t->color = h.color;
                        t->text = text;
                        t->width = _fonts.width(h.font, text);
                        t->x = right ? x - t->width : x;
                        t->y = y;
                        t->height = hm.lineHeight;
                };
                // On a split slide, on the text's side.
                int32_t left = flow.x0;
                int32_t right = flow.x0 + flow.width;
                line(s.header, left, headY, false);
                line(s.footer, left, footY, false);
                if (s.paginate) {
                        char       text[16];
                        int        n = snprintf(text, sizeof text, "%u", unsigned(slide.number));
                        StringView number = arena.copy(StringView(text, uint32_t(n)));
                        line(number, right, footY, true);
                }
                // A frame round the whole slide (a window), over everything.
                frame(flow, Rect{0, 0, _width, _height}, _theme->slideFrame, _theme->body.color);
                result->ops = flow.first;
                result->steps = flow.lastStep;
                return _outOfMemory ? nullptr : result;
        }

        namespace {

                uint32_t charactersOf(StringView text) {
                        uint32_t n = 0;
                        for (uint32_t i = 0; i < text.length(); i++) n += (uint8_t(text[i]) & 0xC0) != 0x80;
                        return n;
                }

                // The first @p n characters of @p text.
                StringView firstCharacters(StringView text, uint32_t n) {
                        uint32_t i = 0;
                        for (; i < text.length(); i++) {
                                if ((uint8_t(text[i]) & 0xC0) != 0x80) {
                                        if (n == 0) break;
                                        n--;
                                }
                        }
                        return text.left(i);
                }

        } // namespace

        uint32_t countCharacters(const SlideLayout &layout, uint32_t step) {
                uint32_t n = 0;
                for (const DrawOp *o = layout.ops; o != nullptr; o = o->next) {
                        if (o->type == OpType::Text && o->fragment <= step) n += charactersOf(o->text);
                }
                return n;
        }

        void drawSlide(const SlideLayout &layout, uint32_t step, Canvas &canvas, FontCache &fonts, ImageCache *images,
                       uint32_t maxCharacters, const Rect *area, bool moving) {
                uint32_t       budget = maxCharacters;
                const Theme   *theme = layout.theme;
                const Palette *palette = theme != nullptr && theme->palette.count > 0 ? &theme->palette : nullptr;
                // All of it, or only the area (what doesn't touch it is passed over).
                Rect slide{0, 0, canvas.width(), canvas.height()};
                Rect base = area != nullptr ? area->intersected(slide) : slide;
                if (base.isEmpty()) return;
                // An area under effects is drawn on the slide's grid of four (scanlines on the same
                // rows, the glow's quarter samples on the same pixels), and with the glow as far
                // round it as the glow reaches, so the area glows as the whole slide would; what is
                // drawn outside the area is then put back as it was (it hasn't changed).
                Rect     want = base;
                uint8_t *saved = nullptr;
                bool     glow = theme != nullptr && theme->glow > 0 && !moving;
                bool     effects = theme != nullptr && maxCharacters == UINT32_MAX && (glow || theme->scanlines > 0);
                if (area != nullptr && effects) {
                        int32_t margin = glow ? effectReach(*theme) : 0;
                        int32_t x0 = (base.x - margin) & ~3, y0 = (base.y - margin) & ~3;
                        int32_t x1 = (base.x + base.width + margin + 3) & ~3,
                                y1 = (base.y + base.height + margin + 3) & ~3;
                        Rect grown = Rect{x0, y0, x1 - x0, y1 - y0}.intersected(slide);
                        if (glow) {
                                uint32_t bpp = bytesPerPixel(canvas.format());
                                saved = static_cast<uint8_t *>(
                                        malloc(size_t(grown.width) * size_t(grown.height) * bpp));
                                if (saved != nullptr) {
                                        for (int32_t y = 0; y < grown.height; y++) {
                                                memcpy(saved + size_t(y) * size_t(grown.width) * bpp,
                                                       canvas.pixels() + size_t(grown.y + y) * canvas.stride() +
                                                               size_t(grown.x) * bpp,
                                                       size_t(grown.width) * bpp);
                                        }
                                        want = grown;
                                }
                        } else {
                                want = grown;
                        }
                }
                Rect inner = base; // what must come out right
                base = want;
                canvas.setClip(base);
                canvas.fill(base, layout.background);
                for (const DrawOp *o = layout.ops; o != nullptr; o = o->next) {
                        if (o->fragment > step) continue;
                        if (area != nullptr && o->type != OpType::Pattern) {
                                // Text's box is its line; a little over for what leans past it.
                                int32_t h = o->height > 0 ? o->height : int32_t(o->font.size) * 2;
                                int32_t pad = o->type == OpType::Text ? int32_t(o->font.size) / 2 : 0;
                                Rect    box{o->x - pad, o->y - pad, o->width + 2 * pad, h + 2 * pad};
                                if (o->type == OpType::Image) box = box.intersected(o->clip);
                                if (box.intersected(base).isEmpty()) continue;
                        }
                        switch (o->type) {
                                case OpType::Fill:
                                        canvas.blend(Rect{o->x, o->y, o->width, o->height}, o->color, o->alpha);
                                        break;
                                case OpType::Frame: {
                                        int32_t w = o->alpha;
                                        canvas.fill(Rect{o->x, o->y, o->width, w}, o->color);
                                        canvas.fill(Rect{o->x, o->y + o->height - w, o->width, w}, o->color);
                                        canvas.fill(Rect{o->x, o->y, w, o->height}, o->color);
                                        canvas.fill(Rect{o->x + o->width - w, o->y, w, o->height}, o->color);
                                        break;
                                }
                                case OpType::Text: {
                                        if (budget == UINT32_MAX) {
                                                fonts.draw(canvas, o->font, o->x, o->y, o->text, o->color);
                                                break;
                                        }
                                        // Typed: only as much as the budget has left.
                                        uint32_t   n = charactersOf(o->text);
                                        StringView shown = n <= budget ? o->text : firstCharacters(o->text, budget);
                                        budget -= n <= budget ? n : budget;
                                        if (!shown.isEmpty()) fonts.draw(canvas, o->font, o->x, o->y, shown, o->color);
                                        break;
                                }
                                case OpType::Image: {
                                        // The part of it inside its area and the slide: all that is scaled.
                                        Rect shown =
                                                Rect{o->x, o->y, o->width, o->height}.intersected(o->clip).intersected(
                                                        base);
                                        if (shown.isEmpty()) break;
                                        Rect window{shown.x - o->x, shown.y - o->y, shown.width, shown.height};
                                        const ScaledImage *image =
                                                images != nullptr
                                                        ? images->get(o->image->path, o->width, o->height, window,
                                                                      o->alpha, palette, canvas.format())
                                                        : nullptr;
                                        if (image != nullptr) {
                                                blitImage(canvas, *image, shown.x, shown.y);
                                        } else {
                                                // It couldn't be decoded (damaged, too big): its box.
                                                Rect c = o->clip;
                                                canvas.fill(Rect{c.x, c.y, c.width, 2}, 0xB0B0B0);
                                                canvas.fill(Rect{c.x, c.y + c.height - 2, c.width, 2}, 0xB0B0B0);
                                                canvas.fill(Rect{c.x, c.y, 2, c.height}, 0xB0B0B0);
                                                canvas.fill(Rect{c.x + c.width - 2, c.y, 2, c.height}, 0xB0B0B0);
                                        }
                                        break;
                                }
                                case OpType::Pattern: {
                                        // Tiled at its own size from the top left.
                                        Rect               whole{0, 0, o->width, o->height};
                                        const ScaledImage *tile =
                                                images != nullptr ? images->get(o->image->path, o->width, o->height,
                                                                                whole, 0, palette, canvas.format())
                                                                  : nullptr;
                                        if (tile == nullptr) break;
                                        Rect tiled = o->clip.intersected(base);
                                        canvas.setClip(tiled);
                                        // From the first tile touching it, on the pattern's grid.
                                        int32_t x0 = o->clip.x + (tiled.x - o->clip.x) / o->width * o->width;
                                        int32_t y0 = o->clip.y + (tiled.y - o->clip.y) / o->height * o->height;
                                        for (int32_t y = y0; y < tiled.y + tiled.height; y += o->height) {
                                                for (int32_t x = x0; x < tiled.x + tiled.width; x += o->width) {
                                                        blitImage(canvas, *tile, x, y);
                                                }
                                        }
                                        canvas.setClip(base);
                                        break;
                                }
                                case OpType::Script:
                                        if (o->script != nullptr) {
                                                canvas.setClip(o->clip.intersected(base));
                                                o->script->draw(canvas, o->x, o->y);
                                                canvas.setClip(base);
                                        }
                                        break;
                        }
                }
                canvas.clearClip();
                if (theme == nullptr || maxCharacters != UINT32_MAX) return;
                if (area == nullptr) {
                        applyEffects(canvas, *theme);
                        return;
                }
                // The area drawn (on the grid), as a canvas of its own.
                uint32_t bpp = bytesPerPixel(canvas.format());
                Canvas   part;
                part.attach(canvas.pixels() + size_t(base.y) * canvas.stride() + size_t(base.x) * bpp, canvas.stride(),
                            base.width, base.height, canvas.format());
                applyEffects(part, *theme, glow);
                if (saved != nullptr) {
                        // Round the area, the pixels as they were.
                        for (int32_t y = 0; y < base.height; y++) {
                                const uint8_t *from = saved + size_t(y) * size_t(base.width) * bpp;
                                uint8_t       *to =
                                        canvas.pixels() + size_t(base.y + y) * canvas.stride() + size_t(base.x) * bpp;
                                int32_t sy = base.y + y;
                                if (sy < inner.y || sy >= inner.y + inner.height) {
                                        memcpy(to, from, size_t(base.width) * bpp);
                                        continue;
                                }
                                int32_t left = inner.x - base.x, right = base.x + base.width - (inner.x + inner.width);
                                memcpy(to, from, size_t(left) * bpp);
                                memcpy(to + size_t(base.width - right) * bpp, from + size_t(base.width - right) * bpp,
                                       size_t(right) * bpp);
                        }
                        free(saved);
                }
                return;
        }

} // namespace preso
