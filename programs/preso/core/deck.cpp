/**
 * @file      deck.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The deck: its source and model, the front matter, and the directives resolved slide by slide
 * (devplan/preso.md §4). The Markdown itself is markdown.cpp's.
 */

#include "deck.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "buffer.h"
#include "transitions.h"

namespace preso {

        Deck::~Deck() {
                clear();
        }

        void Deck::clear() {
                _arena.clear();
                free(_source);
                _source = nullptr;
                _length = 0;
                _lineStarts = nullptr;
                _lineCount = 0;
                _settings = DeckSettings();
                _slides = _lastSlide = nullptr;
                _index = nullptr;
                _slideCount = 0;
                _diagnostics = _lastDiagnostic = nullptr;
                _diagnosticCount = 0;
                _outOfMemory = false;
                return;
        }

        const Slide *Deck::slide(uint32_t number) const {
                if (number == 0 || number > _slideCount || _index == nullptr) return nullptr;
                return _index[number - 1];
        }

        Error Deck::indexLines() {
                uint32_t count = 1;
                for (uint32_t i = 0; i < _length; i++) {
                        if (_source[i] == '\n') count++;
                }
                _lineStarts = static_cast<uint32_t *>(_arena.allocate(count * sizeof(uint32_t), alignof(uint32_t)));
                if (_lineStarts == nullptr) return Error::NoMemory;
                _lineStarts[0] = 0;
                uint32_t n = 1;
                for (uint32_t i = 0; i < _length; i++) {
                        if (_source[i] == '\n') _lineStarts[n++] = i + 1;
                }
                _lineCount = count;
                return Error::Ok;
        }

        uint32_t Deck::lineOf(const char *p) const {
                if (!isInSource(p) || _lineCount == 0) return 0;
                uint32_t offset = uint32_t(p - _source);
                uint32_t low = 0;
                uint32_t high = _lineCount; // the line is the last start <= offset
                while (high - low > 1) {
                        uint32_t mid = (low + high) / 2;
                        if (_lineStarts[mid] <= offset) {
                                low = mid;
                        } else {
                                high = mid;
                        }
                }
                return low + 1;
        }

        void Deck::report(Severity severity, uint32_t line, const char *format, ...) {
                char    text[256];
                va_list args;
                va_start(args, format);
                vsnprintf(text, sizeof text, format, args);
                va_end(args);
                bool        ok = false;
                StringView  message = _arena.copy(StringView(text), &ok);
                Diagnostic *d = ok ? _arena.make<Diagnostic>() : nullptr;
                if (d == nullptr) {
                        _outOfMemory = true;
                        return;
                }
                d->severity = severity;
                d->line = line;
                d->message = message;
                if (_lastDiagnostic != nullptr) {
                        _lastDiagnostic->next = d;
                } else {
                        _diagnostics = d;
                }
                _lastDiagnostic = d;
                _diagnosticCount++;
                return;
        }

        Slide *Deck::appendSlide() {
                Slide *s = _arena.make<Slide>();
                if (s == nullptr) {
                        _outOfMemory = true;
                        return nullptr;
                }
                s->number = ++_slideCount;
                if (_lastSlide != nullptr) {
                        _lastSlide->next = s;
                } else {
                        _slides = s;
                }
                _lastSlide = s;
                return s;
        }

        // ---- The front matter: `---`, lines of `name: value`, `---` (Marp's YAML, the part of it
        // that directives use).

        namespace {

                bool isFence(StringView line, const char *fence) {
                        StringView t = line;
                        while (t.length() > 0 && isBlank(t[t.length() - 1])) t = t.left(t.length() - 1);
                        return t.equals(fence);
                }

        } // namespace

        Error Deck::parseFrontMatter(uint32_t *bodyStart, RawDirective **front) {
                *bodyStart = 0;
                *front = nullptr;
                StringView all(_source, _length);
                if (all.startsWith("\xEF\xBB\xBF")) *bodyStart = 3; // a byte order mark
                StringView rest = all.mid(*bodyStart);
                int32_t    firstEnd = rest.indexOf('\n');
                if (firstEnd < 0 || !isFence(rest.left(uint32_t(firstEnd)), "---")) return Error::Ok;

                // Find the closing fence before reading anything: without one it isn't front
                // matter, and the `---` is the first slide's end, as in Marp.
                uint32_t pos = uint32_t(firstEnd) + 1;
                int32_t  closeAt = -1;
                uint32_t closeEnd = 0;
                while (pos <= rest.length()) {
                        int32_t    end = rest.indexOf('\n', pos);
                        uint32_t   stop = end < 0 ? rest.length() : uint32_t(end);
                        StringView line = rest.mid(pos, stop - pos);
                        if (line.endsWith("\r")) line = line.left(line.length() - 1);
                        if (isFence(line, "---") || isFence(line, "...")) {
                                closeAt = int32_t(pos);
                                closeEnd = end < 0 ? rest.length() : stop + 1;
                                break;
                        }
                        if (end < 0) break;
                        pos = stop + 1;
                }
                if (closeAt < 0) {
                        report(Severity::Warning, 1, "the front matter has no closing `---`: read as a slide break");
                        return Error::Ok;
                }

                StringView     body = rest.mid(uint32_t(firstEnd) + 1, uint32_t(closeAt) - uint32_t(firstEnd) - 1);
                RawDirective **tail = front;
                bool           inBlockScalar = false;
                Error          error = Error::Ok;
                forEachLine(body, [&](StringView line, uint32_t) {
                        if (error != Error::Ok) return;
                        uint32_t lineNumber = lineOf(line.data());
                        // An indented line after `name: |` is that value's: not ours to read.
                        if (inBlockScalar && line.length() > 0 && isBlank(line[0])) return;
                        inBlockScalar = false;
                        StringView t = line.trimmed();
                        if (t.isEmpty() || t.startsWith("#")) return;
                        StringView name, value;
                        bool       spot = false;
                        if (!parseDirectiveLine(t, &name, &value, &spot)) {
                                report(Severity::Warning, lineNumber, "not a directive in the front matter: %.*s",
                                       int(t.length()), t.data());
                                return;
                        }
                        if (value.equals("|") || value.equals(">") || value.equals("|-") || value.equals(">-")) {
                                inBlockScalar = true;
                        }
                        RawDirective *d = _arena.make<RawDirective>();
                        if (d == nullptr) {
                                error = Error::NoMemory;
                                return;
                        }
                        d->name = name;
                        d->value = value;
                        d->spot = spot;
                        d->line = lineNumber;
                        *tail = d;
                        tail = &d->next;
                });
                *bodyStart += closeEnd;
                return error;
        }

        // ---- Resolving the directives

        namespace {

                bool parseBool(StringView value, bool *out) {
                        if (value.equalsIgnoringCase("true") || value.equalsIgnoringCase("yes") ||
                            value.equalsIgnoringCase("on")) {
                                *out = true;
                                return true;
                        }
                        if (value.equalsIgnoringCase("false") || value.equalsIgnoringCase("no") ||
                            value.equalsIgnoringCase("off")) {
                                *out = false;
                                return true;
                        }
                        return false;
                }

                bool validSize(StringView value) {
                        if (value.equals("16:9") || value.equals("4:3")) return true;
                        int32_t x = value.indexOf('x');
                        if (x <= 0) return false;
                        int32_t w = value.left(uint32_t(x)).toInt(-1);
                        int32_t h = value.mid(uint32_t(x) + 1).toInt(-1);
                        return w >= 64 && w <= 4096 && h >= 64 && h <= 4096;
                }

        } // namespace

        void Deck::applyGlobal(const RawDirective &d, bool fromFrontMatter) {
                if (d.spot) {
                        report(Severity::Warning, d.line, "`%.*s` is the deck's: `_%.*s` is the same as `%.*s`",
                               int(d.name.length()), d.name.data(), int(d.name.length()), d.name.data(),
                               int(d.name.length()), d.name.data());
                }
                if (d.name.equals("theme")) {
                        _settings.theme = d.value;
                } else if (d.name.equals("size")) {
                        if (validSize(d.value)) {
                                _settings.size = d.value;
                        } else {
                                report(Severity::Warning, d.line, "size `%.*s`: 16:9, 4:3 or WIDTHxHEIGHT",
                                       int(d.value.length()), d.value.data());
                        }
                } else if (d.name.equals("title")) {
                        _settings.title = d.value;
                } else if (d.name.equals("author")) {
                        _settings.author = d.value;
                } else if (d.name.equals("description")) {
                        _settings.description = d.value;
                } else if (d.name.equals("duration")) {
                        uint32_t ms = 0;
                        if (parseDuration(d.value, 60000, &ms)) {
                                _settings.durationSeconds = ms / 1000;
                        } else {
                                report(Severity::Warning, d.line, "duration `%.*s`: a time, as 25m or 1h30m",
                                       int(d.value.length()), d.value.data());
                        }
                } else if (d.name.equals("headingDivider")) {
                        // Read before the Markdown (parse()); elsewhere it would split slides already made.
                        if (!fromFrontMatter) {
                                report(Severity::Warning, d.line, "headingDivider works in the front matter only");
                        }
                } else if (d.name.equals("style") || d.name.equals("math")) {
                        report(Severity::Warning, d.line, "`%.*s` isn't supported: themes are Preso's own (§4)",
                               int(d.name.length()), d.name.data());
                }
                // marp, lang, image, keywords, url: Marp's metadata, nothing for Preso to do.
                return;
        }

        void Deck::resolve(RawDirective *front) {
                SlideSettings inherited;
                // The front matter's directives come first, as if written on the first slide.
                auto apply = [&](const RawDirective &d, SlideSettings &here, Slide &slide, bool fromFront) {
                        DirectiveScope scope = directiveScope(d.name);
                        if (scope == DirectiveScope::Global) {
                                applyGlobal(d, fromFront);
                                return;
                        }
                        if (scope == DirectiveScope::SlideOnly) {
                                bool on = true;
                                if (d.name.equals("run")) {
                                        slide.run = d.value;
                                } else if (d.name.equals("sound")) {
                                        slide.sound = d.value;
                                } else if (d.name.equals("interactive")) {
                                        if (!d.value.isEmpty() && !parseBool(d.value, &on)) {
                                                report(Severity::Warning, d.line, "interactive: true or false");
                                        }
                                        slide.interactive = on;
                                }
                                return;
                        }
                        // Local: into this slide's settings, and on into the next unless `_`.
                        auto set = [&](SlideSettings &s) {
                                const StringView &n = d.name;
                                if (n.equals("class")) {
                                        s.className = d.value;
                                } else if (n.equals("header")) {
                                        s.header = d.value;
                                } else if (n.equals("footer")) {
                                        s.footer = d.value;
                                } else if (n.equals("backgroundColor")) {
                                        s.backgroundColor = d.value;
                                } else if (n.equals("backgroundImage")) {
                                        s.backgroundImage = d.value;
                                } else if (n.equals("backgroundPosition")) {
                                        s.backgroundPosition = d.value;
                                } else if (n.equals("backgroundRepeat")) {
                                        s.backgroundRepeat = d.value;
                                } else if (n.equals("backgroundSize")) {
                                        s.backgroundSize = d.value;
                                } else if (n.equals("color")) {
                                        s.color = d.value;
                                } else if (n.equals("transition")) {
                                        s.transition = d.value;
                                } else if (n.equals("align")) {
                                        s.align = d.value;
                                } else if (n.equals("paginate")) {
                                        bool on = false;
                                        if (parseBool(d.value, &on)) s.paginate = on;
                                } else if (n.equals("auto")) {
                                        uint32_t ms = 0;
                                        if (parseDuration(d.value, 1000, &ms)) s.autoMs = ms;
                                }
                                return;
                        };
                        // Checked once, where it is written.
                        if (d.name.equals("paginate")) {
                                bool on = false;
                                if (!parseBool(d.value, &on))
                                        report(Severity::Warning, d.line, "paginate: true or false");
                        } else if (d.name.equals("auto")) {
                                uint32_t ms = 0;
                                if (!parseDuration(d.value, 1000, &ms)) {
                                        report(Severity::Warning, d.line, "auto `%.*s`: a time, as 10s or 1m",
                                               int(d.value.length()), d.value.data());
                                }
                        } else if (d.name.equals("align")) {
                                if (!d.value.equals("top") && !d.value.equals("middle") && !d.value.isEmpty()) {
                                        report(Severity::Warning, d.line, "align `%.*s`: top or middle",
                                               int(d.value.length()), d.value.data());
                                }
                        } else if (d.name.equals("transition")) {
                                // Marp's own names (fade, slide...) aren't ours, and are reported.
                                Transition  t;
                                const char *why = "";
                                if (!parseTransition(d.value, &t, &why)) {
                                        report(Severity::Warning, d.line, "transition `%.*s` %s", int(d.value.length()),
                                               d.value.data(), why);
                                }
                        }
                        set(here);
                        if (!d.spot) set(inherited);
                        return;
                };

                for (Slide *slide = _slides; slide != nullptr; slide = slide->next) {
                        // Locals written on this slide apply to it whether above or below its text,
                        // so the slide starts from what it inherits, then takes its own in order.
                        SlideSettings here = inherited;
                        if (slide == _slides) {
                                for (RawDirective *d = front; d != nullptr; d = d->next) apply(*d, here, *slide, true);
                        }
                        for (RawDirective *d = slide->directives; d != nullptr; d = d->next)
                                apply(*d, here, *slide, false);
                        slide->settings = here;
                }
                return;
        }

        Error Deck::parse(char *source, uint32_t length) {
                clear();
                if (length > MaxSource) {
                        free(source);
                        return Error::TooBig;
                }
                _source = source;
                _length = length;
                Error error = indexLines();
                if (error != Error::Ok) {
                        clear();
                        return error;
                }

                uint32_t      bodyStart = 0;
                RawDirective *front = nullptr;
                error = parseFrontMatter(&bodyStart, &front);
                if (error != Error::Ok) {
                        clear();
                        return error;
                }
                uint8_t divider = 0;
                for (RawDirective *d = front; d != nullptr; d = d->next) {
                        if (!d->name.equals("headingDivider")) continue;
                        int32_t level = d->value.toInt(-1);
                        if (level >= 1 && level <= 6) {
                                divider = uint8_t(level);
                        } else {
                                report(Severity::Warning, d->line, "headingDivider `%.*s`: a heading level, 1 to 6",
                                       int(d->value.length()), d->value.data());
                        }
                }
                _settings.headingDivider = divider;

                error = parseMarkdown(*this, StringView(_source + bodyStart, _length - bodyStart), divider);
                if (error == Error::Ok && _outOfMemory) error = Error::NoMemory;
                if (error != Error::Ok) {
                        clear();
                        return error;
                }

                // Unknown directive names in the front matter (comments' are notes: markdown.cpp).
                for (RawDirective *d = front; d != nullptr; d = d->next) {
                        if (directiveScope(d->name) == DirectiveScope::Unknown) {
                                report(Severity::Warning, d->line, "unknown directive `%.*s`", int(d->name.length()),
                                       d->name.data());
                        }
                }
                resolve(front);

                _index = static_cast<Slide **>(_arena.allocate(sizeof(Slide *) * _slideCount, alignof(Slide *)));
                if (_index == nullptr || _outOfMemory) {
                        clear();
                        return Error::NoMemory;
                }
                uint32_t i = 0;
                for (Slide *s = _slides; s != nullptr; s = s->next) _index[i++] = s;
                return Error::Ok;
        }

} // namespace preso
