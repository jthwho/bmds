/**
 * @file      runtime.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Scenes in AngelScript (devplan/preso.md §7, §7.9, §8c): a deck's scripts, one engine for the
 * deck made when its first region is, and a module, a context and a scene for each region; or
 * angel's stage, a window that plays one script. A region runs under a SceneHost, which is
 * either. The parts share this; the core sees only core/scripts.h.
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

        class AngelRegion;

        /**
         * @brief What a scene runs under: a deck (AngelScripts) or angel's stage (StageHost). The
         *        engine's user data (SceneHost::UserData) is its host, so the script API finds it.
         *
         * @par Thread Safety
         * Not thread-safe: the thread scripts run on.
         */
        class SceneHost {
                public:
                        /** @brief The engine's user data type a host is set as. */
                        static constexpr asPWORD UserData = 0x5CE7E;

                        virtual ~SceneHost() = default;

                        /** @brief The host of @p engine's scripts, or nullptr (an engine only listing the API). */
                        static SceneHost *of(asIScriptEngine *engine);

                        /** @brief The engine (made the first time); nullptr if it can't be. */
                        virtual asIScriptEngine *engine() = 0;
                        virtual Platform        &platform() = 0;
                        virtual FontCache       &fonts() = 0;
                        virtual ImageCache      *images() = 0;
                        /** @brief The theme of what runs without a region of its own (angel's, before its stage). */
                        virtual const Theme &theme() const = 0;
                        /** @brief The scene's size without a region (angel's stage's). */
                        virtual void size(int32_t *width, int32_t *height) const = 0;
                        /**
                         * @brief A file a script names (from its own folder: @p path joined to it already) as
                         *        a path to open, into @p out; false (why in @p why) if it may not be read.
                         */
                        virtual bool resolve(StringView path, char *out, size_t size, const char **why) const = 0;
                        /** @brief Whether files are only the deck's (no absolute paths, no `..`; §7.7). */
                        virtual bool isSandboxed() const = 0;
                        /** @brief Said once for a region that failed (a deck's diagnostics, angel's standard error). */
                        virtual void report(const ScriptPlace &place, uint32_t line, const char *message) = 0;
                        /** @brief How long a script runs without yielding before it is stopped (0: no limit). */
                        virtual int64_t runawayNs() const = 0;
                        /** @brief Whether print() goes to the program's output (angel) rather than into the region. */
                        virtual bool passesOutput() const { return false; }
                        /** @brief See AngelScripts::takeInitException(). */
                        virtual bool takeInitException(char *out, size_t size, int *line) = 0;
                        /** @brief A scene's own call (add, play, wait, pause, start) with no region running:
                         *         the region it is for, or nullptr (it does nothing). Angel's stage begins
                         *         here: the script is run again on it from its start. */
                        virtual AngelRegion *needRegion() { return nullptr; }

                        // ---- The host's own Font and Event objects (a deck's, or angel's)

                        /** @brief Font object @p font as the scene's font (a reference of its own). */
                        virtual ScriptFont *scriptFont(void *font) = 0;
                        /** @brief A new Font object for @p key (one reference); nullptr if out of memory. */
                        virtual void *makeFont(const FontKey &key) = 0;
                        virtual void  releaseFont(void *font) = 0;
                        /** @brief A new Event object for @p input (one reference); nullptr if out of memory. */
                        virtual void *makeEvent(const ScriptInput &input) = 0;
                        virtual void  releaseEvent(void *event) = 0;
        };

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

                        AngelRegion(SceneHost &owner, const ScriptPlace &place, uint32_t seed);
                        ~AngelRegion() override;

                        /** @brief Reads and builds the script, then runs it to its end (its beats). */
                        void load();
                        /**
                         * @brief Runs @p module (built by its host, which keeps it) to its end (its beats),
                         *        as load() does. The first beat played after it prints nothing until its
                         *        first scene call: the host ran that much already.
                         */
                        void adopt(asIScriptModule *module);
                        /** @brief Whether it ran to its end. */
                        bool hasFinished() const { return _state == State::Finished; }
                        /** @brief Its error (empty if none). */
                        const char *error() const { return _error; }

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
                        /**
                         * @brief A file the script names, as a path to open (@p out) and as the image
                         *        cache names it (@p cached, which may be @p out); false (why in @p why) if
                         *        it may not be read.
                         */
                        bool resolve(StringView path, char *out, size_t size, char *cached, size_t cachedSize,
                                     const char **why) const;
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
                        /** @brief Its scene's objects (their order: what is drawn over what). */
                        Scene &objects() { return _scene; }
                        /** @brief Its camera (x, y: the scene's point at its middle; scale: zoom; rotation). */
                        Shape *camera() { return _camera; }
                        /** @brief The camera as it was before a script moved it. */
                        void resetCamera();
                        /** @brief The region's place (@p x, @p y on the canvas), its fit, then the camera. */
                        Affine view(int32_t x, int32_t y) const;
                        /** @brief Updaters run, then labels and graphs' edges put where they go (before a frame). */
                        void                settle();
                        const ScriptPlace  &place() const { return _place; }
                        /** @brief The scene's size: the size asked for, when shrunk to fit (§8f). */
                        int32_t sceneWidth() const { return _place.sceneWidth > 0 ? _place.sceneWidth : _place.width; }
                        int32_t sceneHeight() const {
                                return _place.sceneHeight > 0 ? _place.sceneHeight : _place.height;
                        }
                        /** @brief The region's pixels a scene's unit: 1, or less when shrunk to fit. */
                        float      fit() const { return _context.fit; }
                        SceneHost &owner() { return _owner; }
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

                        /** @brief Builds @p code into its module (made here). @return Build()'s result. */
                        int     build(const char *code, uint32_t length);
                        void    begin();
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

                        SceneHost         &_owner;
                        ScriptPlace        _place;
                        char     _folder[256] = "."; ///< Its script's, from the deck's: files are named from it.
                        bool     _ownsModule = true;
                        bool     _quiet = false; ///< Prints nothing until a scene call (adopt()).
                        Shape   *_camera = nullptr;
                        uint32_t _cameraDrawn = 0; ///< Its signature when last drawn.
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
        class AngelScripts : public Scripts, public SceneHost {
                public:
                        explicit AngelScripts(const ScriptSetup &setup);
                        ~AngelScripts() override;

                        ScriptRegion *open(const ScriptPlace &place) override;

                        // ---- SceneHost
                        asIScriptEngine *engine() override;
                        Platform        &platform() override { return *_setup.platform; }
                        FontCache       &fonts() override { return *_setup.fonts; }
                        ImageCache      *images() override { return _setup.images; }
                        const Theme     &theme() const override;
                        void             size(int32_t *width, int32_t *height) const override;
                        /** @brief @p path from the deck's folder into @p out; false (why in @p why) if it
                         *         isn't one a deck may read: absolute, or out of its folder. */
                        bool        resolve(StringView path, char *out, size_t size, const char **why) const override;
                        bool        isSandboxed() const override { return true; }
                        void        report(const ScriptPlace &place, uint32_t line, const char *message) override;
                        int64_t     runawayNs() const override;
                        ScriptFont *scriptFont(void *font) override;
                        void       *makeFont(const FontKey &key) override;
                        void        releaseFont(void *font) override;
                        void       *makeEvent(const ScriptInput &input) override;
                        void        releaseEvent(void *event) override;

                        Deck &deck() { return *_setup.deck; }
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
                        bool takeInitException(char *out, size_t size, int *line) override;

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

        /** @brief The global `scene` (api.cpp's): what draw() is given. */
        void *sceneObject();

        /** @brief A deck's Event for event() (api.cpp's), from @p input. */
        void *makeEventObject(const ScriptInput &input);
        void  releaseEventObject(void *event);

        /**
         * @brief The scene (api.cpp): `Scene` and the global `scene`, its objects and animations,
         *        `SceneTheme`, `Ease`, `readText()`. It names `Font`, `Event` and `Callback`, which
         *        the engine has already: a deck's (registerDeckApi()) or angel's.
         */
        void registerSceneApi(asIScriptEngine *engine);

        /** @brief The deck profile (api.cpp): a deck's `Font`, `Event`, `Sound`, `Callback`, then the scene. */
        void registerDeckApi(asIScriptEngine *engine);

        /** @brief Makes a deck's scripts (core/scripts.h's ScriptsMaker). */
        Scripts *makeAngelScripts(const ScriptSetup &setup);

        /** @brief The deck profile's declarations, sorted, one a line (`preso --api`; @p help: with each one's). */
        void printDeckApi(bool help);

} // namespace preso

#endif // PRESO_SCRIPT_RUNTIME_H
