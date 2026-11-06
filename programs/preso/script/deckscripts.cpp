/**
 * @file      deckscripts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A deck's scripts (devplan/preso.md §7.9): AngelScripts, the deck's SceneHost and its regions'
 * maker, and a region's loading from its slide. Apart from region.cpp so that angel's stage,
 * which runs regions too, doesn't bring in the deck (its parser, md4c).
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

                void messageCallback(const asSMessageInfo *info, void *scripts) {
                        static_cast<AngelScripts *>(scripts)->message(info);
                        return;
                }

                uint32_t mix(uint32_t h, uint32_t v) {
                        h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
                        return h;
                }

        } // namespace

        void AngelRegion::load() {
                // A file's script from the deck's folder, or a block's from the slide. A deck's:
                // its host is AngelScripts.
                auto    &deck = static_cast<AngelScripts &>(_owner);
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
                if (_owner.engine() == nullptr) {
                        free(code);
                        fail("the script engine couldn't be made (out of memory?)");
                        return;
                }
                // Its first line may be `#!`, as angel's: blanked, the lines kept.
                if (length >= 2 && code[0] == '#' && code[1] == '!') {
                        for (uint32_t i = 0; i < length && code[i] != '\n'; i++) code[i] = ' ';
                }
                char errors[200] = {};
                int  errorLine = 0;
                deck.collectMessages(errors, sizeof errors, &errorLine);
                int built = build(code, length);
                deck.collectMessages(nullptr, 0, nullptr);
                // A script that names what only `--trust` gives: said so.
                bool trusted = built < 0 && !deck.isTrusted() && deck.buildsTrusted(code, length);
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
                begin();
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
                _engine->SetUserData(static_cast<SceneHost *>(this), UserData);
                registerAll(_engine, _setup.trusted);
                return _engine;
        }

        const Theme &AngelScripts::theme() const {
                // Every region has its slide's; this is for nothing.
                return Theme::paper();
        }

        void AngelScripts::size(int32_t *width, int32_t *height) const {
                *width = 0;
                *height = 0;
                return;
        }

        void AngelScripts::report(const ScriptPlace &place, uint32_t line, const char *message) {
                if (place.slide == nullptr) return;
                _setup.deck->report(Severity::Error, line, "slide %u: %s", unsigned(place.slide->number), message);
                return;
        }

        int64_t AngelScripts::runawayNs() const {
                return _setup.trusted ? AngelRegion::TrustedRunawayNs : AngelRegion::RunawayNs;
        }

        ScriptFont *AngelScripts::scriptFont(void *font) {
                auto *f = static_cast<ScriptFont *>(font);
                f->addRef();
                return f;
        }

        void *AngelScripts::makeFont(const FontKey &key) {
                return ScriptFont::make(key);
        }

        void AngelScripts::releaseFont(void *font) {
                static_cast<ScriptFont *>(font)->release();
                return;
        }

        void *AngelScripts::makeEvent(const ScriptInput &input) {
                return makeEventObject(input);
        }

        void AngelScripts::releaseEvent(void *event) {
                releaseEventObject(event);
                return;
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
                        _trustedEngine->SetUserData(static_cast<SceneHost *>(this), UserData);
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
