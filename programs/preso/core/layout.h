/**
 * @file      layout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A slide laid out in a theme (devplan/preso.md §6): a list of things to draw, each with the
 * step it appears at, so drawing a step is drawing the list up to it. Laid out once a slide;
 * drawn for each of its steps.
 */

#ifndef PRESO_LAYOUT_H
#define PRESO_LAYOUT_H

#include "canvas.h"
#include "deck.h"
#include "fonts.h"
#include "images.h"
#include "scripts.h"
#include "theme.h"

namespace preso {

        enum class OpType : uint8_t {
                Fill,    ///< A rectangle of `color` (`alpha` of it over what is there).
                Frame,   ///< A rectangle's outline, `alpha` pixels wide.
                Text,    ///< `text` in `font` and `color`, its top left at (`x`, `y`).
                Image,   ///< `image` scaled to `width` × `height` at (`x`, `y`), inside `clip`; `alpha`: its filters.
                Pattern, ///< `image` at its own size (`width` × `height`), tiled over `clip`.
                Script,  ///< `script`'s region, `width` × `height` at (`x`, `y`); its beats from step `beat`.
        };

        struct DrawOp {
                        OpType       type = OpType::Fill;
                        uint8_t      alpha = 255;  ///< Fill: opacity; Frame: line width.
                        uint16_t     fragment = 0; ///< Drawn from this step on (0: always).
                        uint16_t      beat = 0;     ///< Script: the step of its first beat.
                        int32_t      x = 0;
                        int32_t      y = 0;
                        int32_t      width = 0;
                        int32_t      height = 0;
                        Color        color = 0;
                        FontKey      font;
                        StringView   text;
                        const Image  *image = nullptr;
                        ScriptRegion *script = nullptr;
                        Rect          clip;
                        DrawOp      *next = nullptr;
        };

        struct SlideLayout {
                        const Slide *slide = nullptr;
                        const Theme *theme = nullptr; ///< The slide's (its classes' variant).
                        Color        background = 0xFFFFFF;
                        DrawOp      *ops = nullptr;
                        uint16_t     steps = 0;         ///< Its steps after the first: fragments and beats.
                        int32_t      contentBottom = 0; ///< Where the content ended.
                        bool         overflows = false; ///< Past the content's box: cut at the slide's edge.
        };

        /**
         * @brief Lays out a deck's slides in a theme.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class Layouter {
                public:
                        /**
                         * @brief Lays out @p deck in @p themes (each slide in its classes' variant). With
                         *        @p images (nullptr: none), pictures have their own sizes and are drawn.
                         */
                        Layouter(Deck &deck, ThemeSet &themes, FontCache &fonts, Platform &platform,
                                 ImageCache *images);
                        ~Layouter();
                        Layouter(const Layouter &) = delete;
                        Layouter &operator=(const Layouter &) = delete;

                        /** @brief Script regions made by @p scripts (nullptr, the default: boxes). */
                        void setScripts(Scripts *scripts) { _scripts = scripts; }

                        /**
                         * @brief Lays out @p slide in @p arena (which must live as long as the deck);
                         *        with @p report, what doesn't fit goes into the deck's diagnostics.
                         * @return The layout, or nullptr if out of memory.
                         */
                        SlideLayout *layout(const Slide &slide, Arena &arena, bool report);

                        /** @brief The slide's size (the theme's, or the deck's `size`). */
                        int32_t width() const { return _width; }
                        int32_t height() const { return _height; }

                        // ---- The layout's parts (layout.cpp); public for its helpers only.
                        struct Piece;
                        struct Flow;

                private:
                        void    block(Flow &flow, const Block &b);
                        void    blocks(Flow &flow, const Block *first);
                        void    paragraph(Flow &flow, const Block &b, const TextStyle &style, bool fit);
                        void    list(Flow &flow, const Block &b);
                        void    quote(Flow &flow, const Block &b);
                        void    code(Flow &flow, const Block &b);
                        void    table(Flow &flow, const Block &b);
                        void    gather(const Inline *inl, const TextStyle &style, Color color);
                        int32_t lines(Flow &flow, int32_t x0, int32_t width, int32_t y, uint8_t align, bool emit);
                        int32_t pieceWidth(Piece &p);
                        void    imageBox(Flow &flow, const Image &image, int32_t x, int32_t y, int32_t w, int32_t h,
                                         bool background);
                        void    placeholder(Flow &flow, const Image &image, int32_t x, int32_t y, int32_t w, int32_t h);
                        void     scriptBox(Flow &flow, const Image &image, int32_t x, int32_t y, int32_t w, int32_t h);
                        uint16_t stepOf(Flow &flow);
                        bool     scriptOp(Flow &flow, ScriptPlace &place, const Image *image, const Rect &r);
                        bool    naturalSize(const Image &image, int32_t *w, int32_t *h);
                        void    backgrounds(Flow &flow, const Slide &slide);
                        void    heading(Flow &flow, const Block &b);
                        void    frame(Flow &flow, const Rect &r, Frame frame, Color color);
                        DrawOp *insertOp(Flow &flow, DrawOp **at, OpType type);
                        int32_t lineWidth() const { return _width >= 1000 ? 2 : 1; }
                        DrawOp *op(Flow &flow, OpType type);
                        bool    addPiece(const Piece &p);

                        Deck        &_deck;
                        ThemeSet    &_themes;
                        const Theme *_theme; ///< The slide being laid out's.
                        FontCache   &_fonts;
                        Platform    &_platform;
                        ImageCache  *_images;
                        Scripts     *_scripts = nullptr;
                        int32_t      _width;
                        int32_t      _height;
                        Piece       *_pieces = nullptr;
                        uint32_t     _pieceCount = 0;
                        uint32_t     _pieceCapacity = 0;
                        bool         _outOfMemory = false;
        };

        /**
         * @brief Draws @p layout as it is at step @p step (0: before any fragment) onto @p canvas, its
         *        pictures from @p images (nullptr: their boxes) in its theme's palette, its theme's
         *        effects over it. With @p maxCharacters, only that many of its text's characters (the
         *        `type` transition), and no effects (they come with the whole of it). With @p area,
         *        only that part of it (a script region's frame), effects included: an area that
         *        changed must be grown by effectReach() first (a glow reaches round what changed).
         *        With @p moving (a frame of something moving), the area's glow is left out: on the
         *        board it costs a tenth of a second for a region's part, and is drawn when it stops.
         */
        void drawSlide(const SlideLayout &layout, uint32_t step, Canvas &canvas, FontCache &fonts, ImageCache *images,
                       uint32_t maxCharacters = UINT32_MAX, const Rect *area = nullptr, bool moving = false);

        /** @brief The characters of @p layout's text at step @p step (what `type` types). */
        uint32_t countCharacters(const SlideLayout &layout, uint32_t step);

} // namespace preso

#endif // PRESO_LAYOUT_H
