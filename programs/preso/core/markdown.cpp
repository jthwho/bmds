/**
 * @file      markdown.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The deck's Markdown into slides (devplan/preso.md §4), md4c's callbacks building the model:
 * a `---` (md4c's horizontal rule, at the top level) ends a slide; an HTML comment is
 * directives if every line of it is one, else a speaker note; `bg` images go behind the slide;
 * `*` lists and `1)` lists are fragments.
 */

#include <md4c.h>
#include "buffer.h"
#include "deck.h"

namespace preso {

        namespace {

                constexpr uint32_t MaxDepth = 32; // blocks nested deeper are flattened into this one
                constexpr char     Replacement[] = "\xEF\xBF\xBD";

                struct Level {
                                Block  *block = nullptr; // nullptr: the slide itself
                                Block **childTail = nullptr;
                                Block  *implicit = nullptr; // a ListItem's paragraph for its tight text
                                bool    header = false;     // a table's head (THEAD)
                };

                // The md4c blocks that are blocks of the model (the others: the document, rules,
                // a table's head and body, HTML, which become something else or nothing).
                bool isModelBlock(MD_BLOCKTYPE type) {
                        return type != MD_BLOCK_DOC && type != MD_BLOCK_HR && type != MD_BLOCK_THEAD &&
                               type != MD_BLOCK_TBODY && type != MD_BLOCK_HTML;
                }

                class Builder {
                        public:
                                Builder(Deck &deck, uint8_t headingDivider) : _deck(deck), _divider(headingDivider) {}

                                Error run(StringView body);

                        private:
                                static int enterBlock(MD_BLOCKTYPE type, void *detail, void *self);
                                static int leaveBlock(MD_BLOCKTYPE type, void *detail, void *self);
                                static int enterSpan(MD_SPANTYPE type, void *detail, void *self);
                                static int leaveSpan(MD_SPANTYPE type, void *detail, void *self);
                                static int text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *self);

                                int onEnterBlock(MD_BLOCKTYPE type, void *detail);
                                int onLeaveBlock(MD_BLOCKTYPE type, void *detail);
                                int onEnterSpan(MD_SPANTYPE type, void *detail);
                                int onLeaveSpan(MD_SPANTYPE type);
                                int onText(MD_TEXTTYPE type, StringView text);

                                bool   startSlide();
                                Block *pushBlock(BlockType type);
                                // Where text goes now: the innermost block that takes inlines, a
                                // ListItem's paragraph made on the way; nullptr if none.
                                Block     *inlineTarget();
                                Inline    *addInline(InlineType type, StringView text);
                                void       addText(StringView text);
                                void       noteLine(const char *p);
                                void       comment(StringView body, uint32_t line);
                                void       htmlText(StringView html, uint32_t line, bool inParagraph);
                                void       finishHeading(Block *heading);
                                void       dropEmptyParagraph();
                                StringView attribute(const MD_ATTRIBUTE &attribute);
                                StringView copy(StringView text);
                                void       addDirective(StringView name, StringView value, bool spot, uint32_t line);
                                void       addNote(StringView text, uint32_t line);

                                Deck          &_deck;
                                uint8_t        _divider;
                                Slide         *_slide = nullptr;
                                Block        **_slideTail = nullptr;
                                Image        **_backgroundTail = nullptr;
                                Note         **_noteTail = nullptr;
                                RawDirective **_directiveTail = nullptr;
                                Level          _levels[MaxDepth];
                                uint32_t       _depth = 0;    // levels in use (0: the slide)
                                uint32_t       _overflow = 0; // blocks entered past MaxDepth
                                uint32_t       _listDepth = 0;
                                bool           _warnedDepth = false;
                                // Spans: how deep in each.
                                uint32_t _bold = 0, _italic = 0, _strike = 0, _link = 0, _code = 0;
                                // An image's alt text, gathered until its span ends.
                                uint32_t   _imageDepth = 0;
                                Buffer     _alt;
                                StringView _imagePath;
                                uint32_t   _imageLine = 0;
                                // An inline comment spread over several pieces; an HTML block.
                                bool     _inComment = false;
                                uint32_t _commentLine = 0;
                                Buffer   _html;
                                bool     _inHtmlBlock = false;
                                uint32_t _htmlLine = 0;
                                // A code block's text.
                                Buffer   _code_;
                                bool     _inCodeBlock = false;
                                uint32_t _codeLine = 0;
                                Error    _error = Error::Ok;
                };

                StringView plainText(const Inline *inl, Arena &arena) {
                        Buffer text;
                        for (; inl != nullptr; inl = inl->next) {
                                if (inl->type == InlineType::Text || inl->type == InlineType::Code) {
                                        text.append(inl->text);
                                } else if (inl->type == InlineType::SoftBreak || inl->type == InlineType::LineBreak) {
                                        text.append(' ');
                                }
                        }
                        if (text.hasFailed()) return StringView();
                        return arena.copy(text.view().trimmed());
                }

                bool isBreakTag(StringView html) {
                        StringView t = html.trimmed();
                        return t.equalsIgnoringCase("<br>") || t.equalsIgnoringCase("<br/>") ||
                               t.equalsIgnoringCase("<br />");
                }

        } // namespace

        StringView Builder::copy(StringView text) {
                if (_deck.isInSource(text.data())) return text;
                bool       ok = false;
                StringView c = _deck.arena().copy(text, &ok);
                if (!ok) _error = Error::NoMemory;
                return c;
        }

        StringView Builder::attribute(const MD_ATTRIBUTE &a) {
                // Entities in it are translated; the rest is as md4c gives it (escapes resolved).
                bool plain = true;
                for (uint32_t i = 0; a.substr_offsets != nullptr && a.substr_offsets[i] < a.size; i++) {
                        if (a.substr_types[i] != MD_TEXT_NORMAL) plain = false;
                }
                if (plain) return copy(StringView(a.text, a.size));
                Buffer out;
                for (uint32_t i = 0; a.substr_offsets[i] < a.size; i++) {
                        StringView piece(a.text + a.substr_offsets[i], a.substr_offsets[i + 1] - a.substr_offsets[i]);
                        char       utf8[8];
                        if (a.substr_types[i] == MD_TEXT_ENTITY) {
                                StringView t = translateEntity(piece, utf8);
                                out.append(t.isEmpty() ? piece : t);
                        } else if (a.substr_types[i] == MD_TEXT_NULLCHAR) {
                                out.append(Replacement);
                        } else {
                                out.append(piece);
                        }
                }
                if (out.hasFailed()) {
                        _error = Error::NoMemory;
                        return StringView();
                }
                bool       ok = false;
                StringView c = _deck.arena().copy(out.view(), &ok);
                if (!ok) _error = Error::NoMemory;
                return c;
        }

        bool Builder::startSlide() {
                _slide = _deck.appendSlide();
                if (_slide == nullptr) {
                        _error = Error::NoMemory;
                        return false;
                }
                _slideTail = &_slide->blocks;
                _backgroundTail = &_slide->backgrounds;
                _noteTail = &_slide->notes;
                _directiveTail = &_slide->directives;
                _depth = 0;
                _levels[0] = Level();
                _levels[0].childTail = _slideTail;
                return true;
        }

        Block *Builder::pushBlock(BlockType type) {
                Block *b = _deck.arena().make<Block>();
                if (b == nullptr) {
                        _error = Error::NoMemory;
                        return nullptr;
                }
                b->type = type;
                Level &parent = _levels[_depth];
                // A block inside a ListItem ends the paragraph its tight text was going into.
                parent.implicit = nullptr;
                *parent.childTail = b;
                parent.childTail = &b->next;
                Level &l = _levels[_depth + 1];
                l = Level();
                l.block = b;
                l.childTail = &b->children;
                l.header = parent.header;
                _depth++;
                return b;
        }

        Block *Builder::inlineTarget() {
                Level &l = _levels[_depth];
                if (l.block == nullptr) return nullptr;
                switch (l.block->type) {
                        case BlockType::Paragraph:
                        case BlockType::Heading:
                        case BlockType::TableCell: return l.block;
                        case BlockType::ListItem:
                        case BlockType::Quote:
                                if (l.implicit == nullptr) {
                                        Block *p = _deck.arena().make<Block>();
                                        if (p == nullptr) {
                                                _error = Error::NoMemory;
                                                return nullptr;
                                        }
                                        p->type = BlockType::Paragraph;
                                        *l.childTail = p;
                                        l.childTail = &p->next;
                                        l.implicit = p;
                                }
                                return l.implicit;
                        default: return nullptr;
                }
        }

        void Builder::noteLine(const char *p) {
                uint32_t line = _deck.lineOf(p);
                if (line == 0) return;
                if (_slide->line == 0) _slide->line = line;
                for (uint32_t i = 1; i <= _depth; i++) {
                        if (_levels[i].block != nullptr && _levels[i].block->line == 0) _levels[i].block->line = line;
                }
                if (_levels[_depth].implicit != nullptr && _levels[_depth].implicit->line == 0) {
                        _levels[_depth].implicit->line = line;
                }
                return;
        }

        Inline *Builder::addInline(InlineType type, StringView text) {
                Block *target = inlineTarget();
                if (target == nullptr) return nullptr;
                uint8_t style = uint8_t((_bold > 0 ? InlineStyle::Bold : 0) | (_italic > 0 ? InlineStyle::Italic : 0) |
                                        (_strike > 0 ? InlineStyle::Strike : 0) | (_link > 0 ? InlineStyle::Link : 0));
                // A block's inlines are few (runs merge): its end is found by walking.
                Inline **tail = &target->inlines;
                Inline  *last = nullptr;
                while (*tail != nullptr) {
                        last = *tail;
                        tail = &last->next;
                }
                // Text next to text of the same style, both in the source, is one run.
                if (last != nullptr && (type == InlineType::Text || type == InlineType::Code) && last->type == type &&
                    last->style == style && _deck.isInSource(last->text.data()) && !text.isEmpty() &&
                    last->text.data() + last->text.length() == text.data()) {
                        last->text = StringView(last->text.data(), last->text.length() + text.length());
                        return last;
                }
                Inline *inl = _deck.arena().make<Inline>();
                if (inl == nullptr) {
                        _error = Error::NoMemory;
                        return nullptr;
                }
                inl->type = type;
                inl->style = style;
                inl->text = text.isEmpty() ? text : copy(text);
                *tail = inl;
                return inl;
        }

        void Builder::addText(StringView text) {
                addInline(_code > 0 ? InlineType::Code : InlineType::Text, text);
                return;
        }

        void Builder::addDirective(StringView name, StringView value, bool spot, uint32_t line) {
                RawDirective *d = _deck.arena().make<RawDirective>();
                if (d == nullptr) {
                        _error = Error::NoMemory;
                        return;
                }
                d->name = copy(name);
                d->value = copy(value);
                d->spot = spot;
                d->line = line;
                *_directiveTail = d;
                _directiveTail = &d->next;
                return;
        }

        void Builder::addNote(StringView text, uint32_t line) {
                if (text.isEmpty()) return;
                Note *n = _deck.arena().make<Note>();
                if (n == nullptr) {
                        _error = Error::NoMemory;
                        return;
                }
                n->text = copy(text);
                n->line = line;
                *_noteTail = n;
                _noteTail = &n->next;
                return;
        }

        void Builder::comment(StringView body, uint32_t line) {
                StringView t = body.trimmed();
                if (t.isEmpty()) return;
                if (t.equals("fit")) {
                        Block *b = _levels[_depth].block;
                        if (b != nullptr && b->type == BlockType::Heading) {
                                b->flags |= BlockFlag::Fit;
                        } else {
                                _deck.report(Severity::Warning, line,
                                             "`fit` belongs in a heading: # <!-- fit --> Title");
                        }
                        return;
                }
                if (t.equals("interactive") || t.equals("_interactive")) {
                        addDirective("interactive", StringView(), t.startsWith("_"), line);
                        return;
                }

                // Directives if every line is one; a note otherwise.
                bool       allDirectives = true;
                bool       allLookLikeDirectives = true;
                StringView unknown;
                forEachLine(t, [&](StringView l, uint32_t) {
                        StringView name, value;
                        bool       spot = false;
                        if (l.trimmed().isEmpty()) return;
                        if (!parseDirectiveLine(l, &name, &value, &spot)) {
                                allDirectives = allLookLikeDirectives = false;
                                return;
                        }
                        if (directiveScope(name) == DirectiveScope::Unknown) {
                                allDirectives = false;
                                if (name[0] < 'a' || name[0] > 'z') allLookLikeDirectives = false;
                                if (unknown.isEmpty()) unknown = name;
                        }
                });
                if (allDirectives) {
                        forEachLine(t, [&](StringView l, uint32_t offset) {
                                StringView name, value;
                                bool       spot = false;
                                if (!parseDirectiveLine(l, &name, &value, &spot)) return;
                                uint32_t lineHere = _deck.isInSource(t.data()) ? _deck.lineOf(t.data() + offset) : line;
                                addDirective(name, value, spot, lineHere);
                        });
                        return;
                }
                if (allLookLikeDirectives) {
                        _deck.report(Severity::Warning, line, "unknown directive `%.*s`: the comment is a speaker note",
                                     int(unknown.length()), unknown.data());
                }
                addNote(t, line);
                return;
        }

        // HTML that isn't a comment is shown as text (Marp's default, its `<br>` aside); comments
        // are directives or notes.
        void Builder::htmlText(StringView html, uint32_t line, bool inParagraph) {
                Buffer   rest;
                uint32_t i = 0;
                while (i < html.length()) {
                        int32_t open = html.indexOf("<!--", i);
                        if (open < 0) {
                                rest.append(html.mid(i));
                                break;
                        }
                        rest.append(html.mid(i, uint32_t(open) - i));
                        int32_t  close = html.indexOf("-->", uint32_t(open) + 4);
                        uint32_t end = close < 0 ? html.length() : uint32_t(close);
                        uint32_t lineHere = line;
                        // Lines inside the buffer: count the line ends before the comment.
                        for (uint32_t k = 0; k < uint32_t(open); k++) {
                                if (html[k] == '\n') lineHere++;
                        }
                        comment(html.mid(uint32_t(open) + 4, end - uint32_t(open) - 4), lineHere);
                        i = close < 0 ? html.length() : uint32_t(close) + 3;
                }
                if (rest.hasFailed()) {
                        _error = Error::NoMemory;
                        return;
                }
                StringView shown = rest.view().trimmed();
                if (shown.isEmpty()) return;
                if (inParagraph || _depth + 1 >= MaxDepth) {
                        addText(copy(shown));
                        return;
                }
                if (isBreakTag(shown)) return;
                Block *p = pushBlock(BlockType::Paragraph);
                if (p == nullptr) return;
                p->flags |= BlockFlag::Literal;
                p->line = line;
                bool       ok = false;
                StringView copied = _deck.arena().copy(shown, &ok);
                if (!ok) {
                        _error = Error::NoMemory;
                } else {
                        addInline(InlineType::Text, copied);
                }
                _depth--;
                return;
        }

        void Builder::finishHeading(Block *heading) {
                if (_slide->title.isEmpty()) _slide->title = plainText(heading->inlines, _deck.arena());
                // `Text` then `---` on the next line is a heading in CommonMark, not a slide break:
                // the deck's author almost certainly meant the break.
                if (heading->level != 2) return;
                const char *end = nullptr;
                for (const Inline *inl = heading->inlines; inl != nullptr; inl = inl->next) {
                        if (_deck.isInSource(inl->text.data()) && inl->text.length() > 0) {
                                end = inl->text.data() + inl->text.length();
                        }
                }
                if (end == nullptr) return;
                uint32_t line = _deck.lineOf(end);
                // The heading's last text is on its last line; the underline is the next one.
                const char *p = end;
                while (_deck.isInSource(p) && *p != '\n') p++;
                if (!_deck.isInSource(p)) return;
                p++;
                uint32_t dashes = 0;
                while (_deck.isInSource(p) && (*p == ' ' || *p == '\t')) p++;
                while (_deck.isInSource(p) && *p == '-') {
                        dashes++;
                        p++;
                }
                while (_deck.isInSource(p) && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
                if (dashes >= 3 && (!_deck.isInSource(p) || *p == '\n')) {
                        _deck.report(Severity::Warning, line + 1,
                                     "this `---` makes the line above a heading, not a new slide: put a blank line "
                                     "before it");
                }
                return;
        }

        void Builder::dropEmptyParagraph() {
                // A paragraph left with nothing to show (only comments, or `bg` images) goes.
                Block *p = _levels[_depth].block;
                for (const Inline *inl = p->inlines; inl != nullptr; inl = inl->next) {
                        if (inl->type == InlineType::Image || inl->type == InlineType::LineBreak) return;
                        if ((inl->type == InlineType::Text || inl->type == InlineType::Code) &&
                            !inl->text.trimmed().isEmpty()) {
                                return;
                        }
                }
                // It is its parent's last child: unlinked, the parent's tail is where it was.
                Level  &parent = _levels[_depth - 1];
                Block **link = parent.block != nullptr ? &parent.block->children : &_slide->blocks;
                while (*link != nullptr && *link != p) link = &(*link)->next;
                if (*link == p) {
                        *link = nullptr;
                        parent.childTail = link;
                }
                return;
        }

        int Builder::onEnterBlock(MD_BLOCKTYPE type, void *detail) {
                if (_error != Error::Ok) return 1;
                if (isModelBlock(type) && _depth + 1 >= MaxDepth) {
                        // Too deep: its text goes into the deepest block kept.
                        if (!_warnedDepth) {
                                _deck.report(Severity::Warning, _slide->line,
                                             "blocks nested more than %u deep are flattened", unsigned(MaxDepth - 1));
                                _warnedDepth = true;
                        }
                        _overflow++;
                        return 0;
                }
                switch (type) {
                        case MD_BLOCK_HR:
                                if (_depth == 0) {
                                        if (!startSlide()) return 1;
                                }
                                // A rule inside a list or quote: nothing (it can't be drawn as a break).
                                return 0;
                        case MD_BLOCK_H: {
                                auto *d = static_cast<MD_BLOCK_H_DETAIL *>(detail);
                                if (_divider > 0 && _depth == 0 && d->level <= _divider &&
                                    (_slide->blocks != nullptr || _slide->backgrounds != nullptr)) {
                                        if (!startSlide()) return 1;
                                }
                                Block *b = pushBlock(BlockType::Heading);
                                if (b != nullptr) b->level = uint8_t(d->level);
                                return 0;
                        }
                        case MD_BLOCK_P: pushBlock(BlockType::Paragraph); return 0;
                        case MD_BLOCK_QUOTE: pushBlock(BlockType::Quote); return 0;
                        case MD_BLOCK_UL:
                        case MD_BLOCK_OL: {
                                Block *b = pushBlock(BlockType::List);
                                if (b == nullptr) return 0;
                                _listDepth++;
                                b->level = uint8_t(_listDepth);
                                bool fragmented = false;
                                if (type == MD_BLOCK_UL) {
                                        auto *d = static_cast<MD_BLOCK_UL_DETAIL *>(detail);
                                        fragmented = d->mark == '*';
                                        if (d->is_tight) b->flags |= BlockFlag::Tight;
                                } else {
                                        auto *d = static_cast<MD_BLOCK_OL_DETAIL *>(detail);
                                        b->flags |= BlockFlag::Ordered;
                                        b->start = d->start;
                                        fragmented = d->mark_delimiter == ')';
                                        if (d->is_tight) b->flags |= BlockFlag::Tight;
                                }
                                if (fragmented) b->flags |= BlockFlag::Fragmented;
                                return 0;
                        }
                        case MD_BLOCK_LI: {
                                Block *list = _levels[_depth].block;
                                Block *b = pushBlock(BlockType::ListItem);
                                if (b != nullptr && list != nullptr && list->has(BlockFlag::Fragmented)) {
                                        b->fragment = ++_slide->fragments;
                                }
                                return 0;
                        }
                        case MD_BLOCK_CODE: {
                                auto  *d = static_cast<MD_BLOCK_CODE_DETAIL *>(detail);
                                Block *b = pushBlock(BlockType::Code);
                                if (b == nullptr) return 0;
                                b->info = attribute(d->lang);
                                // `angel run` and `angel run below` (§7.3): the info string's words.
                                StringView info = attribute(d->info);
                                uint32_t   word = 0;
                                bool       angel = false;
                                forEachWord(info, [&](StringView w) {
                                        if (word == 0) angel = w.equals("angel");
                                        if (word == 1 && angel && w.equals("run")) b->flags |= BlockFlag::Run;
                                        if (word == 2 && b->has(BlockFlag::Run) && w.equals("below"))
                                                b->flags |= BlockFlag::RunBelow;
                                        word++;
                                });
                                _code_.clear();
                                _inCodeBlock = true;
                                _codeLine = 0;
                                return 0;
                        }
                        case MD_BLOCK_HTML:
                                _html.clear();
                                _inHtmlBlock = true;
                                _htmlLine = 0;
                                return 0;
                        case MD_BLOCK_TABLE: pushBlock(BlockType::Table); return 0;
                        case MD_BLOCK_THEAD: _levels[_depth].header = true; return 0;
                        case MD_BLOCK_TBODY: _levels[_depth].header = false; return 0;
                        case MD_BLOCK_TR: {
                                bool   header = _levels[_depth].header;
                                Block *b = pushBlock(BlockType::TableRow);
                                if (b != nullptr && header) b->flags |= BlockFlag::Header;
                                return 0;
                        }
                        case MD_BLOCK_TH:
                        case MD_BLOCK_TD: {
                                auto  *d = static_cast<MD_BLOCK_TD_DETAIL *>(detail);
                                Block *b = pushBlock(BlockType::TableCell);
                                if (b == nullptr) return 0;
                                if (type == MD_BLOCK_TH) b->flags |= BlockFlag::Header;
                                b->align = d->align == MD_ALIGN_LEFT     ? Align::Left
                                           : d->align == MD_ALIGN_CENTER ? Align::Center
                                           : d->align == MD_ALIGN_RIGHT  ? Align::Right
                                                                         : Align::Default;
                                return 0;
                        }
                        default: return 0;
                }
        }

        int Builder::onLeaveBlock(MD_BLOCKTYPE type, void *) {
                if (_error != Error::Ok) return 1;
                if (isModelBlock(type) && _overflow > 0) {
                        _overflow--;
                        return 0;
                }
                if (type == MD_BLOCK_DOC || type == MD_BLOCK_HR || type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY) {
                        return 0;
                }
                if (type == MD_BLOCK_HTML) {
                        _inHtmlBlock = false;
                        if (_html.hasFailed()) {
                                _error = Error::NoMemory;
                                return 1;
                        }
                        htmlText(_html.view(), _htmlLine, false);
                        return _error == Error::Ok ? 0 : 1;
                }
                if (_depth == 0) return 0;
                Block *b = _levels[_depth].block;
                if (type == MD_BLOCK_CODE) {
                        _inCodeBlock = false;
                        StringView text = _code_.view();
                        if (text.endsWith("\n")) text = text.left(text.length() - 1);
                        if (_code_.hasFailed()) {
                                _error = Error::NoMemory;
                                return 1;
                        }
                        bool ok = false;
                        b->text = _deck.arena().copy(text, &ok);
                        if (!ok) _error = Error::NoMemory;
                        if (b->line == 0) b->line = _codeLine;
                } else if (type == MD_BLOCK_H) {
                        finishHeading(b);
                } else if (type == MD_BLOCK_P) {
                        dropEmptyParagraph();
                } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
                        _listDepth--;
                }
                _depth--;
                return _error == Error::Ok ? 0 : 1;
        }

        int Builder::onEnterSpan(MD_SPANTYPE type, void *detail) {
                if (_error != Error::Ok) return 1;
                if (_imageDepth > 0) {
                        if (type == MD_SPAN_IMG) _imageDepth++;
                        return 0;
                }
                switch (type) {
                        case MD_SPAN_EM: _italic++; break;
                        case MD_SPAN_STRONG: _bold++; break;
                        case MD_SPAN_DEL: _strike++; break;
                        case MD_SPAN_A: _link++; break;
                        case MD_SPAN_CODE: _code++; break;
                        case MD_SPAN_IMG: {
                                auto *d = static_cast<MD_SPAN_IMG_DETAIL *>(detail);
                                _imageDepth = 1;
                                _alt.clear();
                                _imagePath = attribute(d->src);
                                _imageLine = _deck.lineOf(d->src.text);
                                if (_imageLine == 0)
                                        _imageLine = _levels[_depth].block != nullptr ? _levels[_depth].block->line : 0;
                                noteLine(d->src.text);
                                break;
                        }
                        default: break;
                }
                return 0;
        }

        int Builder::onLeaveSpan(MD_SPANTYPE type) {
                if (_error != Error::Ok) return 1;
                if (_imageDepth > 0) {
                        if (type != MD_SPAN_IMG || --_imageDepth > 0) return 0;
                        if (_alt.hasFailed()) {
                                _error = Error::NoMemory;
                                return 1;
                        }
                        Image *image = _deck.arena().make<Image>();
                        bool   ok = false;
                        // The alt text is copied first: the image's words point into it.
                        StringView alt = image != nullptr ? _deck.arena().copy(_alt.view(), &ok) : StringView();
                        if (!ok) {
                                _error = Error::NoMemory;
                                return 1;
                        }
                        image->path = _imagePath;
                        image->line = _imageLine;
                        parseImageKeywords(_deck, alt, *image);
                        if (image->isBackground()) {
                                *_backgroundTail = image;
                                _backgroundTail = &image->next;
                        } else {
                                Inline *inl = addInline(InlineType::Image, StringView());
                                if (inl != nullptr) inl->image = image;
                        }
                        return _error == Error::Ok ? 0 : 1;
                }
                switch (type) {
                        case MD_SPAN_EM: _italic--; break;
                        case MD_SPAN_STRONG: _bold--; break;
                        case MD_SPAN_DEL: _strike--; break;
                        case MD_SPAN_A: _link--; break;
                        case MD_SPAN_CODE: _code--; break;
                        default: break;
                }
                return 0;
        }

        int Builder::onText(MD_TEXTTYPE type, StringView text) {
                if (_error != Error::Ok) return 1;
                char utf8[8];
                if (_inHtmlBlock) {
                        if (_htmlLine == 0) _htmlLine = _deck.lineOf(text.data());
                        _html.append(text);
                        return 0;
                }
                if (_inCodeBlock) {
                        if (_codeLine == 0) _codeLine = _deck.lineOf(text.data());
                        _code_.append(text);
                        return 0;
                }
                if (_imageDepth > 0) {
                        if (type == MD_TEXT_ENTITY) {
                                StringView t = translateEntity(text, utf8);
                                _alt.append(t.isEmpty() ? text : t);
                        } else if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR) {
                                _alt.append(' ');
                        } else if (type == MD_TEXT_NULLCHAR) {
                                _alt.append(Replacement);
                        } else {
                                _alt.append(text);
                        }
                        return 0;
                }
                if (_inComment) {
                        _html.append(text);
                        StringView all = _html.view();
                        if (all.indexOf("-->") >= 0) {
                                _inComment = false;
                                if (_html.hasFailed()) {
                                        _error = Error::NoMemory;
                                        return 1;
                                }
                                htmlText(all, _commentLine, true);
                        }
                        return _error == Error::Ok ? 0 : 1;
                }
                noteLine(text.data());
                switch (type) {
                        case MD_TEXT_NORMAL:
                        case MD_TEXT_CODE:
                        case MD_TEXT_LATEXMATH: addText(text); break;
                        case MD_TEXT_NULLCHAR: addText(copy(Replacement)); break;
                        case MD_TEXT_ENTITY: {
                                StringView t = translateEntity(text, utf8);
                                addText(t.isEmpty() ? text : copy(t));
                                break;
                        }
                        case MD_TEXT_BR: addInline(InlineType::LineBreak, StringView()); break;
                        case MD_TEXT_SOFTBR: addInline(InlineType::SoftBreak, StringView()); break;
                        case MD_TEXT_HTML:
                                if (text.startsWith("<!--")) {
                                        _commentLine = _deck.lineOf(text.data());
                                        _html.clear();
                                        _html.append(text);
                                        if (text.indexOf("-->", 4) >= 0) {
                                                htmlText(_html.view(), _commentLine, true);
                                        } else {
                                                _inComment = true;
                                        }
                                } else if (isBreakTag(text)) {
                                        addInline(InlineType::LineBreak, StringView());
                                } else {
                                        addText(text);
                                }
                                break;
                }
                return _error == Error::Ok ? 0 : 1;
        }

        int Builder::enterBlock(MD_BLOCKTYPE type, void *detail, void *self) {
                return static_cast<Builder *>(self)->onEnterBlock(type, detail);
        }
        int Builder::leaveBlock(MD_BLOCKTYPE type, void *detail, void *self) {
                return static_cast<Builder *>(self)->onLeaveBlock(type, detail);
        }
        int Builder::enterSpan(MD_SPANTYPE type, void *detail, void *self) {
                return static_cast<Builder *>(self)->onEnterSpan(type, detail);
        }
        int Builder::leaveSpan(MD_SPANTYPE type, void *, void *self) {
                return static_cast<Builder *>(self)->onLeaveSpan(type);
        }
        int Builder::text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *self) {
                return static_cast<Builder *>(self)->onText(type, StringView(text, size));
        }

        Error Builder::run(StringView body) {
                if (!startSlide()) return _error;
                MD_PARSER parser = {};
                parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH;
                parser.enter_block = enterBlock;
                parser.leave_block = leaveBlock;
                parser.enter_span = enterSpan;
                parser.leave_span = leaveSpan;
                parser.text = text;
                int result = md_parse(body.data(), body.length(), &parser, this);
                if (_error != Error::Ok) return _error;
                if (result != 0) return Error::NoMemory; // md4c's only failure is memory
                if (_alt.hasFailed() || _html.hasFailed() || _code_.hasFailed()) return Error::NoMemory;
                return Error::Ok;
        }

        Error parseMarkdown(Deck &deck, StringView body, uint8_t headingDivider) {
                Builder builder(deck, headingDivider);
                return builder.run(body);
        }

} // namespace preso
