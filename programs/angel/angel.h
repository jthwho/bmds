/**
 * @file      angel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * angel, the AngelScript runtime (devplan/scripting.md): what its parts share. Every
 * function scripts see is registered through def() and method(), which use the generic
 * calling convention (RV32 has no native one in AngelScript) and keep a line of help for
 * `angel --api-help`. The script API is userspace ABI: append only (tests/scripts/api.lock).
 */

#ifndef ANGEL_H
#define ANGEL_H

#include <angelscript.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace angel {

        // ---- Text built up in a growable buffer (no libstdc++ here).
        class Text {
                public:
                        Text() = default;
                        ~Text();
                        Text(const Text &) = delete;
                        Text &operator=(const Text &) = delete;

                        void        append(const char *data, size_t length);
                        void        append(const char *text);
                        void        appendf(const char *format, ...) __attribute__((format(printf, 2, 3)));
                        void        clear() { _length = 0; }
                        const char *data() const { return _data != nullptr ? _data : ""; }
                        size_t      length() const { return _length; }
                        // Hands the buffer over (NUL-terminated; malloc'd, or nullptr if empty).
                        char *release();

                private:
                        bool reserve(size_t length);

                        char  *_data = nullptr;
                        size_t _length = 0;
                        size_t _capacity = 0;
        };

        // ---- The script string (string.cpp): bytes (UTF-8 as it comes), a value type.
        class String {
                public:
                        String() = default;
                        String(const char *data, size_t length);
                        explicit String(const char *text);
                        String(const String &other);
                        ~String();
                        String &operator=(const String &other);

                        void        assign(const char *data, size_t length);
                        void        append(const char *data, size_t length);
                        const char *data() const { return _data != nullptr ? _data : ""; }
                        // NUL-terminated (the buffer always is).
                        const char *cStr() const { return data(); }
                        uint32_t    length() const { return _length; }
                        char       *mutableData() { return _data; }
                        // Takes over a malloc'd, NUL-terminated buffer of @p length bytes.
                        void adopt(char *data, size_t length);

                private:
                        bool reserve(size_t length);

                        char    *_data = nullptr;
                        uint32_t _length = 0;
                        uint32_t _capacity = 0;
        };

        // ---- Registration (api.cpp).
        // A global function in the generic convention, with its help.
        void def(asIScriptEngine *engine, const char *decl, asGENFUNC_t function, const char *help);
        // A method or behaviour of @p type.
        void method(asIScriptEngine *engine, const char *type, const char *decl, asGENFUNC_t function,
                    const char *help);
        void behaviour(asIScriptEngine *engine, const char *type, asEBehaviours behaviour, const char *decl,
                       asGENFUNC_t function, const char *help);
        // The same with an auxiliary pointer the function reads (`GetAuxiliary()`): one function
        // for many registrations (the GUI's, gui.cpp). @p help is kept, not copied.
        void methodAux(asIScriptEngine *engine, const char *type, const char *decl, asGENFUNC_t function, void *aux,
                       const char *help);
        void behaviourAux(asIScriptEngine *engine, const char *type, asEBehaviours behaviour, const char *decl,
                          asGENFUNC_t function, void *aux, const char *help);
        // A global property (its address the application's), with its help.
        void property(asIScriptEngine *engine, const char *decl, void *address, const char *help);
        // Registration failed: a bug in angel; says so and ends.
        void check(int r, const char *what);
        // Every declaration registered, one a line, sorted (`--api`); with their help (`--api-help`).
        void printApi(asIScriptEngine *engine, bool help);
        // The same as records for an editor (`angel --serve`'s `api`): `api KIND NAME DECLARATION
        // HELP`, tab-separated, a line each; KIND is type, constructor, method, property, enum,
        // constant, function or variable; a member's NAME is `Type.name`.
        void printApiRecords(asIScriptEngine *engine);

        // ---- The parts (each registers its own).
        void registerString(asIScriptEngine *engine);
        void registerCore(asIScriptEngine *engine);
        void registerConsole(asIScriptEngine *engine);
        void registerFiles(asIScriptEngine *engine);
        void registerSystem(asIScriptEngine *engine);
        void registerGraphics(asIScriptEngine *engine);
        void registerSound(asIScriptEngine *engine);
        void registerNetwork(asIScriptEngine *engine);
        void registerGui(asIScriptEngine *engine);
        // The GUI's classes' members, factories and casts, for the classes @p count scripts
        // (@p texts, @p lengths bytes each) name and their parents; all of them with no scripts
        // (null: `--api`, an editor's). Registering all of them costs a second on the board, so a
        // script gets what it names, before it is built. Each class once.
        void registerGuiClasses(asIScriptEngine *engine, const char *const *texts, const size_t *lengths, size_t count);

        // An angel Image (graphics.cpp, opaque here) for the GUI (gui.cpp), which sends its pixels
        // as an icon's: a reference taken or let go, its size, and a number its drawing changes.
        struct ImageInfo {
                        int      width;
                        int      height;
                        uint32_t generation;
        };
        void      imageAddRef(void *image);
        void      imageRelease(void *image);
        ImageInfo imageInfo(void *image);
        // Its pixels as 0xAARRGGBB into @p out (width × height), transparent where it is keyed.
        void imageArgb(void *image, uint32_t *out);

        // A GUI canvas drawn as a Screen is (graphics.cpp's pictures, gui.cpp's Canvas). Its drawing
        // methods find their picture through the hooks (their auxiliary pointer): given the
        // script's object, the picture, or null with an exception raised.
        struct CanvasHooks {
                        void *(*picture)(void *object);
        };
        void registerCanvasDrawing(asIScriptEngine *engine, const char *type, const CanvasHooks *hooks);
        // A canvas's picture in memory, white, until it has a surface; null with an exception raised.
        void *canvasMake(int width, int height);
        // Shown through surface @p fd (taken: its buffers @p width x @p height) from now on, the
        // picture so far copied over. @return 0, or the error (@p fd not taken).
        int  canvasShow(void *picture, int fd, int width, int height);
        void canvasSize(void *picture, int &width, int &height);
        void canvasRelease(void *picture);
        // What was drawn on the canvases since, shown (the GUI's loop, before it waits).
        void presentCanvases();

        // Screens whose events the GUI's loop serves (`Screen.onEvent`): the fd that wakes it (-1:
        // none yet), the next event (a callback referenced and an Event, false if none), whether
        // any is still open or has events to call back.
        int  screenEventFd();
        bool nextScreenEvent(asIScriptFunction *&callback, void *&event);
        void screenEventRelease(void *event);
        bool screensServed();

        // At the script's end (gui.cpp): with a GUI window shown, the GUI's loop runs until it
        // closes (`app.run()`'s); then the connection is closed.
        void finishGui(asIScriptEngine *engine);
        // Before the engine is shut down: the callbacks and objects the GUI's runtime holds let go.
        void releaseGui();

        // At the script's end: what play() put in the background is heard to its end.
        void finishSound();

        // At the script's end: its open windows stay until the user closes them.
        void waitForWindows();

        // Reports a script exception (main.cpp): where, the calls that led there.
        void reportException(asIScriptContext *ctx);

        // The arguments scripts see as `args` (before registerCore), and their array released
        // (before the engine is shut down: an object of the script's types kept past the
        // module would keep it from being freed).
        void setArguments(int count, char **values);
        void releaseArguments();

        // Appends the value at @p address of type @p typeId as print() shows it (strings in a
        // container quoted, objects by their toString() if they have one).
        void formatValue(asIScriptEngine *engine, Text &out, const void *address, int typeId, bool quoted = false);
        // Numbers as concatenation and print() show them: float to 7 significant digits, double 15.
        void appendFloat(Text &out, float v);
        void appendDouble(Text &out, double v);

        // Raises a script exception in the running script (a formatted message).
        void raise(const char *format, ...) __attribute__((format(printf, 1, 2)));

        // The type of `array<string>` (registerString's, for split()).
        asITypeInfo *stringArrayType(asIScriptEngine *engine);

        // ---- The debugger (debug.cpp, devplan/editor.md §17).
        // Commands on descriptor @p fd, events and answers on @p reportFile (--report-fd's).
        // @return False if either is missing.
        bool startDebugger(int fd, FILE *reportFile);
        // True under --debug (the script is built with line cues).
        bool isDebugging();
        // The script's context debugged: its line and exception callbacks set, then the
        // editor's commands read until one starts it (its breakpoints come first).
        void debugContext(asIScriptContext *ctx);

        // Flushes stdout and restores the terminal (console.cpp): before exiting.
        void restoreTerminal();
        // The terminal back in line mode (after Console::key()): before reading a line.
        void cookedTerminal();

} // namespace angel

#endif
