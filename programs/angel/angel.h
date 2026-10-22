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

        // Flushes stdout and restores the terminal (console.cpp): before exiting.
        void restoreTerminal();
        // The terminal back in line mode (after Console::key()): before reading a line.
        void cookedTerminal();

} // namespace angel

#endif
