/**
 * @file      runtime.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A deck's scripts in AngelScript (devplan/preso.md §7, §7.9): one engine for the deck, made
 * when its first region is; a module, a context and a scene for each region. The parts share
 * this; the core sees only core/scripts.h.
 */

#ifndef PRESO_SCRIPT_RUNTIME_H
#define PRESO_SCRIPT_RUNTIME_H

#include <angelscript.h>
#include "angel.h"
#include "../core/deck.h"
#include "../core/scripts.h"
#include "animation.h"
#include "scene.h"

namespace preso {

        class AngelScripts;

        /**
         * @brief A script region: its module, its context (suspended in play(), wait() and
         *        pause()), its scene and its clock.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class AngelRegion : public ScriptRegion {
                public:
                        /** @brief Longest a script runs without play, wait or pause before it is stopped. */
                        static constexpr int64_t RunawayNs = 100000000;
                        /** @brief Fast-forward gives up past this much of a script's time (it runs on). */
                        static constexpr double   EndlessSeconds = 600;
                        static constexpr uint32_t EndlessYields = 20000;
                        static constexpr uint32_t MaxPlaying = 32;
                        /** @brief A trusted script's limit (a system call may block). */
                        static constexpr int64_t TrustedRunawayNs = 1000000000;
                        /** @brief Animations started from handlers at once, at most. */
                        static constexpr uint32_t MaxStarted = 32;
                        /** @brief Sounds asked for before its slide is shown, kept for then. */
                        static constexpr uint32_t MaxQueuedSounds = 8;

                        AngelRegion(AngelScripts &owner, const ScriptPlace &place, uint32_t seed);
                        ~AngelRegion() override;

                        /** @brief Reads and builds the script, then runs it to its end (its beats). */
                        void load();

                        // ---- ScriptRegion
                        uint32_t beats() const override { return _beats; }
                        uint32_t beat() const override { return _beat; }
                        void     seek(uint32_t beat, bool played) override;
                        void     next() override;
                        bool     isMoving() const override;
                        uint32_t advance(int64_t ns, Rect *areas) override;
                        void     draw(Canvas &canvas, int32_t x, int32_t y) override;
                        bool     hasFailed() const override { return _state == State::Failed; }
                        bool     isInteractive() const override;
                        void     setEntered(bool entered) override;
                        uint32_t input(const ScriptInput &input, Rect *areas) override;
                        void     setAudible(bool audible) override;

                        // ---- For the script API (api.cpp): the region running now's.
                        void play(Animation *const *animations, uint32_t count, float seconds);
                        void wait(float seconds);
                        void pause();
                        void add(Shape *shape);
                        void remove(Shape *shape);
                        void write(const char *data, size_t length);
                        /** @brief An animation started from a handler: runs alongside, @p seconds long. */
                        void start(Animation *animation, float seconds);
                        /** @brief A sound to hear (platform's @p sound), now or when its slide is shown. */
                        void playSound(int32_t sound, float volume);
                        /** @brief A widget's change: its onChange() called (after the handler that made it). */
                        void                changed(Shape *widget);
                        bool                inHandler() const { return _inDraw; }
                        double              clock() const { return _clock; }
                        angel::RandomState &random() { return _random; }
                        SceneContext       &context() { return _context; }
                        const ScriptPlace  &place() const { return _place; }
                        AngelScripts       &owner() { return _owner; }
                        /** @brief Called each line (a line callback): stops a runaway script. */
                        void line(asIScriptContext *ctx);
                        /** @brief The canvas draw() is drawing on now, nullptr outside draw(). */
                        Canvas *drawCanvas() const { return _drawCanvas; }
                        int32_t drawX() const { return _drawX; }
                        int32_t drawY() const { return _drawY; }

                private:
                        enum class State : uint8_t {
                                Ready,    ///< Prepared or running, not suspended.
                                Playing,  ///< In play() or wait(): its clock runs.
                                AtPause,  ///< In pause(): waits for Next.
                                Finished, ///< Its script ran to its end.
                                Failed,   ///< It couldn't be built, threw, or ran away.
                        };

                        void    restart();
                        void    run();
                        void    endPlay();
                        void    step();
                        void    fail(const char *message, int line = 0);
                        void    where(int line, char *out, size_t size) const;
                        int64_t runaway() const;
                        // A handler (event(), tick(), a widget's callback) run on the handlers' context.
                        bool   handler(asIScriptFunction *function, void *object, float number, bool withObject);
                        void   stepStarted(int64_t ns);
                        Shape *widgetAt(int32_t x, int32_t y);
                        void   focusNext(int32_t by);
                        Shape *focusedWidget();
                        bool   widgetKey(const ScriptInput &input);
                        void   dragTo(Shape *slider, int32_t x);
                        void   failFrom(asIScriptContext *ctx, const char *what);
                        bool   yielded();
                        // What the region's objects looked like and covered when last drawn: what a
                        // frame redraws is what changed of it.
                        struct Drawn {
                                        const Shape *shape;
                                        uint32_t     signature;
                                        Rect         bounds;
                        };
                        void     remember();
                        uint32_t changes(Rect *areas);
                        bool     drawnWhole() const;
                        void     drawOutput(Canvas &canvas, int32_t x, int32_t y);
                        void     drawConsole(Canvas &canvas, int32_t x, int32_t y);
                        void     drawError(Canvas &canvas, int32_t x, int32_t y);

                        AngelScripts      &_owner;
                        ScriptPlace        _place;
                        SceneContext       _context;
                        uint32_t           _seed;
                        asIScriptModule   *_module = nullptr;
                        asIScriptFunction *_entry = nullptr;
                        asIScriptFunction *_drawFunction = nullptr;
                        asIScriptFunction *_eventFunction = nullptr;
                        asIScriptFunction *_tickFunction = nullptr;
                        asIScriptContext  *_ctx = nullptr;
                        asIScriptContext  *_drawCtx = nullptr;
                        Scene              _scene;
                        angel::RandomState _random = {};
                        State              _state = State::Ready;
                        uint32_t           _beats = 0;
                        uint32_t           _beat = 0;
                        // Fast-forward: to beat _target, then stopping at its end (_toEnd) or playing on.
                        bool     _fastForward = false;
                        uint32_t _target = 0;
                        bool     _toEnd = false;
                        double   _forwardStart = 0; ///< The clock when fast-forward began.
                        uint32_t _forwardYields = 0;
                        // Where seek() left it, so seeking there again costs nothing.
                        bool     _sought = false;
                        uint32_t _soughtBeat = 0;
                        bool     _soughtPlayed = false;
                        bool     _movedSince = false;
                        // The play going on.
                        double     _clock = 0;
                        double     _playStart = 0;
                        double     _playEnd = 0;
                        float      _playLength = 0;
                        Animation *_playing[MaxPlaying] = {};
                        uint32_t   _playingCount = 0;
                        // The runaway stop.
                        int64_t  _runStart = 0;
                        uint32_t _lines = 0;
                        bool     _ranAway = false;
                        // draw()'s
                        Canvas  *_drawCanvas = nullptr;
                        int32_t  _drawX = 0;
                        int32_t  _drawY = 0;
                        bool     _inDraw = false;
                        Drawn   *_drawn = nullptr;
                        uint32_t _drawnCount = 0;
                        uint32_t _drawnCapacity = 0;
                        uint32_t _outputDrawn = 0;        ///< print()'s bytes when last drawn.
                        void    *_canvasObject = nullptr; ///< The Canvas draw() is given (api.cpp's).
                        char    *_output = nullptr;       ///< print()'s, its last lines.
                        uint32_t _outputLength = 0;
                        char     _error[256] = {};
                        bool     _reported = false;
                        // Interactive pieces
                        bool   _entered = false;
                        bool   _audible = false;
                        Shape *_dragging = nullptr; ///< A slider held by the pointer.
                        Shape *_pressing = nullptr; ///< A button held by the pointer.
                        struct Started {
                                        Animation *animation;
                                        float      elapsed;
                                        float      length;
                        };
                        Started  _started[MaxStarted] = {};
                        uint32_t _startedCount = 0;
                        Shape   *_changes[8] = {}; ///< Widgets changed during a handler: their callbacks after.
                        uint32_t _changeCount = 0;
                        struct QueuedSound {
                                        int32_t sound;
                                        float   volume;
                        };
                        QueuedSound _queued[MaxQueuedSounds] = {};
                        uint32_t    _queuedCount = 0;
        };

        /**
         * @brief A deck's scripts: the engine, and the regions made.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class AngelScripts : public Scripts {
                public:
                        explicit AngelScripts(const ScriptSetup &setup);
                        ~AngelScripts() override;

                        ScriptRegion *open(const ScriptPlace &place) override;

                        /** @brief The engine (made the first time); nullptr if it can't be. */
                        asIScriptEngine *engine();
                        Platform        &platform() { return *_setup.platform; }
                        Deck            &deck() { return *_setup.deck; }
                        FontCache       &fonts() { return *_setup.fonts; }
                        ImageCache      *images() { return _setup.images; }
                        /** @brief @p path from the deck's folder into @p out; false (why in @p why) if it
                         *         isn't one a deck may read: absolute, or out of its folder. */
                        bool resolve(StringView path, char *out, size_t size, const char **why) const;
                        /** @brief The first compiler error's text to @p into, its line to @p row (nullptr: nowhere). */
                        void collectMessages(char *into, size_t size, int *row);
                        bool isTrusted() const { return _setup.trusted; }
                        /** @brief Whether @p code (a script that fails the deck profile) builds with `--trust`'s API. */
                        bool buildsTrusted(const char *code, uint32_t length);
                        void message(const asSMessageInfo *info);
                        /** @brief What a deck's engine has: the deck profile, and with @p trusted angel's system. */
                        static void registerAll(asIScriptEngine *engine, bool trusted);
                        /**
                         * @brief The exception a global's initializer threw (the engine says it as a
                         *        message; the script's context then has only "Failed to initialize"),
                         *        and its line; false if none since the last call.
                         */
                        bool takeInitException(char *out, size_t size, int *line);

                private:
                        ScriptSetup      _setup;
                        char             _folder[512] = ".";
                        asIScriptEngine *_engine = nullptr;
                        bool             _engineFailed = false;
                        asIScriptEngine *_trustedEngine = nullptr; ///< buildsTrusted()'s.
                        AngelRegion    **_regions = nullptr;
                        uint32_t         _count = 0;
                        uint32_t         _capacity = 0;
                        uint32_t         _deckSeed = 0;
                        char            *_messages = nullptr;
                        size_t           _messagesSize = 0;
                        int             *_messageRow = nullptr;
                        char             _initException[200] = {};
                        int              _initExceptionLine = 0;
        };

        /** @brief The region whose script is running now (the script API's), or nullptr. */
        AngelRegion *runningRegion();

        /** @brief A script's Canvas for draw() (api.cpp's: it draws on the running region's). */
        void *makeCanvasObject();
        void  releaseCanvasObject(void *canvas);

        /** @brief A script's Event for event() (api.cpp's), from @p input. */
        void *makeEventObject(const ScriptInput &input);
        void  releaseEventObject(void *event);

        /** @brief The deck profile (api.cpp): the scene, animations, time, the theme, the region. */
        void registerDeckApi(asIScriptEngine *engine);

        /** @brief Makes a deck's scripts (core/scripts.h's ScriptsMaker). */
        Scripts *makeAngelScripts(const ScriptSetup &setup);

        /** @brief The deck profile's declarations, sorted, one a line (`preso --api`; @p help: with each one's). */
        void printDeckApi(bool help);

} // namespace preso

#endif // PRESO_SCRIPT_RUNTIME_H
