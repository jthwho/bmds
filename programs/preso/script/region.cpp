/**
 * @file      region.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Running a region's script (devplan/preso.md §7.6). The script's context is suspended in
 * play() and wait() (its clock runs, frames animate) and in pause() (a beat: Next goes on).
 * Fast-forward runs it with every play() and wait() over at once: to count its beats, for
 * Previous and jumps (from its start again: the top-level patch initializes each global where
 * it is declared, so running the statements again starts them afresh), and for what is left of
 * a beat when Next comes during it.
 */

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "angel.h"
#include "runtime.h"

namespace preso {

        namespace {

                constexpr uint32_t MaxScript = 256u << 10;
                constexpr uint32_t MaxOutput = 2048;
                constexpr Color    ErrorColor = 0xC01C28;

                AngelRegion *running = nullptr;

                void hostWrite(void *context, const char *data, size_t length) {
                        static_cast<AngelRegion *>(context)->write(data, length);
                        return;
                }

                uint64_t hostMillis(void *context) {
                        return uint64_t(static_cast<AngelRegion *>(context)->clock() * 1000.0 + 0.5);
                }

                angel::LanguageHost host = {hostWrite, hostMillis, nullptr, nullptr};

                // The region's for as long as this lives: what print(), millis() and random() reach,
                // and the API's runningRegion().
                class Running {
                        public:
                                explicit Running(AngelRegion *region) : _before(running) {
                                        running = region;
                                        host.context = region;
                                        host.random = &region->random();
                                        angel::setLanguageHost(&host);
                                }
                                ~Running() {
                                        running = _before;
                                        if (_before != nullptr) {
                                                host.context = _before;
                                                host.random = &_before->random();
                                        } else {
                                                angel::setLanguageHost(nullptr);
                                        }
                                }
                                Running(const Running &) = delete;
                                Running &operator=(const Running &) = delete;

                        private:
                                AngelRegion *_before;
                };

                void lineCallback(asIScriptContext *ctx, void *region) {
                        static_cast<AngelRegion *>(region)->line(ctx);
                        return;
                }

                void messageCallback(const asSMessageInfo *info, void *scripts) {
                        static_cast<AngelScripts *>(scripts)->message(info);
                        return;
                }

                uint32_t mix(uint32_t h, uint32_t v) {
                        h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
                        return h;
                }

        } // namespace

        AngelRegion *runningRegion() {
                return running;
        }

        // ---- AngelRegion

        AngelRegion::AngelRegion(AngelScripts &owner, const ScriptPlace &place, uint32_t seed)
            : _owner(owner), _place(place), _seed(seed) {
                _context.platform = &owner.platform();
                _context.fonts = &owner.fonts();
                _context.images = owner.images();
                _context.theme = place.theme;
        }

        AngelRegion::~AngelRegion() {
                for (uint32_t i = 0; i < _playingCount; i++) _playing[i]->release();
                _scene.clear();
                if (_ctx != nullptr) {
                        _ctx->Abort();
                        _ctx->Release();
                }
                if (_drawCtx != nullptr) _drawCtx->Release();
                if (_canvasObject != nullptr) releaseCanvasObject(_canvasObject);
                if (_module != nullptr) _module->Discard();
                free(_output);
                free(_drawn);
        }

        void AngelRegion::where(int line, char *out, size_t size) const {
                // A file's script by its name and line; a block's by the deck's line.
                if (_place.image != nullptr) {
                        StringView script = _place.image->script;
                        snprintf(out, size, "%.*s:%d", int(script.length() < 120 ? script.length() : 120),
                                 script.data(), line);
                } else {
                        snprintf(out, size, "line %u", unsigned(_place.firstLine + uint32_t(line > 0 ? line - 1 : 0)));
                }
                return;
        }

        void AngelRegion::fail(const char *message, int line) {
                if (_state == State::Failed) return;
                _state = State::Failed;
                snprintf(_error, sizeof _error, "%s", message);
                // Said once, where the deck names it (the terminal's messages, `preso check`): a
                // file's region at its image, a block at its line.
                uint32_t at = _place.image != nullptr ? _place.image->line
                                                      : _place.firstLine + uint32_t(line > 0 ? line - 1 : 0);
                if (!_reported && _place.slide != nullptr) {
                        _reported = true;
                        _owner.deck().report(Severity::Error, at, "slide %u: %s", unsigned(_place.slide->number),
                                             _error);
                }
                return;
        }

        void AngelRegion::failFrom(asIScriptContext *ctx, const char *what) {
                const char *section = nullptr;
                int         column = 0;
                int         line = ctx->GetExceptionLineNumber(&column, &section);
                char        message[256], inner[200], at[140];
                // A global's initializer threw: what it threw, where (not "Failed to initialize").
                int innerLine = 0;
                if (_owner.takeInitException(inner, sizeof inner, &innerLine) &&
                    strncmp(what, "Failed to initialize", 20) == 0) {
                        what = inner;
                        line = innerLine;
                }
                where(line, at, sizeof at);
                snprintf(message, sizeof message, "%s: %.100s", at, what);
                fail(message, line);
                return;
        }

        void AngelRegion::load() {
                // A file's script from the deck's folder, or a block's from the slide.
                char     message[256], at[140];
                char    *code = nullptr;
                uint32_t length = 0;
                if (_place.image != nullptr) {
                        StringView  script = _place.image->script;
                        char        path[512];
                        const char *why = nullptr;
                        if (!_owner.resolve(script, path, sizeof path, &why)) {
                                snprintf(message, sizeof message, "script `%.*s` %s", int(script.length()),
                                         script.data(), why);
                                fail(message);
                                return;
                        }
                        Error error = _owner.platform().readFile(path, MaxScript, &code, &length);
                        if (error != Error::Ok) {
                                snprintf(message, sizeof message, "script `%.*s` %s", int(script.length()),
                                         script.data(), error == Error::NotFound ? "isn't there" : errorName(error));
                                fail(message);
                                return;
                        }
                } else {
                        length = _place.source.length();
                        code = static_cast<char *>(malloc(length + 1));
                        if (code == nullptr) {
                                fail("out of memory for the block's script");
                                return;
                        }
                        memcpy(code, _place.source.data(), length);
                        code[length] = '\0';
                }
                asIScriptEngine *engine = _owner.engine();
                if (engine == nullptr) {
                        free(code);
                        fail("the script engine couldn't be made (out of memory?)");
                        return;
                }
                char name[24];
                snprintf(name, sizeof name, "r%p", static_cast<void *>(this));
                _module = engine->GetModule(name, asGM_ALWAYS_CREATE);
                // Its first line may be `#!`, as angel's: blanked, the lines kept.
                if (length >= 2 && code[0] == '#' && code[1] == '!') {
                        for (uint32_t i = 0; i < length && code[i] != '\n'; i++) code[i] = ' ';
                }
                char errors[200] = {};
                int  errorLine = 0;
                _owner.collectMessages(errors, sizeof errors, &errorLine);
                int built = _module == nullptr ? -1 : _module->AddScriptSection("script", code, length);
                if (built >= 0) {
                        // Globals outside top-level statements are initialized as it is built: as
                        // this region's.
                        Running scope(this);
                        built = _module->Build();
                }
                _owner.collectMessages(nullptr, 0, nullptr);
                // A script that names what only `--trust` gives: said so.
                bool trusted = built < 0 && !_owner.isTrusted() && _owner.buildsTrusted(code, length);
                free(code);
                if (built < 0) {
                        // An initializer that threw: what it threw, not "Failed to initialize".
                        char inner[200];
                        int  line = 0;
                        if (strstr(errors, "Failed to initialize") != nullptr &&
                            _owner.takeInitException(inner, sizeof inner, &line)) {
                                snprintf(errors, sizeof errors, "%s", inner);
                                errorLine = line;
                        }
                        where(errorLine, at, sizeof at);
                        snprintf(message, sizeof message, "%.120s: %s%.110s", at, trusted ? "needs --trust: " : "",
                                 errors[0] != '\0' ? errors : "the script couldn't be built");
                        fail(message, errorLine);
                        return;
                }
                _entry = _module->GetFunctionByName("$top");
                if (_entry == nullptr) _entry = _module->GetFunctionByDecl("void main()");
                _drawFunction = _module->GetFunctionByDecl("void draw(Canvas@, float)");
                _eventFunction = _module->GetFunctionByDecl("void event(Event@)");
                _tickFunction = _module->GetFunctionByDecl("void tick(float)");
                // Only handlers (or draw()) is a script too: nothing to run first.
                if (_entry == nullptr && _drawFunction == nullptr && _eventFunction == nullptr &&
                    _tickFunction == nullptr) {
                        where(1, at, sizeof at);
                        snprintf(message, sizeof message,
                                 "%s: nothing to run (statements, a main(), draw(), event() or tick())", at);
                        fail(message);
                        return;
                }
                _ctx = engine->CreateContext();
                if (_ctx == nullptr) {
                        fail("out of memory for the script's context");
                        return;
                }
                _ctx->SetLineCallback(asFUNCTION(lineCallback), this, asCALL_CDECL);
                // Run to its end once: its beats, and what is wrong in it.
                _target = UINT32_MAX;
                _toEnd = true;
                restart();
                if (_place.gated) _beat = 1; // past its gate
                _fastForward = true;
                run();
                _fastForward = false;
                _beats = _beat;
                _sought = false;
                return;
        }

        void AngelRegion::restart() {
                if (_state == State::Failed) return;
                for (uint32_t i = 0; i < _playingCount; i++) _playing[i]->release();
                _playingCount = 0;
                _scene.clear();
                _clock = 0;
                _playStart = _playEnd = 0;
                _beat = 0;
                _outputLength = 0;
                _forwardStart = 0;
                _forwardYields = 0;
                for (uint32_t i = 0; i < _startedCount; i++) _started[i].animation->release();
                _startedCount = 0;
                _changeCount = 0;
                _queuedCount = 0;
                _dragging = _pressing = nullptr;
                angel::seedRandom(_random, _seed);
                asEContextState state = _ctx->GetState();
                if (state == asEXECUTION_SUSPENDED || state == asEXECUTION_ACTIVE) _ctx->Abort();
                {
                        Running scope(this);
                        _module->ResetGlobalVars(nullptr);
                }
                if (_entry == nullptr) {
                        _state = State::Finished; // handlers only
                        return;
                }
                if (_ctx->Prepare(_entry) < 0) {
                        fail("the script couldn't be started");
                        return;
                }
                // A block waits at its gate: its first beat runs it.
                _state = _place.gated ? State::AtPause : State::Ready;
                return;
        }

        int64_t AngelRegion::runaway() const {
                return _owner.isTrusted() ? TrustedRunawayNs : RunawayNs;
        }

        void AngelRegion::line(asIScriptContext *ctx) {
                // The clock read now and then: a line costs a few dozen nanoseconds.
                if ((++_lines & 255) != 0) return;
                if (_owner.platform().now() - _runStart > runaway()) {
                        _ranAway = true;
                        ctx->Abort();
                }
                return;
        }

        bool AngelRegion::yielded() {
                // play, wait, pause: the runaway stop's clock starts again. In fast-forward, false
                // once the script has run on past any end (it loops for ever): it plays from there.
                _runStart = _owner.platform().now();
                _lines = 0;
                if (!_fastForward) return true;
                _forwardYields++;
                if (_clock - _forwardStart > EndlessSeconds || _forwardYields > EndlessYields) {
                        _fastForward = false;
                        return false;
                }
                return true;
        }

        void AngelRegion::run() {
                if (_state == State::Failed || _state == State::Finished || _ctx == nullptr) return;
                Running scope(this);
                _state = State::Ready;
                _runStart = _owner.platform().now();
                _lines = 0;
                _ranAway = false;
                int r = _ctx->Execute();
                switch (r) {
                        case asEXECUTION_SUSPENDED: break; // play(), wait() or pause() said which
                        case asEXECUTION_FINISHED:
                                endPlay();
                                _state = State::Finished;
                                break;
                        case asEXECUTION_EXCEPTION: {
                                const char *what = _ctx->GetExceptionString();
                                failFrom(_ctx, what != nullptr ? what : "an exception");
                                break;
                        }
                        case asEXECUTION_ABORTED:
                                if (_ranAway) {
                                        char message[256], at[140];
                                        where(_ctx->GetLineNumber(), at, sizeof at);
                                        snprintf(message, sizeof message,
                                                 "%.120s: ran %d ms without play(), wait() or pause(): stopped (a loop "
                                                 "that never ends?)",
                                                 at, int(runaway() / 1000000));
                                        fail(message, _ctx->GetLineNumber());
                                } else {
                                        fail("the script was stopped");
                                }
                                break;
                        default: fail("the script couldn't be run"); break;
                }
                return;
        }

        void AngelRegion::play(Animation *const *animations, uint32_t count, float seconds) {
                if (_inDraw) {
                        angel::raise("play() is for the script's statements, not draw() or a handler (start() an "
                                     "animation there)");
                        return;
                }
                if (!(seconds >= 0)) seconds = 0;
                // The play's length: its longest animation's, with its delay.
                float length = 0;
                for (uint32_t i = 0; i < count; i++) {
                        float d = animations[i]->duration >= 0 ? animations[i]->duration : seconds;
                        float end = animations[i]->delay + d;
                        if (end > length) length = end;
                }
                if (count == 0) length = seconds;
                if (yielded()) {
                        if (_fastForward) {
                                for (uint32_t i = 0; i < count; i++) {
                                        animations[i]->begin(_scene);
                                        animations[i]->end(_scene);
                                }
                                _clock += length;
                                return;
                        }
                }
                if (count > MaxPlaying) {
                        angel::raise("play() takes %u animations at most", unsigned(MaxPlaying));
                        return;
                }
                endPlay();
                for (uint32_t i = 0; i < count; i++) {
                        animations[i]->addRef();
                        _playing[_playingCount++] = animations[i];
                }
                _playStart = _clock;
                _playEnd = _clock + length;
                _playLength = seconds;
                _state = State::Playing;
                // What begins now shows as it begins (a frame at its start, as a transition's last).
                step();
                asIScriptContext *ctx = asGetActiveContext();
                if (ctx != nullptr) ctx->Suspend();
                return;
        }

        void AngelRegion::wait(float seconds) {
                if (_inDraw) {
                        angel::raise("wait() is for the script's statements, not draw() or a handler (start() an "
                                     "animation there)");
                        return;
                }
                if (!(seconds >= 0)) seconds = 0;
                if (yielded() && _fastForward) {
                        _clock += seconds;
                        return;
                }
                endPlay();
                _playStart = _clock;
                _playEnd = _clock + seconds;
                _playLength = seconds;
                _state = State::Playing;
                asIScriptContext *ctx = asGetActiveContext();
                if (ctx != nullptr) ctx->Suspend();
                return;
        }

        void AngelRegion::pause() {
                if (_inDraw) {
                        angel::raise("pause() is for the script's statements, not draw() or a handler (start() an "
                                     "animation there)");
                        return;
                }
                (void)yielded();
                if (_fastForward && _beat < _target) {
                        // Passed: the next beat, and live from here if that is where it goes.
                        _beat++;
                        if (_beat == _target && !_toEnd) _fastForward = false;
                        return;
                }
                _state = State::AtPause;
                asIScriptContext *ctx = asGetActiveContext();
                if (ctx != nullptr) ctx->Suspend();
                return;
        }

        void AngelRegion::add(Shape *shape) {
                (void)_scene.add(shape);
                return;
        }

        void AngelRegion::remove(Shape *shape) {
                _scene.remove(shape);
                return;
        }

        void AngelRegion::write(const char *data, size_t length) {
                if (_output == nullptr) {
                        _output = static_cast<char *>(malloc(MaxOutput));
                        if (_output == nullptr) return;
                }
                // The last MaxOutput bytes are kept.
                if (length >= MaxOutput) {
                        data += length - (MaxOutput - 1);
                        length = MaxOutput - 1;
                }
                if (_outputLength + length >= MaxOutput) {
                        uint32_t drop = uint32_t(_outputLength + length - (MaxOutput - 1));
                        memmove(_output, _output + drop, _outputLength - drop);
                        _outputLength -= drop;
                }
                memcpy(_output + _outputLength, data, length);
                _outputLength += uint32_t(length);
                return;
        }

        void AngelRegion::step() {
                // Each animation of the play at its point: begun at its delay, ended at its length.
                for (uint32_t i = 0; i < _playingCount; i++) {
                        Animation *a = _playing[i];
                        if (a->hasEnded()) continue;
                        double local = _clock - (_playStart + a->delay);
                        if (local < 0) continue;
                        if (!a->hasBegun()) a->begin(_scene);
                        float length = a->duration >= 0 ? a->duration : _playLength;
                        if (length <= 0 || local >= length) {
                                a->end(_scene);
                        } else {
                                a->apply(float(local / length));
                        }
                }
                return;
        }

        void AngelRegion::endPlay() {
                for (uint32_t i = 0; i < _playingCount; i++) {
                        _playing[i]->end(_scene);
                        _playing[i]->release();
                }
                _playingCount = 0;
                if (_clock < _playEnd) _clock = _playEnd;
                return;
        }

        void AngelRegion::seek(uint32_t beat, bool played) {
                if (_state == State::Failed || _ctx == nullptr) return;
                if (_sought && !_movedSince && _soughtBeat == beat && _soughtPlayed == played) return;
                _target = beat;
                _toEnd = played;
                restart();
                bool runs = true;
                if (_place.gated) {
                        // Beat 0 is the gate, before the script: nothing has run.
                        if (beat == 0) {
                                runs = false;
                        } else {
                                _beat = 1;
                        }
                }
                if (runs) {
                        _fastForward = !(beat == _beat && !played);
                        this->run();
                        _fastForward = false;
                }
                _sought = true;
                _soughtBeat = beat;
                _soughtPlayed = played;
                _movedSince = false;
                return;
        }

        void AngelRegion::next() {
                if (_state == State::Failed || _state == State::Finished || _ctx == nullptr) return;
                _movedSince = true;
                if (_state == State::Playing) {
                        // The rest of this beat at once: to its pause (or its end).
                        endPlay();
                        _target = _beat;
                        _toEnd = true;
                        _fastForward = true;
                        _forwardStart = _clock;
                        _forwardYields = 0;
                        run();
                        _fastForward = false;
                }
                if (_state == State::AtPause) {
                        _beat++;
                        run();
                }
                return;
        }

        bool AngelRegion::isMoving() const {
                if (_state == State::Failed) return false;
                return _state == State::Playing || _startedCount > 0 || (_entered && _tickFunction != nullptr);
        }

        uint32_t AngelRegion::advance(int64_t ns, Rect *areas) {
                if (!isMoving()) return 0;
                _movedSince = true;
                if (_state == State::Playing) {
                        _clock += double(ns) / 1e9;
                        if (_clock >= _playEnd) {
                                // This play is over: the script goes on (to another, a pause, its end).
                                _clock = _playEnd;
                                endPlay();
                                run();
                        } else {
                                step();
                        }
                }
                stepStarted(ns);
                if (_entered && _tickFunction != nullptr && _state != State::Failed)
                        (void)handler(_tickFunction, nullptr, float(double(ns) / 1e9), false);
                return changes(areas);
        }

        // ---- Interactive pieces

        bool AngelRegion::handler(asIScriptFunction *function, void *object, float number, bool withObject) {
                // On the handlers' context (draw()'s): the script's own is suspended in its statements.
                asIScriptEngine *engine = _owner.engine();
                if (engine == nullptr || _state == State::Failed) return false;
                if (_drawCtx == nullptr) {
                        _drawCtx = engine->CreateContext();
                        if (_drawCtx == nullptr) return false;
                        _drawCtx->SetLineCallback(asFUNCTION(lineCallback), this, asCALL_CDECL);
                }
                if (_drawCtx->Prepare(function) < 0) return false;
                if (withObject) {
                        _drawCtx->SetArgObject(0, object);
                } else if (function->GetParamCount() == 1) {
                        _drawCtx->SetArgFloat(0, number);
                }
                Running scope(this);
                bool    was = _inDraw;
                _inDraw = true;
                _runStart = _owner.platform().now();
                _lines = 0;
                _ranAway = false;
                int r = _drawCtx->Execute();
                _inDraw = was;
                if (r == asEXECUTION_EXCEPTION) {
                        const char *what = _drawCtx->GetExceptionString();
                        failFrom(_drawCtx, what != nullptr ? what : "an exception in a handler");
                } else if (r == asEXECUTION_ABORTED) {
                        char message[200], at[140];
                        where(_drawCtx->GetLineNumber(), at, sizeof at);
                        snprintf(message, sizeof message, "%s: a handler ran %d ms: stopped", at,
                                 int(runaway() / 1000000));
                        fail(message, _drawCtx->GetLineNumber());
                }
                _movedSince = true;
                // The widgets it changed: their callbacks now (they may change more, a few rounds).
                for (uint32_t round = 0; round < 4 && _changeCount > 0 && _state != State::Failed; round++) {
                        Shape   *changed[8];
                        uint32_t n = _changeCount;
                        memcpy(changed, _changes, n * sizeof(Shape *));
                        _changeCount = 0;
                        for (uint32_t i = 0; i < n; i++) {
                                if (changed[i]->callback == nullptr) continue;
                                (void)handler(static_cast<asIScriptFunction *>(changed[i]->callback), nullptr, 0,
                                              false);
                        }
                }
                return _state != State::Failed;
        }

        void AngelRegion::start(Animation *animation, float seconds) {
                if (!(seconds >= 0)) seconds = 0;
                float length = (animation->duration >= 0 ? animation->duration : seconds) + animation->delay;
                if (_fastForward || length <= 0) {
                        animation->begin(_scene);
                        animation->end(_scene);
                        return;
                }
                if (_startedCount == MaxStarted) {
                        // No room: the oldest ends at once.
                        _started[0].animation->end(_scene);
                        _started[0].animation->release();
                        memmove(_started, _started + 1, (MaxStarted - 1) * sizeof(Started));
                        _startedCount--;
                }
                animation->addRef();
                _started[_startedCount++] = Started{animation, 0, length};
                if (animation->delay <= 0) animation->begin(_scene);
                _movedSince = true;
                return;
        }

        void AngelRegion::stepStarted(int64_t ns) {
                float    dt = float(double(ns) / 1e9);
                uint32_t kept = 0;
                for (uint32_t i = 0; i < _startedCount; i++) {
                        Started   &st = _started[i];
                        Animation *a = st.animation;
                        st.elapsed += dt;
                        float local = st.elapsed - a->delay;
                        if (local >= 0 && !a->hasBegun()) a->begin(_scene);
                        float length = st.length - a->delay;
                        if (st.elapsed >= st.length || length <= 0) {
                                a->end(_scene);
                                a->release();
                                continue;
                        }
                        if (local >= 0) a->apply(local / length);
                        _started[kept++] = st;
                }
                _startedCount = kept;
                return;
        }

        void AngelRegion::playSound(int32_t sound, float volume) {
                // Never in fast-forward; before its slide shows (drawn ahead), when it does.
                if (_fastForward || sound < 0) return;
                if (_audible) {
                        _owner.platform().playSound(sound, volume);
                } else if (_queuedCount < MaxQueuedSounds) {
                        _queued[_queuedCount++] = QueuedSound{sound, volume};
                }
                return;
        }

        void AngelRegion::setAudible(bool audible) {
                _audible = audible;
                if (audible) {
                        for (uint32_t i = 0; i < _queuedCount; i++)
                                _owner.platform().playSound(_queued[i].sound, _queued[i].volume);
                }
                _queuedCount = 0;
                return;
        }

        void AngelRegion::changed(Shape *widget) {
                for (uint32_t i = 0; i < _changeCount; i++) {
                        if (_changes[i] == widget) return;
                }
                if (_changeCount < 8) _changes[_changeCount++] = widget;
                return;
        }

        bool AngelRegion::isInteractive() const {
                if (_state == State::Failed) return false;
                if (_eventFunction != nullptr || _tickFunction != nullptr) return true;
                for (uint32_t i = 0; i < _scene.count(); i++) {
                        if (_scene.at(i)->isWidget()) return true;
                }
                return false;
        }

        Shape *AngelRegion::focusedWidget() {
                for (uint32_t i = 0; i < _scene.count(); i++) {
                        if (_scene.at(i)->isWidget() && _scene.at(i)->focused) return _scene.at(i);
                }
                return nullptr;
        }

        void AngelRegion::focusNext(int32_t by) {
                // Among the region's widgets, in the order they were added, round.
                Shape   *list[64];
                uint32_t n = 0;
                int32_t  at = -1;
                for (uint32_t i = 0; i < _scene.count() && n < 64; i++) {
                        Shape *s = _scene.at(i);
                        if (!s->isWidget() || !s->visible) continue;
                        if (s->focused) at = int32_t(n);
                        s->focused = false;
                        list[n++] = s;
                }
                if (n == 0) return;
                int32_t next = at < 0 ? 0 : (at + by + int32_t(n)) % int32_t(n);
                list[next]->focused = true;
                return;
        }

        void AngelRegion::setEntered(bool entered) {
                if (_entered == entered) return;
                _entered = entered;
                _movedSince = true;
                if (entered) {
                        if (focusedWidget() == nullptr) focusNext(1);
                } else {
                        for (uint32_t i = 0; i < _scene.count(); i++) _scene.at(i)->focused = false;
                        if (_pressing != nullptr) _pressing->pressed = false;
                        _dragging = _pressing = nullptr;
                }
                return;
        }

        Shape *AngelRegion::widgetAt(int32_t x, int32_t y) {
                // The topmost: the last added.
                Affine none;
                for (uint32_t i = _scene.count(); i-- > 0;) {
                        Shape *s = _scene.at(i);
                        if (!s->isWidget() || !s->visible) continue;
                        Rect b = shapeBounds(_context, *s, none);
                        if (x >= b.x && y >= b.y && x < b.x + b.width && y < b.y + b.height) return s;
                }
                return nullptr;
        }

        void AngelRegion::dragTo(Shape *slider, int32_t x) {
                float scale = slider->scale != 0 ? slider->scale : 1;
                float v = slider->sliderValueAt((float(x) - slider->x) / scale);
                if (v == slider->value) return;
                slider->value = v;
                changed(slider);
                return;
        }

        bool AngelRegion::widgetKey(const ScriptInput &in) {
                // Tab among the widgets; arrows move a slider; Space or Enter presses.
                if (in.type != ScriptInput::Type::KeyDown) return false;
                uint32_t usage = in.key & 0xFFFF;
                if (usage == 0x2B) {
                        if (focusedWidget() == nullptr && !isInteractive()) return false;
                        focusNext(1);
                        return true;
                }
                Shape *w = focusedWidget();
                if (w == nullptr) return false;
                bool press = usage == 0x2C || usage == 0x28 || usage == 0x58;
                switch (w->kind) {
                        case ShapeKind::Slider: {
                                float step = (w->maximum - w->minimum) / 20;
                                float v = w->value;
                                if (usage == 0x4F || usage == 0x52)
                                        v += step;
                                else if (usage == 0x50 || usage == 0x51)
                                        v -= step;
                                else if (usage == 0x4A)
                                        v = w->minimum;
                                else if (usage == 0x4D)
                                        v = w->maximum;
                                else
                                        return false;
                                v = v < w->minimum ? w->minimum : v > w->maximum ? w->maximum : v;
                                if (v != w->value) {
                                        w->value = v;
                                        changed(w);
                                }
                                return true;
                        }
                        case ShapeKind::Button:
                                if (!press) return false;
                                changed(w);
                                return true;
                        case ShapeKind::Toggle:
                                if (!press) return false;
                                w->on = !w->on;
                                changed(w);
                                return true;
                        default: return false;
                }
        }

        uint32_t AngelRegion::input(const ScriptInput &in, Rect *areas) {
                if (_state == State::Failed) return 0;
                bool key = in.type == ScriptInput::Type::KeyDown || in.type == ScriptInput::Type::KeyUp;
                if (key && !_entered) return 0;
                _movedSince = true;
                bool used = false;
                // Widgets first: the keys that move among them, the pointer over them.
                if (key) {
                        used = widgetKey(in);
                } else if (in.type == ScriptInput::Type::PointerDown && in.button == 1) {
                        Shape *w = widgetAt(in.x, in.y);
                        if (w != nullptr) {
                                for (uint32_t i = 0; i < _scene.count(); i++) _scene.at(i)->focused = false;
                                w->focused = _entered;
                                used = true;
                                if (w->kind == ShapeKind::Slider) {
                                        _dragging = w;
                                        dragTo(w, in.x);
                                } else if (w->kind == ShapeKind::Button) {
                                        _pressing = w;
                                        w->pressed = true;
                                } else if (w->kind == ShapeKind::Toggle) {
                                        w->on = !w->on;
                                        changed(w);
                                }
                        }
                } else if (in.type == ScriptInput::Type::PointerMove && _dragging != nullptr) {
                        dragTo(_dragging, in.x);
                        used = true;
                } else if (in.type == ScriptInput::Type::PointerUp) {
                        if (_dragging != nullptr) used = true;
                        _dragging = nullptr;
                        if (_pressing != nullptr) {
                                // A click: let go over it.
                                _pressing->pressed = false;
                                if (widgetAt(in.x, in.y) == _pressing) changed(_pressing);
                                _pressing = nullptr;
                                used = true;
                        }
                }
                // Then the script's event(), with what the widgets didn't take.
                if (!used && _eventFunction != nullptr) {
                        void *event = makeEventObject(in);
                        if (event != nullptr) {
                                (void)handler(_eventFunction, event, 0, true);
                                releaseEventObject(event);
                        }
                } else if (_changeCount > 0) {
                        // Widgets changed outside a handler: their callbacks.
                        for (uint32_t round = 0; round < 4 && _changeCount > 0 && _state != State::Failed; round++) {
                                Shape   *changedNow[8];
                                uint32_t n = _changeCount;
                                memcpy(changedNow, _changes, n * sizeof(Shape *));
                                _changeCount = 0;
                                for (uint32_t i = 0; i < n; i++) {
                                        if (changedNow[i]->callback == nullptr) continue;
                                        (void)handler(static_cast<asIScriptFunction *>(changedNow[i]->callback),
                                                      nullptr, 0, false);
                                }
                        }
                }
                return changes(areas);
        }

        bool AngelRegion::drawnWhole() const {
                // draw()'s may be anywhere; so may new lines print()ed, and an error's box.
                return _drawFunction != nullptr || _outputLength != _outputDrawn || _state == State::Failed;
        }

        namespace {

                // @p r into @p areas (@p n so far): joined to one it overlaps, or when there is no room.
                void addArea(Rect *areas, uint32_t &n, const Rect &r) {
                        if (r.isEmpty()) return;
                        for (uint32_t i = 0; i < n; i++) {
                                if (!areas[i].intersected(r).isEmpty()) {
                                        areas[i] = uniteRects(areas[i], r);
                                        return;
                                }
                        }
                        if (n < ScriptRegion::MaxAreas) {
                                areas[n++] = r;
                        } else {
                                areas[n - 1] = uniteRects(areas[n - 1], r);
                        }
                        return;
                }

        } // namespace

        uint32_t AngelRegion::changes(Rect *areas) {
                Rect whole{0, 0, _place.width, _place.height};
                if (drawnWhole()) {
                        areas[0] = whole;
                        return 1;
                }
                // Each object that looks different or moved: where it was and where it is. One gone:
                // where it was.
                uint32_t n = 0;
                bool     seen[64] = {};
                Affine   none;
                for (uint32_t i = 0; i < _scene.count(); i++) {
                        Shape   *shape = _scene.at(i);
                        uint32_t signature = shapeSignature(*shape);
                        int32_t  was = -1;
                        for (uint32_t k = 0; k < _drawnCount; k++) {
                                if (_drawn[k].shape == shape) {
                                        was = int32_t(k);
                                        break;
                                }
                        }
                        if (was >= 0 && was < 64) seen[was] = true;
                        if (was >= 0 && _drawn[was].signature == signature) continue;
                        if (was >= 0) addArea(areas, n, _drawn[was].bounds);
                        addArea(areas, n, shapeBounds(_context, *shape, none).intersected(whole));
                }
                for (uint32_t k = 0; k < _drawnCount; k++) {
                        if (k >= 64 || !seen[k]) addArea(areas, n, _drawn[k].bounds);
                }
                // Joined parts may now overlap: joined again.
                for (bool joined = true; joined;) {
                        joined = false;
                        for (uint32_t a = 0; a < n && !joined; a++) {
                                for (uint32_t b = a + 1; b < n && !joined; b++) {
                                        if (areas[a].intersected(areas[b]).isEmpty()) continue;
                                        areas[a] = uniteRects(areas[a], areas[b]);
                                        areas[b] = areas[--n];
                                        joined = true;
                                }
                        }
                }
                return n;
        }

        void AngelRegion::remember() {
                // As it was just drawn.
                _outputDrawn = _outputLength;
                uint32_t count = _scene.count();
                if (count > _drawnCapacity) {
                        auto *grown = static_cast<Drawn *>(realloc(_drawn, count * sizeof(Drawn)));
                        if (grown == nullptr) {
                                _drawnCount = 0;
                                return;
                        }
                        _drawn = grown;
                        _drawnCapacity = count;
                }
                Rect   whole{0, 0, _place.width, _place.height};
                Affine none;
                for (uint32_t i = 0; i < count; i++) {
                        Shape *shape = _scene.at(i);
                        _drawn[i] = Drawn{shape, shapeSignature(*shape),
                                          shapeBounds(_context, *shape, none).intersected(whole)};
                }
                _drawnCount = count;
                return;
        }

        void AngelRegion::draw(Canvas &canvas, int32_t x, int32_t y) {
                if (_state == State::Failed) {
                        drawError(canvas, x, y);
                        return;
                }
                if (_drawFunction != nullptr) {
                        // Its own drawing, under its objects.
                        asIScriptEngine *engine = _owner.engine();
                        if (_drawCtx == nullptr && engine != nullptr) {
                                _drawCtx = engine->CreateContext();
                                if (_drawCtx != nullptr)
                                        _drawCtx->SetLineCallback(asFUNCTION(lineCallback), this, asCALL_CDECL);
                        }
                        if (_canvasObject == nullptr) _canvasObject = makeCanvasObject();
                        void *canvasObject = _canvasObject;
                        if (_drawCtx != nullptr && canvasObject != nullptr && _drawCtx->Prepare(_drawFunction) >= 0) {
                                Running scope(this);
                                _drawCanvas = &canvas;
                                _drawX = x;
                                _drawY = y;
                                _inDraw = true;
                                _drawCtx->SetArgObject(0, canvasObject);
                                _drawCtx->SetArgFloat(1, float(_clock));
                                _runStart = _owner.platform().now();
                                _lines = 0;
                                _ranAway = false;
                                int r = _drawCtx->Execute();
                                _inDraw = false;
                                _drawCanvas = nullptr;
                                if (r == asEXECUTION_EXCEPTION) {
                                        const char *what = _drawCtx->GetExceptionString();
                                        failFrom(_drawCtx, what != nullptr ? what : "an exception in draw()");
                                } else if (r == asEXECUTION_ABORTED) {
                                        fail("draw() ran 100 ms: stopped");
                                }
                                if (_state == State::Failed) {
                                        drawError(canvas, x, y);
                                        return;
                                }
                        }
                }
                _scene.draw(canvas, _context, x, y);
                if (_place.console) {
                        drawConsole(canvas, x, y);
                } else {
                        drawOutput(canvas, x, y);
                }
                if (_entered && _place.theme != nullptr) {
                        // Entered: a thin frame in the accent, inside its edge.
                        Color   c = _place.theme->accent;
                        int32_t w = _place.width, h = _place.height;
                        canvas.fill(Rect{x, y, w, 2}, c);
                        canvas.fill(Rect{x, y + h - 2, w, 2}, c);
                        canvas.fill(Rect{x, y, 2, h}, c);
                        canvas.fill(Rect{x + w - 2, y, 2, h}, c);
                }
                remember();
                return;
        }

        void AngelRegion::drawOutput(Canvas &canvas, int32_t x, int32_t y) {
                if (_outputLength == 0 || _place.theme == nullptr) return;
                // The last lines that fit, from its top left, in the theme's code font.
                FontKey key = _place.theme->code.font;
                key.size = uint16_t(key.size * 3 / 4 < 12 ? 12 : key.size * 3 / 4);
                FontMetrics m = _context.fonts->metrics(key);
                if (m.lineHeight <= 0) return;
                uint32_t   fit = uint32_t(_place.height / m.lineHeight);
                StringView all(_output, _outputLength);
                if (all.endsWith("\n")) all = all.left(all.length() - 1);
                uint32_t lines = 0;
                forEachLine(all, [&](StringView, uint32_t) { lines++; });
                uint32_t skip = lines > fit ? lines - fit : 0;
                uint32_t n = 0;
                forEachLine(all, [&](StringView line, uint32_t) {
                        if (n++ < skip) return;
                        _context.fonts->draw(canvas, key, x + 4, y + 2 + int32_t(n - 1 - skip) * m.lineHeight, line,
                                             _place.theme->body.color);
                });
                return;
        }

        void AngelRegion::drawConsole(Canvas &canvas, int32_t x, int32_t y) {
                // A block's pane: its printed lines in the code font, the last that fit.
                if (_outputLength == 0 || _place.theme == nullptr) return;
                FontKey     key = _place.theme->code.font;
                FontMetrics m = _context.fonts->metrics(key);
                if (m.lineHeight <= 0) return;
                uint32_t   fit = uint32_t(_place.height / m.lineHeight);
                StringView all(_output, _outputLength);
                if (all.endsWith("\n")) all = all.left(all.length() - 1);
                uint32_t lines = 0;
                forEachLine(all, [&](StringView, uint32_t) { lines++; });
                uint32_t skip = lines > fit ? lines - fit : 0;
                uint32_t n = 0;
                forEachLine(all, [&](StringView line, uint32_t) {
                        if (n++ < skip) return;
                        _context.fonts->draw(canvas, key, x, y + int32_t(n - 1 - skip) * m.lineHeight, line,
                                             _place.theme->code.color);
                });
                return;
        }

        void AngelRegion::drawError(Canvas &canvas, int32_t x, int32_t y) {
                // A marked box with what went wrong, as a missing picture's.
                int32_t w = _place.width, h = _place.height;
                canvas.fill(Rect{x, y, w, 2}, ErrorColor);
                canvas.fill(Rect{x, y + h - 2, w, 2}, ErrorColor);
                canvas.fill(Rect{x, y, 2, h}, ErrorColor);
                canvas.fill(Rect{x + w - 2, y, 2, h}, ErrorColor);
                if (_place.theme == nullptr) return;
                FontKey     key = _place.theme->header.font;
                FontMetrics m = _context.fonts->metrics(key);
                StringView  text(_error);
                int32_t     ty = y + 8;
                while (!text.isEmpty() && ty + m.lineHeight <= y + h) {
                        uint32_t n = _context.fonts->fit(key, text, w - 16);
                        if (n == 0) break;
                        // At a space, if there is one.
                        if (n < text.length()) {
                                uint32_t space = n;
                                while (space > 0 && text[space] != ' ') space--;
                                if (space > 0) n = space;
                        }
                        _context.fonts->draw(canvas, key, x + 8, ty, text.left(n), ErrorColor);
                        text = text.mid(n).trimmed();
                        ty += m.lineHeight;
                }
                return;
        }

        // ---- AngelScripts

        AngelScripts::AngelScripts(const ScriptSetup &setup) : _setup(setup) {
                folderOf(setup.deckPath, _folder, sizeof _folder);
                // The deck's seed: its file's name (not where it is: the same deck, the same numbers).
                const char *name = setup.deckPath;
                for (const char *p = setup.deckPath; *p != '\0'; p++) {
                        if (*p == '/') name = p + 1;
                }
                uint32_t h = 2166136261u;
                for (const char *p = name; *p != '\0'; p++) h = (h ^ uint8_t(*p)) * 16777619u;
                _deckSeed = h;
        }

        AngelScripts::~AngelScripts() {
                for (uint32_t i = 0; i < _count; i++) {
                        _regions[i]->~AngelRegion();
                        free(_regions[i]);
                }
                free(_regions);
                if (_trustedEngine != nullptr) _trustedEngine->ShutDownAndRelease();
                if (_engine != nullptr) {
                        // Objects the canvas object held, then the engine.
                        _engine->ShutDownAndRelease();
                        angel::forgetHelp();
                }
        }

        asIScriptEngine *AngelScripts::engine() {
                if (_engine != nullptr || _engineFailed) return _engine;
                _engine = angel::createEngine(messageCallback, this, true);
                if (_engine == nullptr) {
                        _engineFailed = true;
                        return nullptr;
                }
                registerAll(_engine, _setup.trusted);
                return _engine;
        }

        void AngelScripts::registerAll(asIScriptEngine *engine, bool trusted) {
                angel::registerString(engine);
                angel::registerLanguage(engine);
                angel::registerColors(engine);
                registerDeckApi(engine);
                if (trusted) {
                        // angel's system without its windows, drawing, sound or GUI, whose names are
                        // the deck's; nor its processes, which answer through angel's GUI loop.
                        angel::registerFiles(engine);
                        angel::registerSystem(engine);
                        angel::registerNetwork(engine);
                }
                return;
        }

        bool AngelScripts::buildsTrusted(const char *code, uint32_t length) {
                if (_trustedEngine == nullptr) {
                        _trustedEngine = angel::createEngine(nullptr, nullptr, true);
                        if (_trustedEngine == nullptr) return false;
                        registerAll(_trustedEngine, true);
                }
                asIScriptModule *module = _trustedEngine->GetModule("trusted", asGM_ALWAYS_CREATE);
                bool             built = module != nullptr && module->AddScriptSection("script", code, length) >= 0 &&
                             module->Build() >= 0;
                if (module != nullptr) module->Discard();
                return built;
        }

        void AngelScripts::collectMessages(char *into, size_t size, int *row) {
                _messages = into;
                _messagesSize = size;
                _messageRow = row;
                return;
        }

        void AngelScripts::message(const asSMessageInfo *info) {
                // A global's initializer that threw: "Exception 'WHAT' in 'FUNCTION'", kept.
                if (info->type == asMSGTYPE_INFORMATION && strncmp(info->message, "Exception '", 11) == 0) {
                        const char *what = info->message + 11;
                        const char *end = strstr(what, "' in '");
                        int         n = end != nullptr ? int(end - what) : int(strlen(what));
                        snprintf(_initException, sizeof _initException, "%.*s", n, what);
                        _initExceptionLine = info->row;
                        return;
                }
                // The first error is the one shown (as compilers' others often follow from it).
                if (_messages == nullptr || info->type != asMSGTYPE_ERROR || _messages[0] != '\0') return;
                snprintf(_messages, _messagesSize, "%s", info->message);
                if (_messageRow != nullptr) *_messageRow = info->row;
                return;
        }

        bool AngelScripts::takeInitException(char *out, size_t size, int *line) {
                if (_initException[0] == '\0') return false;
                snprintf(out, size, "%s", _initException);
                *line = _initExceptionLine;
                _initException[0] = '\0';
                return true;
        }

        bool AngelScripts::resolve(StringView path, char *out, size_t size, const char **why) const {
                // Inside the deck's folder only (devplan/preso.md §7.7): a deck someone sends can't
                // read anything else.
                if (path.isEmpty()) {
                        *why = "has no name";
                        return false;
                }
                if (path[0] == '/' || path.indexOf('\\') >= 0 || path.indexOf(':') >= 0) {
                        *why = "isn't in the deck's folder (a deck's files are named from it)";
                        return false;
                }
                bool     outside = false;
                uint32_t start = 0;
                for (uint32_t i = 0; i <= path.length(); i++) {
                        if (i == path.length() || path[i] == '/') {
                                if (path.mid(start, i - start).equals("..")) outside = true;
                                start = i + 1;
                        }
                }
                if (outside) {
                        *why = "isn't in the deck's folder (`..` leaves it)";
                        return false;
                }
                int n = strcmp(_folder, ".") == 0
                                ? snprintf(out, size, "%.*s", int(path.length()), path.data())
                                : snprintf(out, size, "%s/%.*s", _folder, int(path.length()), path.data());
                if (n < 0 || size_t(n) >= size) {
                        *why = "has too long a name";
                        return false;
                }
                return true;
        }

        ScriptRegion *AngelScripts::open(const ScriptPlace &place) {
                if (_count == _capacity) {
                        uint32_t capacity = _capacity != 0 ? _capacity * 2 : 8;
                        auto *grown = static_cast<AngelRegion **>(realloc(_regions, capacity * sizeof(AngelRegion *)));
                        if (grown == nullptr) return nullptr;
                        _regions = grown;
                        _capacity = capacity;
                }
                void *p = malloc(sizeof(AngelRegion));
                if (p == nullptr) return nullptr;
                uint32_t seed = mix(mix(_deckSeed, place.slide != nullptr ? place.slide->number : 0), place.index);
                auto    *region = new (p) AngelRegion(*this, place, seed);
                _regions[_count++] = region;
                region->load();
                return region;
        }

        Scripts *makeAngelScripts(const ScriptSetup &setup) {
                void *p = malloc(sizeof(AngelScripts));
                return p != nullptr ? new (p) AngelScripts(setup) : nullptr;
        }

} // namespace preso
