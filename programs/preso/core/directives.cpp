/**
 * @file      directives.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The words of a deck (devplan/preso.md §4): directive names and lines, images' keywords,
 * entities, times.
 */

#include "deck.h"

namespace preso {

        namespace {

                struct DirectiveName {
                                const char    *name;
                                DirectiveScope scope;
                };

                // Marp's directives, then ours. Marp's metadata (marp, lang, image, keywords, url)
                // is known so it isn't taken for a note; style and math are known to be reported.
                constexpr DirectiveName Directives[] = {
                        {"theme", DirectiveScope::Global},
                        {"size", DirectiveScope::Global},
                        {"headingDivider", DirectiveScope::Global},
                        {"title", DirectiveScope::Global},
                        {"author", DirectiveScope::Global},
                        {"description", DirectiveScope::Global},
                        {"marp", DirectiveScope::Global},
                        {"lang", DirectiveScope::Global},
                        {"image", DirectiveScope::Global},
                        {"keywords", DirectiveScope::Global},
                        {"url", DirectiveScope::Global},
                        {"style", DirectiveScope::Global},
                        {"math", DirectiveScope::Global},
                        {"duration", DirectiveScope::Global},
                        {"paginate", DirectiveScope::Local},
                        {"header", DirectiveScope::Local},
                        {"footer", DirectiveScope::Local},
                        {"class", DirectiveScope::Local},
                        {"backgroundColor", DirectiveScope::Local},
                        {"backgroundImage", DirectiveScope::Local},
                        {"backgroundPosition", DirectiveScope::Local},
                        {"backgroundRepeat", DirectiveScope::Local},
                        {"backgroundSize", DirectiveScope::Local},
                        {"color", DirectiveScope::Local},
                        {"transition", DirectiveScope::Local},
                        {"align", DirectiveScope::Local},
                        {"auto", DirectiveScope::Local},
                        {"run", DirectiveScope::SlideOnly},
                        {"sound", DirectiveScope::SlideOnly},
                        {"interactive", DirectiveScope::SlideOnly},
                };

                bool isNameStart(char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                }
                bool isNameChar(char c) {
                        return isNameStart(c) || (c >= '0' && c <= '9') || c == '_' || c == '-';
                }

                StringView unquote(StringView value) {
                        if (value.length() >= 2 && ((value[0] == '"' && value[value.length() - 1] == '"') ||
                                                    (value[0] == '\'' && value[value.length() - 1] == '\''))) {
                                return value.mid(1, value.length() - 2);
                        }
                        return value;
                }

                // A size in pixels: `200`, `200px`; other units aren't (Preso's slides are pixels).
                bool parsePixels(StringView text, uint16_t *out) {
                        if (text.endsWith("px")) text = text.left(text.length() - 2);
                        int32_t v = text.toInt(-1);
                        if (v <= 0 || v > 8192) return false;
                        *out = uint16_t(v);
                        return true;
                }

                bool parsePercent(StringView text, uint32_t max, uint32_t *out) {
                        if (!text.endsWith("%")) return false;
                        float v = text.left(text.length() - 1).toFloat(-1.0f);
                        if (v <= 0.0f || v > float(max)) return false;
                        *out = uint32_t(v + 0.5f);
                        return true;
                }

                // Marp's filters Preso doesn't draw (yet): known, so they aren't alt text, and said so.
                bool isOtherFilter(StringView name) {
                        static const char *const names[] = {"blur",       "brightness", "contrast", "drop-shadow",
                                                            "hue-rotate", "invert",     "opacity",  "saturate"};
                        for (const char *n : names) {
                                if (name.equals(n)) return true;
                        }
                        return false;
                }

                void appendUtf8(char *out, uint32_t *n, uint32_t cp) {
                        if (cp < 0x80) {
                                out[(*n)++] = char(cp);
                        } else if (cp < 0x800) {
                                out[(*n)++] = char(0xC0 | (cp >> 6));
                                out[(*n)++] = char(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                                out[(*n)++] = char(0xE0 | (cp >> 12));
                                out[(*n)++] = char(0x80 | ((cp >> 6) & 0x3F));
                                out[(*n)++] = char(0x80 | (cp & 0x3F));
                        } else {
                                out[(*n)++] = char(0xF0 | (cp >> 18));
                                out[(*n)++] = char(0x80 | ((cp >> 12) & 0x3F));
                                out[(*n)++] = char(0x80 | ((cp >> 6) & 0x3F));
                                out[(*n)++] = char(0x80 | (cp & 0x3F));
                        }
                        return;
                }

                struct Entity {
                                const char *name;
                                uint32_t    codePoint;
                };

                // The entities a slide is likely to use; others stay as written.
                constexpr Entity Entities[] = {
                        {"amp", '&'},      {"lt", '<'},        {"gt", '>'},       {"quot", '"'},      {"apos", '\''},
                        {"nbsp", 0xA0},    {"copy", 0xA9},     {"reg", 0xAE},     {"trade", 0x2122},  {"mdash", 0x2014},
                        {"ndash", 0x2013}, {"hellip", 0x2026}, {"middot", 0xB7},  {"bull", 0x2022},   {"rarr", 0x2192},
                        {"larr", 0x2190},  {"uarr", 0x2191},   {"darr", 0x2193},  {"harr", 0x2194},   {"rArr", 0x21D2},
                        {"times", 0xD7},   {"divide", 0xF7},   {"deg", 0xB0},     {"plusmn", 0xB1},   {"laquo", 0xAB},
                        {"raquo", 0xBB},   {"lsquo", 0x2018},  {"rsquo", 0x2019}, {"ldquo", 0x201C},  {"rdquo", 0x201D},
                        {"euro", 0x20AC},  {"pound", 0xA3},    {"yen", 0xA5},     {"cent", 0xA2},     {"sect", 0xA7},
                        {"para", 0xB6},    {"micro", 0xB5},    {"le", 0x2264},    {"ge", 0x2265},     {"ne", 0x2260},
                        {"infin", 0x221E}, {"check", 0x2713},  {"star", 0x2606},  {"hearts", 0x2665},
                };

        } // namespace

        DirectiveScope directiveScope(StringView name) {
                for (const DirectiveName &d : Directives) {
                        if (name.equals(d.name)) return d.scope;
                }
                return DirectiveScope::Unknown;
        }

        bool parseDirectiveLine(StringView line, StringView *name, StringView *value, bool *spot) {
                StringView t = line.trimmed();
                uint32_t   i = 0;
                *spot = false;
                if (t.length() > 0 && t[0] == '_') {
                        *spot = true;
                        i = 1;
                }
                uint32_t start = i;
                if (i >= t.length() || !isNameStart(t[i])) return false;
                while (i < t.length() && isNameChar(t[i])) i++;
                *name = t.mid(start, i - start);
                while (i < t.length() && isBlank(t[i])) i++;
                if (i >= t.length() || t[i] != ':') return false;
                // YAML's `key: value` wants a space after the colon (or nothing): `a:b` is a word.
                if (i + 1 < t.length() && !isBlank(t[i + 1])) return false;
                StringView v = t.mid(i + 1).trimmed();
                // A YAML comment after the value (` # ...`), unless the value is quoted.
                if (v.length() > 0 && v[0] != '"' && v[0] != '\'') {
                        int32_t hash = v.indexOf(" #");
                        if (hash >= 0) v = v.left(uint32_t(hash)).trimmed();
                }
                *value = unquote(v);
                return true;
        }

        void parseImageKeywords(Deck &deck, StringView alt, Image &image) {
                const char *altStart = nullptr;
                const char *altEnd = nullptr;
                bool        altContiguous = true;
                forEachWord(alt, [&](StringView word) {
                        uint32_t percent = 0;
                        if (word.equals("bg")) {
                                image.flags |= ImageFlag::Background;
                        } else if (word.equals("script")) {
                                image.flags |= ImageFlag::Script;
                        } else if (word.startsWith("script:") && word.length() > 7) {
                                image.flags |= ImageFlag::Script;
                                image.script = word.mid(7);
                        } else if (word.equals("contain") || word.equals("fit")) {
                                image.fit = ImageFit::Contain;
                        } else if (word.equals("cover")) {
                                image.fit = ImageFit::Cover;
                        } else if (word.equals("auto")) {
                                image.fit = ImageFit::Natural;
                        } else if (word.equals("vertical")) {
                                image.flags |= ImageFlag::Vertical;
                        } else if (word.equals("left") || word.equals("right")) {
                                image.side = word.equals("left") ? ImageSide::Left : ImageSide::Right;
                        } else if (word.startsWith("left:") || word.startsWith("right:")) {
                                bool       left = word.startsWith("left:");
                                StringView share = word.mid(left ? 5 : 6);
                                image.side = left ? ImageSide::Left : ImageSide::Right;
                                if (parsePercent(share, 99, &percent)) {
                                        image.share = uint8_t(percent);
                                } else {
                                        deck.report(Severity::Warning, image.line,
                                                    "image `%.*s`: a share is a percentage", int(word.length()),
                                                    word.data());
                                }
                        } else if (parsePercent(word, 1000, &percent)) {
                                image.fit = ImageFit::Scale;
                                image.scale = uint16_t(percent);
                        } else if (word.startsWith("w:") || word.startsWith("width:")) {
                                StringView v = word.mid(word.startsWith("w:") ? 2 : 6);
                                if (!parsePixels(v, &image.width)) {
                                        deck.report(Severity::Warning, image.line, "image `%.*s`: a width in pixels",
                                                    int(word.length()), word.data());
                                }
                        } else if (word.startsWith("h:") || word.startsWith("height:")) {
                                StringView v = word.mid(word.startsWith("h:") ? 2 : 7);
                                if (!parsePixels(v, &image.height)) {
                                        deck.report(Severity::Warning, image.line, "image `%.*s`: a height in pixels",
                                                    int(word.length()), word.data());
                                }
                        } else if (word.equals("grayscale") || word.startsWith("grayscale:")) {
                                image.flags |= ImageFlag::Grayscale;
                        } else if (word.equals("sepia") || word.startsWith("sepia:")) {
                                image.flags |= ImageFlag::Sepia;
                        } else if (isOtherFilter(word) ||
                                   (word.indexOf(':') > 0 && isOtherFilter(word.left(uint32_t(word.indexOf(':')))))) {
                                deck.report(Severity::Warning, image.line,
                                            "image filter `%.*s` isn't drawn: grayscale and sepia are",
                                            int(word.length()), word.data());
                        } else {
                                // Alt text: the words in between keywords, as one run of the source.
                                if (altStart == nullptr) {
                                        altStart = word.data();
                                } else if (!altContiguous) {
                                        // Only the first run of words is kept.
                                        return;
                                }
                                altEnd = word.data() + word.length();
                                return;
                        }
                        if (altStart != nullptr) altContiguous = false;
                        return;
                });
                if (altStart != nullptr) image.alt = StringView(altStart, uint32_t(altEnd - altStart));
                if (image.isScript() && image.script.isEmpty()) image.script = image.path;
                if (image.isScript() && !image.script.endsWith(".as")) {
                        deck.report(Severity::Warning, image.line,
                                    "a script region's script is an AngelScript file (.as): %.*s",
                                    int(image.script.length()), image.script.data());
                }
                if (!image.isBackground() && image.side != ImageSide::None) {
                        deck.report(Severity::Warning, image.line,
                                    "`left` and `right` split the slide for a `bg` image only");
                }
                return;
        }

        StringView translateEntity(StringView entity, char *utf8) {
                if (entity.length() < 3 || entity[0] != '&' || entity[entity.length() - 1] != ';') return StringView();
                StringView name = entity.mid(1, entity.length() - 2);
                uint32_t   cp = 0;
                if (name.startsWith("#x") || name.startsWith("#X")) {
                        if (name.length() < 3 || name.length() > 8) return StringView();
                        for (uint32_t i = 2; i < name.length(); i++) {
                                char     c = name[i];
                                uint32_t d = c >= '0' && c <= '9'   ? uint32_t(c - '0')
                                             : c >= 'a' && c <= 'f' ? uint32_t(c - 'a' + 10)
                                             : c >= 'A' && c <= 'F' ? uint32_t(c - 'A' + 10)
                                                                    : 99;
                                if (d == 99) return StringView();
                                cp = cp * 16 + d;
                        }
                } else if (name.startsWith("#")) {
                        int32_t v = name.mid(1).toInt(-1);
                        if (v < 0 || name.length() > 8) return StringView();
                        cp = uint32_t(v);
                } else {
                        bool found = false;
                        for (const Entity &e : Entities) {
                                if (name.equals(e.name)) {
                                        cp = e.codePoint;
                                        found = true;
                                        break;
                                }
                        }
                        if (!found) return StringView();
                }
                // CommonMark: 0 and what isn't a character become U+FFFD.
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                uint32_t n = 0;
                appendUtf8(utf8, &n, cp);
                return StringView(utf8, n);
        }

        bool parseDuration(StringView text, uint32_t bareUnitMs, uint32_t *ms) {
                StringView t = text.trimmed();
                if (t.isEmpty()) return false;
                int32_t whole = t.toInt(-1);
                if (whole >= 0) {
                        *ms = uint32_t(whole) * bareUnitMs;
                        return true;
                }
                uint64_t total = 0;
                uint32_t i = 0;
                while (i < t.length()) {
                        uint32_t start = i;
                        while (i < t.length() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.')) i++;
                        if (i == start) return false;
                        float    amount = t.mid(start, i - start).toFloat(-1.0f);
                        uint32_t unitStart = i;
                        while (i < t.length() && t[i] >= 'a' && t[i] <= 'z') i++;
                        StringView unit = t.mid(unitStart, i - unitStart);
                        uint32_t   scale = unit.equals("ms")  ? 1
                                           : unit.equals("s") ? 1000
                                           : unit.equals("m") ? 60000
                                           : unit.equals("h") ? 3600000
                                                              : 0;
                        if (amount < 0.0f || scale == 0) return false;
                        total += uint64_t(amount * float(scale) + 0.5f);
                        while (i < t.length() && isBlank(t[i])) i++;
                }
                if (total > UINT32_MAX) return false;
                *ms = uint32_t(total);
                return true;
        }

} // namespace preso
