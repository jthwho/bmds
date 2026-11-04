/**
 * @file      scripts.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What the core knows of a deck's scripts (devplan/preso.md §7, §7.9): regions of slides that
 * a script draws in, each with its beats (steps of its slide), its own clock, and its
 * drawing. The scripts themselves are another part's (programs/preso/script: AngelScript); a
 * build without them passes none, and regions are boxes.
 */

#ifndef PRESO_SCRIPTS_H
#define PRESO_SCRIPTS_H

#include "canvas.h"
#include "types.h"

namespace preso {

        class Deck;
        class FontCache;
        class ImageCache;
        class Platform;
        class ThemeSet;
        struct Image;
        struct Slide;
        struct Theme;

        /**
         * @brief A script's region of a slide: its beats, its clock, its picture.
         *
         * A region is at a beat (0: before its first pause()), either playing it (its clock
         * running) or at its end (still, waiting for Next).
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        /** @brief Input a region gets (an entered one's keys; the pointer over it). */
        struct ScriptInput {
                        enum class Type : uint8_t {
                                KeyDown,
                                KeyUp,
                                PointerMove,
                                PointerDown,
                                PointerUp,
                                Wheel,
                        };
                        Type     type = Type::KeyDown;
                        bool     repeat = false;
                        uint32_t key = 0;       ///< HID usage ((page << 16) | usage).
                        uint32_t character = 0; ///< What the key types (0: nothing).
                        int32_t  x = 0;         ///< The pointer, in the region's pixels.
                        int32_t  y = 0;
                        int32_t  button = 0; ///< PointerDown, PointerUp: 1 the first.
                        int32_t  wheel = 0;
        };

        class ScriptRegion {
                public:
                        virtual ~ScriptRegion() = default;

                        /** @brief Its beats: the steps it adds to its slide (its pause()s). */
                        virtual uint32_t beats() const = 0;
                        /** @brief The beat it is at. */
                        virtual uint32_t beat() const = 0;
                        /**
                         * @brief Beat @p beat from its start, to be played (@p played false: forward,
                         *        onto it), or as it ends, still (true: back onto it, or jumped to).
                         */
                        virtual void seek(uint32_t beat, bool played) = 0;
                        /** @brief Next: what is left of this beat at once, then the next one played. */
                        virtual void next() = 0;
                        /**
                         * @brief Whether frames change it: its clock running, an animation started from
                         *        a handler, or an entered region's tick().
                         */
                        virtual bool isMoving() const = 0;
                        /** @brief advance()'s parts at most. */
                        static constexpr uint32_t MaxAreas = 8;
                        /**
                         * @brief @p ns more of its clock. @return How many parts of the region changed,
                         *        into @p areas (MaxAreas at most; its own pixels): what moved covered
                         *        before, and covers now.
                         */
                        virtual uint32_t advance(int64_t ns, Rect *areas) = 0;
                        /**
                         * @brief Draws it as it now is with its top left at (@p x, @p y), inside the
                         *        canvas's clip, over what is there (the slide under it).
                         */
                        virtual void draw(Canvas &canvas, int32_t x, int32_t y) = 0;
                        /** @brief Whether its script failed: it shows the error instead. */
                        virtual bool hasFailed() const = 0;

                        // ---- Interactive pieces (§7.10)

                        /** @brief Whether it takes input: an `event()` or `tick()`, or widgets. */
                        virtual bool isInteractive() const = 0;
                        /** @brief Entered (its keys the script's, tick() each frame, its frame drawn) or not. */
                        virtual void setEntered(bool entered) = 0;
                        /** @brief Input for it, in its own pixels. @return The parts that changed, as advance(). */
                        virtual uint32_t input(const ScriptInput &input, Rect *areas) = 0;
                        /**
                         * @brief Its slide shown (its sounds heard) or not. Sounds asked for while it
                         *        isn't (its slide drawn ahead) are heard when it is.
                         */
                        virtual void setAudible(bool audible) = 0;
        };

        /** @brief Where a region is: its slide, its image (`![script ...](file)`), its size. */
        struct ScriptPlace {
                        const Slide *slide = nullptr;
                        const Image *image = nullptr;
                        const Theme *theme = nullptr; ///< The slide's.
                        int32_t      width = 0;
                        int32_t      height = 0;
                        uint32_t     index = 0;       ///< Its number on the slide (0 up): its random numbers'.
                        StringView   source;          ///< A block's script, written in the slide (image: nullptr).
                        uint32_t     firstLine = 0;   ///< A block's: the deck's line of its script's line 1.
                        bool         gated = false;   ///< Its first beat runs it (a block's: the step after its code).
                        bool         console = false; ///< Printed lines fill it, in the code font (a block's pane).
        };

        /**
         * @brief A deck's scripts: their regions, made as slides are laid out and kept as long as
         *        this. Each region runs its script to its end once as it is made (to count its
         *        beats), so what is wrong in a script is found then: reported into the deck.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class Scripts {
                public:
                        virtual ~Scripts() = default;

                        /** @brief A region for @p place; nullptr if out of memory. */
                        virtual ScriptRegion *open(const ScriptPlace &place) = 0;
        };

        /** @brief What a deck's scripts are made with. */
        struct ScriptSetup {
                        bool        trusted = false; ///< `--trust`: angel's system API too (§7.7).
                        Platform   *platform = nullptr;
                        Deck       *deck = nullptr;
                        ThemeSet   *themes = nullptr;
                        FontCache  *fonts = nullptr;
                        ImageCache *images = nullptr;
                        const char *deckPath = nullptr;
        };

        /**
         * @brief Makes a deck's scripts (nullptr: out of memory) in malloc's memory: the core ends
         *        them with their destructor and free().
         */
        using ScriptsMaker = Scripts *(*)(const ScriptSetup &setup);

} // namespace preso

#endif // PRESO_SCRIPTS_H
