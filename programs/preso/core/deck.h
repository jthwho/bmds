/**
 * @file      deck.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A deck (devplan/preso.md §4): Marp's Markdown parsed into slides, each a tree of blocks and
 * inline runs, with its directives resolved, its images' keywords read, its notes kept. The
 * model lives in the deck's arena and points into the deck's source wherever it can.
 */

#ifndef PRESO_DECK_H
#define PRESO_DECK_H

#include "arena.h"
#include "types.h"

namespace preso {

        // ---- Images

        /** @brief How an image fills its place (Marp's `contain`, `cover`, a percentage). */
        enum class ImageFit : uint8_t {
                Auto,    ///< Nothing said: a background `cover`, inline its own size (or `w:`/`h:`).
                Contain, ///< `contain` (Marp's `fit` too): all of it, letterboxed.
                Cover,   ///< `cover`: the whole place, cropped.
                Scale,   ///< `50%`: its own size scaled (`Image::scale`).
                Natural, ///< `auto`: its own size.
        };

        /** @brief A background's side of a split slide (`bg left`, `bg right:40%`). */
        enum class ImageSide : uint8_t {
                None,
                Left,
                Right
        };

        /** @brief `Image::flags`. */
        namespace ImageFlag {
                constexpr uint8_t Background = 0x01; ///< `bg`: behind the slide, not in its text.
                constexpr uint8_t Script = 0x02;     ///< `script`: a script region (§7.2).
                constexpr uint8_t Vertical = 0x04;   ///< `bg vertical`: several backgrounds stacked.
                constexpr uint8_t Grayscale = 0x08;  ///< Marp's `grayscale` filter.
                constexpr uint8_t Sepia = 0x10;      ///< Marp's `sepia` filter.
        } // namespace ImageFlag

        /** @brief An image in a slide, its alt text's keywords read (§4). */
        struct Image {
                        StringView path;   ///< As written: relative to the deck's folder.
                        StringView script; ///< A script region's script (`path`, or `script:FILE`'s).
                        StringView alt;    ///< The alt text's words that aren't keywords.
                        uint32_t   line = 0;
                        uint16_t   width = 0;   ///< `w:` in pixels; 0: none.
                        uint16_t   height = 0;  ///< `h:` in pixels; 0: none.
                        uint16_t   scale = 100; ///< Percent, for ImageFit::Scale.
                        uint8_t    share = 0;   ///< A split's share of the slide in percent; 0: half.
                        ImageFit   fit = ImageFit::Auto;
                        ImageSide  side = ImageSide::None;
                        uint8_t    flags = 0;
                        Image     *next = nullptr; ///< The slide's next background.

                        bool isBackground() const { return (flags & ImageFlag::Background) != 0; }
                        bool isScript() const { return (flags & ImageFlag::Script) != 0; }
        };

        // ---- Inline runs

        enum class InlineType : uint8_t {
                Text,
                Code,      ///< `code`.
                SoftBreak, ///< A line end inside a paragraph: drawn as a space.
                LineBreak, ///< A hard break (two spaces, a backslash, `<br>`).
                Image,     ///< An inline image (`Inline::image`).
        };

        /** @brief `Inline::style` (bits). */
        namespace InlineStyle {
                constexpr uint8_t Bold = 0x1;
                constexpr uint8_t Italic = 0x2;
                constexpr uint8_t Strike = 0x4;
                constexpr uint8_t Link = 0x8; ///< Shown as a link, not followed.
        } // namespace InlineStyle

        struct Inline {
                        InlineType type = InlineType::Text;
                        uint8_t    style = 0;
                        StringView text;
                        Image     *image = nullptr;
                        Inline    *next = nullptr;
        };

        // ---- Blocks

        enum class BlockType : uint8_t {
                Paragraph, ///< Inlines.
                Heading,   ///< Inlines; `level` 1-6.
                List,      ///< ListItem children.
                ListItem,  ///< Block children.
                Quote,     ///< Block children.
                Code,      ///< `text`, `info`.
                Table,     ///< TableRow children.
                TableRow,  ///< TableCell children.
                TableCell, ///< Inlines; `align`.
        };

        /** @brief `Block::flags`. */
        namespace BlockFlag {
                constexpr uint16_t Ordered = 0x01;    ///< A List: numbered.
                constexpr uint16_t Fragmented = 0x02; ///< A List: its items appear one at a time (`*`, `1)`).
                constexpr uint16_t Tight = 0x04;      ///< A List: no blank lines between items.
                constexpr uint16_t Fit = 0x08;        ///< A Heading: `<!-- fit -->`, scaled to the width.
                constexpr uint16_t Run = 0x10;        ///< A Code block: `angel run`, run on the next step (§7.3).
                constexpr uint16_t RunBelow = 0x20;   ///< With Run: the output under the code, not beside it.
                constexpr uint16_t Header = 0x40;     ///< A TableRow or TableCell of the header.
                constexpr uint16_t Literal = 0x80;    ///< A Paragraph of HTML, shown as text.
        } // namespace BlockFlag

        enum class Align : uint8_t {
                Default,
                Left,
                Center,
                Right
        };

        struct Block {
                        BlockType  type = BlockType::Paragraph;
                        uint8_t    level = 0;              ///< A Heading's level; a List's depth (1 the outermost).
                        Align      align = Align::Default; ///< A TableCell's.
                        uint16_t   flags = 0;
                        uint16_t   fragment = 0; ///< A ListItem's step (1...), 0: shown from the start.
                        uint32_t   start = 1;    ///< An ordered List's first number.
                        uint32_t   line = 0;
                        StringView info; ///< A Code block's language (its info string's first word).
                        StringView text; ///< A Code block's text (no final line end).
                        Inline    *inlines = nullptr;
                        Block     *children = nullptr;
                        Block     *next = nullptr;

                        bool has(uint16_t flag) const { return (flags & flag) != 0; }
        };

        // ---- Directives and slides

        /** @brief A directive as written, in the front matter or a comment. */
        struct RawDirective {
                        StringView    name;  ///< Without a leading `_`.
                        StringView    value; ///< Unquoted.
                        uint32_t      line = 0;
                        bool          spot = false; ///< `_name`: this slide only.
                        RawDirective *next = nullptr;
        };

        /** @brief What a slide's directives come to (§4): inherited from slide to slide. */
        struct SlideSettings {
                        StringView className; ///< `class`.
                        StringView header;
                        StringView footer;
                        StringView backgroundColor;
                        StringView backgroundImage; ///< As written (CSS's `url(...)` or a path).
                        StringView backgroundPosition;
                        StringView backgroundRepeat;
                        StringView backgroundSize;
                        StringView color;
                        StringView transition; ///< As written: a name and its arguments.
                        StringView align;      ///< `top` or `middle`; empty: the theme's.
                        uint32_t   autoMs = 0; ///< `auto`: the next step after this long; 0: never.
                        bool       paginate = false;
        };

        struct Note {
                        StringView text;
                        uint32_t   line = 0;
                        Note      *next = nullptr;
        };

        struct Slide {
                        uint32_t      number = 0; ///< From 1.
                        uint32_t      line = 0;   ///< Of its first content; 0: empty.
                        StringView    title;      ///< Its first heading's text; empty if none.
                        SlideSettings settings;
                        StringView    run;                 ///< `run`: a command started on reaching it (§6a).
                        StringView    sound;               ///< `sound`: played as it appears.
                        bool          interactive = false; ///< Its script region takes the keys (§7.5).
                        uint16_t      fragments = 0;       ///< Fragmented list items: steps before the next slide.
                        Block        *blocks = nullptr;
                        Image        *backgrounds = nullptr;
                        Note         *notes = nullptr;
                        RawDirective *directives = nullptr; ///< As written on it, in order.
                        Slide        *next = nullptr;
        };

        /** @brief The deck's own directives (Marp's global ones). */
        struct DeckSettings {
                        StringView theme;
                        StringView size;  ///< `16:9` (the default), `4:3` or `WIDTHxHEIGHT`.
                        StringView title; ///< Metadata, as Marp's.
                        StringView author;
                        StringView description;
                        uint32_t   durationSeconds = 0; ///< `duration`: the talk's length; 0: none.
                        uint8_t headingDivider = 0; ///< `headingDivider`: a heading this level or above starts a slide.
        };

        enum class Severity : uint8_t {
                Warning,
                Error
        };

        /** @brief Something `preso check` reports (§8). */
        struct Diagnostic {
                        Severity    severity = Severity::Warning;
                        uint32_t    line = 0; ///< 0: the deck as a whole.
                        StringView  message;
                        Diagnostic *next = nullptr;
        };

        /**
         * @brief A parsed deck.
         *
         * @par Thread Safety
         * Not thread-safe: one owner. A parsed deck is only read, so readers on several
         * threads are safe once parse() has returned.
         */
        class Deck {
                public:
                        /** @brief The biggest deck file parsed. */
                        static constexpr uint32_t MaxSource = 8u << 20;

                        Deck() = default;
                        ~Deck();
                        Deck(const Deck &) = delete;
                        Deck &operator=(const Deck &) = delete;

                        /**
                         * @brief Parses @p source (@p length bytes of Markdown), taking it over: it
                         *        is free()d with the deck, and the model points into it.
                         *
                         * What can be shown always is: a mistake in the deck is a diagnostic, not
                         * a failure. Fails only for memory (Error::NoMemory) or a deck past
                         * MaxSource (Error::TooBig), and the deck is then empty.
                         */
                        [[nodiscard]] Error parse(char *source, uint32_t length);

                        const DeckSettings &settings() const { return _settings; }
                        const Slide        *slides() const { return _slides; }
                        uint32_t            slideCount() const { return _slideCount; }
                        /** @brief Slide @p number (from 1), or nullptr. */
                        const Slide      *slide(uint32_t number) const;
                        const Diagnostic *diagnostics() const { return _diagnostics; }
                        uint32_t          diagnosticCount() const { return _diagnosticCount; }
                        /** @brief The model's memory, for the budget. */
                        size_t bytesReserved() const { return _arena.bytesReserved(); }

                        // ---- For the parser (deck.cpp, markdown.cpp)
                        Arena &arena() { return _arena; }
                        /** @brief The line (from 1) @p p is on, if it points into the source; else 0. */
                        uint32_t lineOf(const char *p) const;
                        bool     isInSource(const char *p) const {
                                return _source != nullptr && p >= _source && p < _source + _length;
                        }
                        void report(Severity severity, uint32_t line, const char *format, ...)
                                __attribute__((format(printf, 4, 5)));
                        /** @brief A new slide at the end; nullptr if out of memory. */
                        Slide *appendSlide();
                        /** @brief Marks the parse as out of memory (parse() then fails). */
                        void setOutOfMemory() { _outOfMemory = true; }

                private:
                        void  clear();
                        Error indexLines();
                        Error parseFrontMatter(uint32_t *bodyStart, RawDirective **front);
                        void  resolve(RawDirective *front);
                        void  applyGlobal(const RawDirective &d, bool fromFrontMatter);

                        Arena        _arena;
                        char        *_source = nullptr;
                        uint32_t     _length = 0;
                        uint32_t    *_lineStarts = nullptr;
                        uint32_t     _lineCount = 0;
                        DeckSettings _settings;
                        Slide       *_slides = nullptr;
                        Slide       *_lastSlide = nullptr;
                        Slide      **_index = nullptr;
                        uint32_t     _slideCount = 0;
                        Diagnostic  *_diagnostics = nullptr;
                        Diagnostic  *_lastDiagnostic = nullptr;
                        uint32_t     _diagnosticCount = 0;
                        bool         _outOfMemory = false;
        };

        // ---- Shared by the parser's parts (directives.cpp)

        /** @brief Where a directive applies (§4). */
        enum class DirectiveScope : uint8_t {
                Unknown,
                Global,    ///< The deck's, wherever it is written (the last wins).
                Local,     ///< From its slide on (`_name`: its slide only).
                SlideOnly, ///< Its slide only, with or without `_` (`run`, `sound`, `interactive`).
        };

        /** @brief @p name's scope (Unknown if it isn't a directive). */
        DirectiveScope directiveScope(StringView name);

        /**
         * @brief Reads `name: value` (or `_name: value`) from @p line; false if it isn't that
         *        form. The value is unquoted ('...' or "...").
         */
        bool parseDirectiveLine(StringView line, StringView *name, StringView *value, bool *spot);

        /**
         * @brief Reads an image's alt text (Marp's keywords and ours) into @p image; the words
         *        that aren't keywords become its `alt`. Unknown `name:value` words are reported.
         */
        void parseImageKeywords(Deck &deck, StringView alt, Image &image);

        /** @brief The UTF-8 for an HTML entity (`&amp;`, `&#65;`), or an empty view if unknown. */
        StringView translateEntity(StringView entity, char *utf8 /* 8 bytes */);

        /** @brief A time (`25m`, `90s`, `1h30m`, `500ms`; a bare number in @p bareUnitMs units). */
        bool parseDuration(StringView text, uint32_t bareUnitMs, uint32_t *ms);

        /** @brief Parses the Markdown of @p body (in the deck's source) into the deck's slides. */
        Error parseMarkdown(Deck &deck, StringView body, uint8_t headingDivider);

} // namespace preso

#endif // PRESO_DECK_H
