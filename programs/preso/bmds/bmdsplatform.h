/**
 * @file      bmdsplatform.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Preso on BMDS (devplan/preso.md §2): the system's fonts and code colouring, a surface the
 * slides are copied onto by the 2D-DMA, the presenter's actions through Controls, and the
 * deck's folder watched. Its own loop, not libbmdsgame's: that one wakes for its ticks, and a
 * slide that doesn't change should cost nothing; this one sleeps until an event or a change.
 */

#ifndef PRESO_BMDSPLATFORM_H
#define PRESO_BMDSPLATFORM_H

#include "../core/platform.h"

namespace preso {

        /**
         * @brief The platform on BMDS, through the SDK.
         *
         * @par Thread Safety
         * As Platform.
         */
        class BmdsPlatform : public Platform {
                public:
                        BmdsPlatform() = default;
                        ~BmdsPlatform() override;
                        BmdsPlatform(const BmdsPlatform &) = delete;
                        BmdsPlatform &operator=(const BmdsPlatform &) = delete;

                        int32_t     openFont(const char *name) override;
                        void        closeFont(int32_t font) override;
                        FontMetrics fontMetrics(int32_t font) override;
                        int32_t     textWidth(int32_t font, StringView text) override;
                        uint32_t    textFit(int32_t font, StringView text, int32_t maxWidth) override;
                        void        drawText(Canvas &canvas, int32_t font, int32_t x, int32_t y, StringView text,
                                             Color color) override;

                        int32_t  findSyntax(StringView name) override;
                        uint32_t colorLine(int32_t syntax, StringView line, uint32_t *state, SyntaxSpan *spans,
                                           uint32_t max) override;

                        Error imageSize(const char *path, int32_t *width, int32_t *height) override;
                        Error decodeImage(const char *path, int32_t minWidth, int32_t minHeight,
                                          Picture *picture) override;

                        void       *allocatePixels(uint32_t bytes) override;
                        void        freePixels(void *pixels) override;
                        PixelFormat pixelFormat() const override { return PixelFormat::Rgb565; }

                        Error       openScreen(int32_t width, int32_t height, const char *title, bool sharp) override;
                        void        closeScreen() override;
                        const char *dataFolder() override;
                        void        present(const Canvas &canvas) override;
                        void        presentAreas(const Canvas &canvas, const Rect *areas, uint32_t count) override;
                        bool        beginPieces(const Canvas &from, const Canvas &to) override;
                        bool        presentPieces(const Piece *pieces, uint32_t count) override;
                        void        endPieces() override;
                        uint32_t    frameBuffers() const override { return _bufferCount; }
                        void        waitFrame() override;
                        bool        beginFrame(Canvas &canvas) override;
                        void        endFrame() override;
                        void        animate(bool on) override { _animating = on; }
                        void        setInputMode(InputMode mode) override;
                        void        setPointerVisible(bool visible) override;
                        bool        readTerminal() override;
                        bool        outputIsTerminal() override;
                        void        setTicking(bool on) override { _ticking = on; }
                        Error       startDemo(const char *command, const char *folder) override;
                        int32_t     openSound(const char *path) override;
                        void        playSound(int32_t sound, float volume) override;
                        void        closeSound(int32_t sound) override;
                        int64_t     now() override;
                        Error       watch(const char *path) override;
                        Error       run(App &app) override;
                        void        quit() override { _quit = true; }

                private:
                        static constexpr uint32_t MaxSounds = 32;

                        void dispatch(App &app);
                        void freeQuietSounds();
                        void restoreTerminal();
                        void demoFinished(App &app);
                        // A buffer's content is no longer the slide's (a frame drawn straight in).
                        void staleAll();

                        int32_t   _surface = -1;
                        static constexpr uint32_t MaxWatches = 8;
                        int32_t   _watches[MaxWatches] = {-1, -1, -1, -1, -1, -1, -1, -1}; ///< The deck's folders'.
                        uint32_t  _watchCount = 0;
                        int32_t   _width = 0;
                        int32_t   _height = 0;
                        void     *_buffers[3] = {};
                        uint32_t  _strides[3] = {};
                        uint32_t  _bufferCount = 0;
                        Rect      _stale[3]; ///< What each buffer lacks of the slide presented since it was.
                        bool      _controls = false;
                        bool      _declared = false; ///< The controls asked for (once: they are the process's).
                        bool      _animating = false;
                        int32_t   _frame = -1; ///< The buffer beginFrame() acquired.
                        InputMode _mode = InputMode::Deck;
                        bool      _dmaPieces = false; ///< beginPieces() gave the pictures to the 2D-DMA.
                        uint32_t  _shownFrame = 0;    ///< The display's frame the last present was for.
                        char      _data[256] = {};
                        int32_t   _pointerX = 0; ///< Where the pointer was last (a wheel's place).
                        // The terminal it was started from: raw while its keys are read.
                        bool      _terminal = false;
                        void     *_savedTerminal = nullptr; ///< A struct termios.
                        bool      _ticking = false;
                        int64_t   _nextSecond = 0;
                        int32_t   _stickX = 0; ///< A pad's left stick, as last told.
                        int32_t   _stickY = 0;
                        int32_t   _pointerY = 0;
                        // A demo (§6a): its process, the pipe SIGCHLD's handler writes.
                        int32_t _demoPid = -1;
                        int32_t _childPipe[2] = {-1, -1};
                        // Sounds: libbmdsgame's mixer, opened with the first; each slot's last voice (a
                        // sound closed while it plays is freed once it has stopped).
                        void *_audio = nullptr; ///< A bmds_game_audio_t.
                        bool  _audioFailed = false;
                        struct Sound {
                                        void   *loaded = nullptr; ///< A bmds_game_sound_t (game.h stays in the .cpp).
                                        int32_t voice = -1;
                                        bool    closed = false;
                        };
                        Sound     _sounds[MaxSounds];
                        bool      _quit = false;
        };

} // namespace preso

#endif // PRESO_BMDSPLATFORM_H
