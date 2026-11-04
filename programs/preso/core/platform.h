/**
 * @file      platform.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The core's only way out (devplan/preso.md §2): one class per platform (BMDS, headless, SDL3
 * later) gives it files, fonts, colouring code, the screen and its events, and watching a
 * folder. A call is added here when a step needs it.
 */

#ifndef PRESO_PLATFORM_H
#define PRESO_PLATFORM_H

#include "canvas.h"
#include "types.h"

namespace preso {

        struct FontMetrics {
                        int32_t ascent = 0;     ///< Pixels above the baseline.
                        int32_t descent = 0;    ///< Pixels below it.
                        int32_t lineHeight = 0; ///< From one line's top to the next's.
        };

        /** @brief A run of a line of code in one style (`SyntaxStyle`, the SDK's `BMDS_SYNTAX_...`). */
        struct SyntaxSpan {
                        uint32_t start = 0;
                        uint32_t end = 0;
                        uint8_t  style = 0;
        };

        /**
         * @brief A decoded picture, `stride` bytes a row, its pixels from Platform::allocatePixels()
         *        (freed with freePixels()): 0xAARRGGBB words (straight alpha), or for an opaque one
         *        bytes B, G, R or RGB565 words (what a JPEG decoder makes without a copy; RGB565 is
         *        two thirds of the memory, and the board's slides are RGB565 anyway).
         */
        struct Picture {
                        enum class Layout : uint8_t {
                                Bgra32,
                                Bgr24,
                                Rgb565
                        };

                        uint32_t *pixels = nullptr;
                        Layout    layout = Layout::Bgra32;
                        int32_t   width = 0;
                        int32_t   height = 0;
                        uint32_t  stride = 0;
                        bool      alpha = false; ///< Some pixels may be transparent.
        };

        /** @brief The styles code is coloured in (the SDK's numbers). */
        enum class SyntaxStyle : uint8_t {
                Normal,
                Keyword,
                Type,
                Constant,
                Number,
                String,
                Character,
                Comment,
                Preprocessor,
                Operator,
                Function,
                Heading,
                Emphasis,
                Added,
                Removed,
                Error,
                Count
        };

        /** @brief What a presenter does (keys, a clicker, a pad: the platform's bindings). */
        enum class InputMode : uint8_t {
                Deck,     ///< Next, Previous...
                Overview, ///< Left, Right, Up, Down, Choose, Close.
                Script,   ///< Leave; every other key a key (an entered script region's).
        };

        enum class Action : uint8_t {
                None,
                Next,     ///< The next step: a fragment, or the next slide.
                Previous, ///< The step before.
                First,
                Last,
                Quit,
                Black,    ///< The screen black (again: back), B or `.`, a clicker's blank.
                White,    ///< The screen white.
                Overview, ///< A grid of every slide (Tab).
                // The overview's (InputMode::Overview):
                Left,
                Right,
                Up,
                Down,
                Choose, ///< Go to the slide chosen.
                Close,  ///< Back to the slide shown before.
                // Script regions (§7.10):
                Into,  ///< Enter the slide's interactive region: its keys are the script's (I).
                Leave, ///< Give the keys back (InputMode::Script's, Escape).
                ClearInk, ///< The slide's ink wiped (C, a pad's North button).
        };

        /**
         * @brief What a key means with no Controls (the terminal's keys, a system without them):
         *        @p usage a HID usage (page 7's), in @p mode. Action::None: a plain key.
         */
        Action defaultAction(uint32_t usage, InputMode mode);

        /**
         * @brief A rectangle of a frame made of pictures (Platform::presentPieces()): copied from
         *        one of two pictures, or filled.
         */
        struct Piece {
                        enum Source : uint8_t {
                                From,
                                To,
                                Fill
                        };

                        int32_t x = 0; ///< Where on the screen.
                        int32_t y = 0;
                        int32_t width = 0;
                        int32_t height = 0;
                        int32_t fromX = 0; ///< Where in the picture (Fill: unused).
                        int32_t fromY = 0;
                        Source  source = From;
                        Color   color = 0; ///< Fill's.
        };

        /** @brief What the keys are for (on BMDS a Controls set each). */


        struct Event {
                        enum class Type : uint8_t {
                                Action, ///< `action`.
                                Key,    ///< A key no action took: `key` (HID usage), `character` (0: none).
                                Close,  ///< The window was closed.
                                KeyUp,  ///< A key let go (InputMode::Script's): `key`.
                                Pointer, ///< The pointer moved or a button changed: `x`, `y`, `buttons`.
                                Wheel,   ///< `wheel` notches (away from the user positive), at `x`, `y`.
                                Stick,   ///< A pad's left stick: `x`, `y` from -32767 to 32767 (0: centred).
                        };

                        Type     type = Type::Action;
                        Action   action = Action::None;
                        bool     repeat = false; ///< A held key's repeat.
                        uint32_t key = 0;
                        uint32_t character = 0;
                        int32_t  x = 0; ///< Pointer, Wheel: in the slide's pixels.
                        int32_t  y = 0;
                        uint32_t buttons = 0; ///< Pointer: down, a bit each (1 the first).
                        int32_t  wheel = 0;
        };

        /** @brief What the platform's loop calls (Platform::run()). */
        class App {
                public:
                        virtual ~App() = default;
                        virtual void event(const Event &event) = 0;
                        /** @brief Something in the watched folder changed (Platform::watch()). */
                        virtual void filesChanged() = 0;
                        /**
                         * @brief A display frame while animating (Platform::animate()): draws it (between
                         *        beginFrame() and endFrame()). @p ns: Platform::now().
                         */
                        virtual void frame(int64_t ns) { (void)ns; }
                        /** @brief Bytes typed on the terminal (Platform::readTerminal()). */
                        virtual void terminalInput(const char *data, uint32_t length) {
                                (void)data;
                                (void)length;
                        }
                        /** @brief About once a second while Platform::setTicking() is on. */
                        virtual void second() {}
                        /** @brief The demo Platform::startDemo() started has ended, with @p status. */
                        virtual void demoEnded(int32_t status) { (void)status; }
        };

        /**
         * @brief What the core asks of the system it runs on.
         *
         * @par Thread Safety
         * Called from the core's thread only.
         */
        class Platform {
                public:
                        virtual ~Platform() = default;

                        // ---- Files

                        /**
                         * @brief Reads all of @p path (NUL-terminated) into a malloc'd buffer (the
                         *        caller free()s it; one byte past the end is a NUL), at most
                         *        @p maxLength bytes (Error::TooBig past that). By default through C's
                         *        stdio, which every platform has.
                         */
                        [[nodiscard]] virtual Error readFile(const char *path, uint32_t maxLength, char **data,
                                                             uint32_t *length);
                        /** @brief Whether there is a file at @p path (by default through stdio). */
                        virtual bool fileExists(const char *path);

                        // ---- Fonts: a font is a name ("IBM Plex Sans 32 bold"), open as a handle.

                        /** @brief A handle (0 up) for the font @p name names, or a negative error. */
                        virtual int32_t     openFont(const char *name) = 0;
                        virtual void        closeFont(int32_t font) = 0;
                        virtual FontMetrics fontMetrics(int32_t font) = 0;
                        /** @brief @p text's advance in pixels (one line). */
                        virtual int32_t textWidth(int32_t font, StringView text) = 0;
                        /** @brief The bytes of @p text (whole characters) that fit in @p maxWidth pixels. */
                        virtual uint32_t textFit(int32_t font, StringView text, int32_t maxWidth) = 0;
                        /** @brief Draws one line of @p text with its top left at (@p x, @p y), clipped. */
                        virtual void drawText(Canvas &canvas, int32_t font, int32_t x, int32_t y, StringView text,
                                              Color color) = 0;

                        // ---- Colouring code (none: plain text)

                        /** @brief The language @p name names (an id, an extension, `angel`), or -1. */
                        virtual int32_t findSyntax(StringView name) {
                                (void)name;
                                return -1;
                        }
                        /**
                         * @brief Colours @p line of language @p syntax, which starts in *@p state (and
                         *        is left in the line's end state): up to @p max spans, their number.
                         */
                        virtual uint32_t colorLine(int32_t syntax, StringView line, uint32_t *state, SyntaxSpan *spans,
                                                   uint32_t max) {
                                (void)syntax;
                                (void)line;
                                (void)state;
                                (void)spans;
                                (void)max;
                                return 0;
                        }

                        // ---- Pictures (PNG, JPEG, BMP: what the platform decodes)

                        /**
                         * @brief The size of the picture at @p path, upright. By default the file is
                         *        only looked for: Error::NotSupported if it is there (no decoder),
                         *        Error::NotFound if not.
                         */
                        [[nodiscard]] virtual Error imageSize(const char *path, int32_t *width, int32_t *height);
                        /**
                         * @brief Decodes the picture at @p path, upright, at least @p minWidth ×
                         *        @p minHeight where it can come smaller (a JPEG reduced), else
                         *        whole. By default Error::NotSupported.
                         */
                        [[nodiscard]] virtual Error decodeImage(const char *path, int32_t minWidth, int32_t minHeight,
                                                                Picture *picture) {
                                (void)path;
                                (void)minWidth;
                                (void)minHeight;
                                (void)picture;
                                return Error::NotSupported;
                        }

                        // ---- Where Preso's own files are (`themes/`): nullptr, none

                        virtual const char *dataFolder() { return nullptr; }

                        // ---- Pixels for slides (where the platform's copies want them)

                        virtual void *allocatePixels(uint32_t bytes);
                        virtual void  freePixels(void *pixels);
                        /** @brief The format slides are drawn in (the screen's). */
                        virtual PixelFormat pixelFormat() const { return PixelFormat::Rgb565; }

                        // ---- The screen (none: Error::NotSupported)

                        /**
                         * @brief Opens the screen, or a window, for slides @p width × @p height; with
                         *        @p sharp scaled by whole multiples only (a theme's `Scale=nearest`).
                         */
                        [[nodiscard]] virtual Error openScreen(int32_t width, int32_t height, const char *title,
                                                               bool sharp) {
                                (void)width;
                                (void)height;
                                (void)title;
                                (void)sharp;
                                return Error::NotSupported;
                        }
                        /**
                         * @brief Closes the screen openScreen() opened, its memory given back (while a
                         *        demo has the screen, §6a); openScreen() opens it again.
                         */
                        virtual void closeScreen() {}
                        /** @brief Shows @p canvas (the screen's size and format). */
                        virtual void present(const Canvas &canvas) { (void)canvas; }
                        /**
                         * @brief Shows @p canvas where only @p count @p areas changed since the last
                         *        present() or presentAreas() (a script region's frame). By default
                         *        all of it.
                         */
                        virtual void presentAreas(const Canvas &canvas, const Rect *areas, uint32_t count) {
                                (void)areas;
                                (void)count;
                                present(canvas);
                        }
                        /**
                         * @brief A screen buffer to draw a frame straight into (a transition, an
                         *        overlay), as @p canvas: false if the platform has none (then the
                         *        frame isn't drawn). Waits for one to be free: on BMDS that paces
                         *        frames to the display. endFrame() shows it.
                         */
                        virtual bool beginFrame(Canvas &canvas) {
                                (void)canvas;
                                return false;
                        }
                        virtual void endFrame() {}
                        /**
                         * @brief Frames made of pieces of @p from and @p to (each the screen's size and
                         *        format, unchanged until endPieces()) follow: on BMDS both are given
                         *        to the 2D-DMA, which then copies each frame (the CPU copies PSRAM at
                         *        a third of its speed). By default the CPU copies them.
                         */
                        virtual bool beginPieces(const Canvas &from, const Canvas &to);
                        /** @brief A frame of @p count pieces (in order: later ones over earlier ones). */
                        virtual bool presentPieces(const Piece *pieces, uint32_t count);
                        /**
                         * @brief How many screen buffers frames take turns in (each frame's buffer
                         *        holds the frame this many before), so a frame need only change what
                         *        differs from that; 0: not known (every frame whole).
                         */
                        virtual uint32_t frameBuffers() const { return 0; }
                        virtual void     endPieces();

                        /**
                         * @brief Waits for the display's next frame: an animation's pace when a frame
                         *        has nothing new to show (rather than spinning).
                         */
                        virtual void waitFrame() {}

                        /** @brief Calls App::frame() each display frame while @p on. */
                        virtual void animate(bool on) { (void)on; }
                        /** @brief What the keys mean from now on. */
                        virtual void setInputMode(InputMode mode) { (void)mode; }
                        // ---- The terminal it was started from (none: false)

                        /**
                         * @brief Reads the keys typed on standard input, if it is a terminal (raw, until
                         *        the platform ends): App::terminalInput(). False if it isn't one.
                         */
                        virtual bool readTerminal() { return false; }
                        /** @brief Whether standard output is a terminal (the notes redraw it in place). */
                        virtual bool outputIsTerminal() { return false; }
                        /** @brief Calls App::second() about once a second while @p on. */
                        virtual void setTicking(bool on) { (void)on; }

                        // ---- Live demos (§6a; none: Error::NotSupported)

                        /**
                         * @brief Starts @p command through the shell, in folder @p folder: App::demoEnded()
                         *        when it ends. The presenter closes the screen meanwhile (closeScreen()),
                         *        its memory the demo's.
                         */
                        [[nodiscard]] virtual Error startDemo(const char *command, const char *folder) {
                                (void)command;
                                (void)folder;
                                return Error::NotSupported;
                        }

                        /** @brief The system's pointer over the slides, or none (the default). */
                        virtual void setPointerVisible(bool visible) { (void)visible; }

                        // ---- Sounds (none: -1)

                        /** @brief A sound file (WAV, MP3, Ogg, FLAC: the platform's) loaded to play, or a negative error. */
                        virtual int32_t openSound(const char *path) {
                                (void)path;
                                return -1;
                        }
                        /** @brief Plays sound @p sound at @p volume (0 to 1), over what plays. */
                        virtual void playSound(int32_t sound, float volume) {
                                (void)sound;
                                (void)volume;
                        }
                        virtual void closeSound(int32_t sound) { (void)sound; }
                        /** @brief Monotonic nanoseconds (by default C11's timespec_get). */
                        virtual int64_t now();

                        /** @brief Tells the app of changes in folder @p path from now on (App::filesChanged()). */
                        [[nodiscard]] virtual Error watch(const char *path) {
                                (void)path;
                                return Error::NotSupported;
                        }
                        /** @brief Runs the loop: events to @p app until quit(). */
                        [[nodiscard]] virtual Error run(App &app) {
                                (void)app;
                                return Error::NotSupported;
                        }
                        /** @brief Ends run() once the event being handled is done. */
                        virtual void quit() {}

                protected:
                        const Canvas *_piecesFrom = nullptr; ///< beginPieces()'s, for the default copies.
                        const Canvas *_piecesTo = nullptr;
        };

} // namespace preso

#endif // PRESO_PLATFORM_H
