/**
 * @file      presenter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Presenting a deck (devplan/preso.md §6): the slide and step shown, the actions that move
 * them, the next step drawn ahead of time so a step is a copy, the deck reloaded when its
 * folder changes (staying on its slide); transitions, black and white screens, going to a slide
 * by its number, and the overview of every slide.
 */

#ifndef PRESO_PRESENTER_H
#define PRESO_PRESENTER_H

#include <stdio.h>
#include "deck.h"
#include "fonts.h"
#include "layout.h"
#include "platform.h"
#include "theme.h"
#include "transitions.h"

namespace preso {

        /**
         * @brief A deck and what of it is shown.
         *
         * @par Thread Safety
         * Not thread-safe: the platform's loop calls it.
         */
        class Presenter : public App {
                public:
                        /**
                         * @brief Presents the deck at @p path (kept: it must live as long as this),
                         *        its problems written to @p messages (nullptr: nowhere).
                         */
                        Presenter(Platform &platform, const char *path, FILE *messages);
                        ~Presenter() override;
                        Presenter(const Presenter &) = delete;
                        Presenter &operator=(const Presenter &) = delete;

                        /** @brief Reads the deck (its problems to `messages`); fails if it can't be read. */
                        [[nodiscard]] Error load();
                        /** @brief Opens the screen at slide @p number and runs until quit. */
                        [[nodiscard]] Error run(uint32_t number);

                        // ---- App
                        void event(const Event &event) override;
                        void filesChanged() override;
                        void frame(int64_t ns) override;

                        // ---- What is shown (for the tests)
                        uint32_t slideNumber() const { return _slide; }
                        uint32_t step() const { return _step; }
                        bool     inTransition() const { return _mode == Mode::Transition; }
                        bool     inOverview() const { return _mode == Mode::Overview; }
                        uint32_t selected() const { return _selected; }
                        /** @brief The colour the screen is blanked to (Action::Black, White), or NoColor. */
                        Color         blank() const { return _blank; }
                        StringView    typedNumber() const { return StringView(_typed, _typedLength); }
                        const Deck   *deck() const { return _deck; }
                        const Canvas &shown() const { return _shown; }
                        const Canvas &ahead() const { return _ahead; } ///< During a transition: the slide it goes to.
                        /** @brief Moves as @p action does, and shows the result. */
                        void act(Action action);
                        /** @brief Reads the deck again, keeping the slide; false (the old kept) if it can't. */
                        bool reload();

                private:
                        struct Loaded;

                        enum class Mode : uint8_t {
                                Slides,
                                Transition,
                                Overview
                        };

                        Error read(Loaded *&loaded);
                        void  drop(Loaded *loaded);
                        void  target(Action action, uint32_t &slide, uint32_t &step) const;
                        void  draw(Canvas &canvas, uint32_t slide, uint32_t step);
                        void  show(bool forward);
                        void  finishTransition();
                        void  showOverlay();
                        void  key(const Event &event);
                        void  goTo(uint32_t slide);
                        // The overview
                        void     openOverview();
                        void     closeOverview(bool choose);
                        void     moveSelection(int32_t by);
                        void     drawOverview(Canvas &screen);
                        bool     makeThumbnail();
                        void     dropThumbnails();
                        uint32_t perPage() const { return _columns * _rows; }
                        void     prepareAhead();
                        void     printNewDiagnostics();
                        uint32_t fragments(uint32_t slide) const;

                        Platform   &_platform;
                        const char *_path;
                        FILE       *_messages;
                        FontCache   _fonts;
                        ImageCache  _images;
                        Loaded     *_loaded = nullptr;
                        Deck       *_deck = nullptr;
                        uint32_t    _slide = 1;
                        uint32_t    _step = 0;
                        // The shown picture, and the next step's drawn ahead.
                        Canvas   _shown;
                        Canvas   _ahead;
                        void    *_pixels[2] = {nullptr, nullptr};
                        uint32_t _aheadSlide = 0;
                        uint32_t _aheadStep = 0;
                        bool     _aheadValid = false;
                        bool     _screen = false;
                        uint32_t _shownSlide = 0; ///< What _shown holds.
                        uint32_t _shownStep = 0;
                        Mode     _mode = Mode::Slides;
                        // A transition playing: from _shown to _ahead.
                        Transition _transition;
                        int64_t    _transitionStart = 0;
                        uint32_t   _typeTotal = 0;    ///< `type`'s characters.
                        Piece     *_pieces = nullptr; ///< A frame's (MaxPieces), made once.
                        uint32_t   typePieces(uint32_t budget, uint32_t since);
                        // What each screen buffer shows of the transition (its progress, or `type`'s
                        // characters; UINT32_MAX: not known), so a frame changes only what differs.
                        uint32_t _buffered[4] = {};
                        uint32_t _lastStage = UINT32_MAX; ///< Pixelate's stage last shown.
                        uint32_t _frameIndex = 0;
                        Color    _blank = NoColor;
                        char     _typed[8] = {}; ///< A slide's number, typed before Enter.
                        uint32_t _typedLength = 0;
                        // The overview: a grid of thumbnails, made one a frame and kept.
                        uint32_t     _selected = 1;
                        uint32_t     _columns = 4;
                        uint32_t     _rows = 3;
                        int32_t      _thumbWidth = 0;
                        int32_t      _thumbHeight = 0;
                        ScaledImage *_thumbs = nullptr; ///< One a slide (pixels nullptr: not made).
                        uint32_t     _thumbCount = 0;
                        char         _status[160] = {}; ///< Why the deck couldn't be read again: shown in a corner.
        };

        /**
         * @brief `preso check`'s work: reads @p path, lays out every slide, and writes what it
         *        found to @p out (`file:line: warning: ...`); with @p verbose, the slides too.
         * @return 0: nothing found; 1: something reported; 2: the deck couldn't be read.
         */
        int checkDeck(Platform &platform, const char *path, bool verbose, FILE *out);

} // namespace preso

#endif // PRESO_PRESENTER_H
