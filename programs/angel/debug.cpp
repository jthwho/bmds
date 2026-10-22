/**
 * @file      debug.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * angel's debugger (devplan/editor.md §17): `--debug FD` takes an editor's commands on FD, a
 * line each, and writes its events and answers on the report descriptor. A line callback (the
 * script built with line cues) stops at breakpoints, after steps and on a pause; while stopped,
 * the editor asks for the call stack and the variables, described without running script code.
 */

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "angel.h"
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"

namespace angel {

        namespace {

                constexpr int    MaxBreaks = 64;
                constexpr int    PollEvery = 256;   // lines between looks for commands while running
                constexpr size_t MaxValue = 80;     // a value's description, in bytes
                constexpr asUINT MaxChildren = 500; // a list's members shown
                constexpr int    MaxSections = 64;  // as main.cpp's includes

                // The function the top-level statements are compiled into (as_config.h).
                constexpr const char *TopLevel = "$top";

                enum class Mode : uint8_t {
                        Run,
                        Into,
                        Over,
                        Out,
                        Pause,
                };

                struct Break {
                                char *file; // canonical
                                int   line;
                };

                struct Section {
                                const char *name; // the engine's (its copy: the pointer is stable)
                                char       *canonical;
                };

                int         commandFd = -1;
                FILE       *events = nullptr;
                bool        detached = true;
                Break       breaks[MaxBreaks];
                int         breakCount = 0;
                Section     sections[MaxSections];
                int         sectionCount = 0;
                Mode        mode = Mode::Run;
                int         countdown = PollEvery;
                const char *stopSection = nullptr; // where the last stop was (for steps)
                int         stopLine = -1;
                asUINT      stopDepth = UINT_MAX;
                const char *lastSection = nullptr; // the last line seen (for breakpoints)
                int         lastLine = -1;
                asUINT      lastDepth = 0;
                bool        inStop = false;
                char        input[1024];
                size_t      inputLength = 0;

                char *canonicalOf(const char *path) {
                        char resolved[PATH_MAX];
                        return realpath(path, resolved) != nullptr ? strdup(resolved) : strdup(path);
                }

                // A section's canonical path, worked out once a section.
                const char *canonicalSection(const char *section) {
                        if (section == nullptr) return "";
                        for (int i = 0; i < sectionCount; ++i)
                                if (sections[i].name == section) return sections[i].canonical;
                        char *canonical = canonicalOf(section);
                        if (canonical == nullptr) return section;
                        if (sectionCount == MaxSections) {
                                free(sections[0].canonical);
                                memmove(sections, sections + 1, sizeof(Section) * (MaxSections - 1));
                                --sectionCount;
                        }
                        sections[sectionCount++] = Section{section, canonical};
                        return canonical;
                }

                bool isBreak(const char *section, int line) {
                        if (breakCount == 0) return false;
                        const char *file = nullptr;
                        for (int i = 0; i < breakCount; ++i) {
                                if (breaks[i].line != line) continue;
                                if (file == nullptr) file = canonicalSection(section);
                                if (strcmp(breaks[i].file, file) == 0) return true;
                        }
                        return false;
                }

                void setBreak(const char *file, int line, bool on) {
                        char *canonical = canonicalOf(file);
                        if (canonical == nullptr) return;
                        for (int i = 0; i < breakCount; ++i) {
                                if (breaks[i].line != line || strcmp(breaks[i].file, canonical) != 0) continue;
                                if (!on) {
                                        free(breaks[i].file);
                                        breaks[i] = breaks[--breakCount];
                                }
                                free(canonical);
                                return;
                        }
                        if (on && breakCount < MaxBreaks)
                                breaks[breakCount++] = Break{canonical, line};
                        else
                                free(canonical);
                        return;
                }

                // The editor is gone: the script runs on, undebugged.
                void detach() {
                        detached = true;
                        if (commandFd >= 0) close(commandFd);
                        commandFd = -1;
                        mode = Mode::Run;
                        for (int i = 0; i < breakCount; ++i) free(breaks[i].file);
                        breakCount = 0;
                        return;
                }

                // A command's line into @p out: 1, 0 (none yet, unless @p wait), -1 (the end).
                int readCommand(bool wait, char *out, size_t max) {
                        for (;;) {
                                char *nl = static_cast<char *>(memchr(input, '\n', inputLength));
                                if (nl != nullptr) {
                                        const size_t length = size_t(nl - input);
                                        const size_t keep = length < max ? length : max - 1;
                                        memcpy(out, input, keep);
                                        out[keep] = 0;
                                        if (keep > 0 && out[keep - 1] == '\r') out[keep - 1] = 0;
                                        inputLength -= length + 1;
                                        memmove(input, nl + 1, inputLength);
                                        return 1;
                                }
                                if (inputLength == sizeof(input)) inputLength = 0; // a line too long: dropped
                                if (!wait) {
                                        struct pollfd p = {commandFd, POLLIN, 0};
                                        if (poll(&p, 1, 0) <= 0) return 0;
                                }
                                const ssize_t n = read(commandFd, input + inputLength, sizeof(input) - inputLength);
                                if (n < 0 && errno == EINTR) continue;
                                if (n <= 0) return -1;
                                inputLength += size_t(n);
                        }
                }

                // ---- Describing values (no script code runs: toString() isn't called)

                struct Value {
                                void *address = nullptr; // an object's own; a handle's variable
                                int   typeId = 0;
                                bool  set = true; // false: not initialized yet
                };

                asIScriptEngine *engineOf(asIScriptContext *ctx) {
                        return ctx->GetEngine();
                }

                // The object a value is (a handle followed): its address and type, or null.
                void *objectOf(const Value &v, int &typeId) {
                        typeId = v.typeId & ~asTYPEID_OBJHANDLE;
                        if (!v.set || v.address == nullptr) return nullptr;
                        if ((v.typeId & asTYPEID_OBJHANDLE) != 0) return *static_cast<void **>(v.address);
                        return v.address;
                }

                bool isArray(asITypeInfo *type) {
                        return type != nullptr && (type->GetFlags() & asOBJ_TEMPLATE) != 0 &&
                               strcmp(type->GetName(), "array") == 0;
                }

                // The members or elements of @p v: @p each(name, Value) for each, at most MaxChildren.
                template <typename Fn> asUINT eachChild(asIScriptEngine *engine, const Value &v, Fn each) {
                        int   typeId = 0;
                        void *object = objectOf(v, typeId);
                        if (object == nullptr || (typeId & asTYPEID_MASK_OBJECT) == 0) return 0;
                        asITypeInfo *type = engine->GetTypeInfoById(typeId);
                        if (type == nullptr) return 0;
                        char   name[24];
                        asUINT n = 0;
                        if (isArray(type)) {
                                CScriptArray *array = static_cast<CScriptArray *>(object);
                                const int     element = array->GetElementTypeId();
                                for (asUINT i = 0; i < array->GetSize() && n < MaxChildren; ++i, ++n) {
                                        snprintf(name, sizeof(name), "[%u]", unsigned(i));
                                        if (!each(name, Value{array->At(i), element, true})) break;
                                }
                                return n;
                        }
                        if ((type->GetFlags() & asOBJ_SCRIPT_OBJECT) != 0) {
                                asIScriptObject *o = static_cast<asIScriptObject *>(object);
                                for (asUINT i = 0; i < o->GetPropertyCount() && n < MaxChildren; ++i, ++n)
                                        if (!each(o->GetPropertyName(i),
                                                  Value{o->GetAddressOfProperty(i), o->GetPropertyTypeId(i), true}))
                                                break;
                                return n;
                        }
                        for (asUINT i = 0; i < type->GetPropertyCount() && n < MaxChildren; ++i) {
                                const char *pname = nullptr;
                                int         ptype = 0, offset = 0;
                                bool        isPrivate = false, isProtected = false, isReference = false;
                                if (type->GetProperty(i, &pname, &ptype, &isPrivate, &isProtected, &offset,
                                                      &isReference) < 0 ||
                                    isPrivate)
                                        continue;
                                void *address = static_cast<char *>(object) + offset;
                                if (isReference) address = *static_cast<void **>(address);
                                ++n;
                                if (!each(pname, Value{address, ptype, true})) break;
                        }
                        return n;
                }

                asUINT childCount(asIScriptEngine *engine, const Value &v) {
                        int   typeId = 0;
                        void *object = objectOf(v, typeId);
                        if (object == nullptr || (typeId & asTYPEID_MASK_OBJECT) == 0) return 0;
                        asITypeInfo *type = engine->GetTypeInfoById(typeId);
                        if (type == nullptr) return 0;
                        if (isArray(type)) {
                                const asUINT size = static_cast<CScriptArray *>(object)->GetSize();
                                return size < MaxChildren ? size : MaxChildren;
                        }
                        return eachChild(engine, v, [](const char *, const Value &) { return true; });
                }

                void describe(asIScriptEngine *engine, Text &out, const Value &v, int depth);

                void describeString(Text &out, const String &s) {
                        out.append("\"");
                        for (uint32_t i = 0; i < s.length() && out.length() < MaxValue * 2; ++i) {
                                const char c = s.data()[i];
                                if (c == '\n')
                                        out.append("\\n");
                                else if (c == '\t')
                                        out.append("\\t");
                                else if (c == '"' || c == '\\') {
                                        out.append("\\");
                                        out.append(&c, 1);
                                } else if (uint8_t(c) < 0x20)
                                        out.appendf("\\x%02x", unsigned(uint8_t(c)));
                                else
                                        out.append(&c, 1);
                        }
                        out.append("\"");
                        return;
                }

                void describe(asIScriptEngine *engine, Text &out, const Value &v, int depth) {
                        if (!v.set) {
                                out.append("(not set yet)");
                                return;
                        }
                        int   typeId = 0;
                        void *object = objectOf(v, typeId);
                        if (object == nullptr) {
                                out.append("null");
                                return;
                        }
                        if ((typeId & asTYPEID_MASK_OBJECT) == 0) {
                                // A primitive or an enum: as print() shows it (no script code for these).
                                formatValue(engine, out, object, typeId);
                                return;
                        }
                        asITypeInfo *type = engine->GetTypeInfoById(typeId);
                        if (type == nullptr) {
                                out.append("?");
                                return;
                        }
                        if (typeId == engine->GetTypeIdByDecl("string")) {
                                describeString(out, *static_cast<const String *>(object));
                                return;
                        }
                        const bool array = isArray(type);
                        const bool script = (type->GetFlags() & asOBJ_SCRIPT_OBJECT) != 0;
                        if (!array && !script) {
                                out.appendf("<%s>", type->GetName());
                                return;
                        }
                        if (depth > 0) {
                                out.append(array ? "[…]" : "{…}");
                                return;
                        }
                        out.append(array ? "[" : "{");
                        bool first = true;
                        eachChild(engine, v, [&](const char *name, const Value &child) {
                                if (out.length() > MaxValue) {
                                        out.append(", …");
                                        return false;
                                }
                                if (!first) out.append(", ");
                                first = false;
                                if (!array) out.appendf("%s: ", name);
                                describe(engine, out, child, depth + 1);
                                return true;
                        });
                        out.append(array ? "]" : "}");
                        return;
                }

                // A field of a tab-separated line: tabs and newlines would split it.
                void field(const char *text, size_t length) {
                        for (size_t i = 0; i < length; ++i) {
                                const char c = text[i];
                                fputc(c == '\t' || c == '\n' || c == '\r' ? ' ' : c, events);
                        }
                        return;
                }
                void field(const char *text) {
                        field(text != nullptr ? text : "", text != nullptr ? strlen(text) : 0);
                        return;
                }

                // `var NAME TYPE VALUE CHILDREN`.
                void writeVar(asIScriptEngine *engine, const char *name, const Value &v) {
                        Text value;
                        describe(engine, value, v, 0);
                        size_t length = value.length();
                        if (length > MaxValue) {
                                length = MaxValue;
                                while (length > 0 && (uint8_t(value.data()[length]) & 0xc0) == 0x80) --length;
                        }
                        fputs("var\t", events);
                        field(name);
                        fputc('\t', events);
                        field(engine->GetTypeDeclaration(v.typeId, true));
                        fputc('\t', events);
                        field(value.data(), length);
                        if (length < value.length()) fputs("…", events);
                        fprintf(events, "\t%u\n", unsigned(childCount(engine, v)));
                        return;
                }

                // ---- The variables of a frame

                asIScriptModule *moduleOf(asIScriptContext *ctx) {
                        for (asUINT level = 0; level < ctx->GetCallstackSize(); ++level) {
                                asIScriptFunction *f = ctx->GetFunction(level);
                                if (f != nullptr && f->GetModule() != nullptr) return f->GetModule();
                        }
                        return nullptr;
                }

                // Each variable in scope at @p level (or the globals, @p level < 0): @p each(name, Value).
                template <typename Fn> void eachVariable(asIScriptContext *ctx, int level, Fn each) {
                        if (level >= 0) {
                                const asUINT at = asUINT(level);
                                if (ctx->GetFunction(at) == nullptr) return;
                                const int count = ctx->GetVarCount(at);
                                for (int i = 0; i < count; ++i) {
                                        const char *name = nullptr;
                                        int         typeId = 0;
                                        if (ctx->GetVar(asUINT(i), at, &name, &typeId) < 0 || name == nullptr ||
                                            name[0] == 0 || !ctx->IsVarInScope(asUINT(i), at))
                                                continue;
                                        void *address = ctx->GetAddressOfVar(asUINT(i), at);
                                        if (!each(name, Value{address, typeId, address != nullptr})) return;
                                }
                                void *self = ctx->GetThisPointer(at);
                                if (self != nullptr && !each("this", Value{self, ctx->GetThisTypeId(at), true})) return;
                                return;
                        }
                        asIScriptModule *module = moduleOf(ctx);
                        if (module == nullptr) return;
                        for (asUINT i = 0; i < module->GetGlobalVarCount(); ++i) {
                                const char *name = nullptr;
                                int         typeId = 0;
                                if (module->GetGlobalVar(i, &name, nullptr, &typeId) < 0) continue;
                                void *address = module->GetAddressOfGlobalVar(i);
                                if (!each(name, Value{address, typeId, address != nullptr})) return;
                        }
                        return;
                }

                // @p path's value: a name (a local, `this`, this's member, a global), then `.member`
                // and `[index]` parts. @return An error message, or null.
                const char *resolve(asIScriptContext *ctx, int level, const char *path, Value &out) {
                        asIScriptEngine *engine = engineOf(ctx);
                        const char      *p = path;
                        size_t           n = 0;
                        while (p[n] == '_' || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') ||
                               (n > 0 && p[n] >= '0' && p[n] <= '9'))
                                ++n;
                        if (n == 0) return "a name first";
                        bool found = false;
                        // The innermost declaration wins: the last in scope.
                        const auto match = [&](const char *name, const Value &v) {
                                if (strlen(name) == n && strncmp(name, p, n) == 0) {
                                        out = v;
                                        found = true;
                                }
                                return true;
                        };
                        if (level >= 0) {
                                eachVariable(ctx, level, match);
                                if (!found) {
                                        void *self = ctx->GetThisPointer(asUINT(level));
                                        if (self != nullptr)
                                                eachChild(engine, Value{self, ctx->GetThisTypeId(asUINT(level)), true},
                                                          match);
                                }
                        }
                        if (!found) eachVariable(ctx, -1, match);
                        if (!found) return "no such variable here";
                        p += n;
                        while (*p != 0) {
                                if (*p == '.') {
                                        ++p;
                                        n = 0;
                                        while (p[n] != 0 && p[n] != '.' && p[n] != '[') ++n;
                                        if (n == 0) return "a member's name after '.'";
                                        found = false;
                                        const Value of = out;
                                        eachChild(engine, of, [&](const char *name, const Value &v) {
                                                if (strlen(name) != n || strncmp(name, p, n) != 0) return true;
                                                out = v;
                                                found = true;
                                                return false;
                                        });
                                        if (!found) return "no such member";
                                        p += n;
                                } else if (*p == '[') {
                                        char        *end = nullptr;
                                        const long   index = strtol(p + 1, &end, 10);
                                        int          typeId = 0;
                                        void        *object = objectOf(out, typeId);
                                        asITypeInfo *type =
                                                object != nullptr ? engine->GetTypeInfoById(typeId) : nullptr;
                                        if (end == p + 1 || *end != ']') return "an index in [ ]";
                                        if (!isArray(type)) return "not an array";
                                        CScriptArray *array = static_cast<CScriptArray *>(object);
                                        if (index < 0 || asUINT(index) >= array->GetSize())
                                                return "index out of bounds";
                                        out = Value{array->At(asUINT(index)), array->GetElementTypeId(), true};
                                        p = end + 1;
                                } else {
                                        return "a path is a name, then .member and [index] parts";
                                }
                                if (!out.set) return "not set yet";
                        }
                        return nullptr;
                }

                // ---- Answers (while stopped)

                void writeStack(asIScriptContext *ctx) {
                        for (asUINT level = 0; level < ctx->GetCallstackSize(); ++level) {
                                asIScriptFunction *f = ctx->GetFunction(level);
                                if (f == nullptr) continue; // a native call's place in the stack
                                const char *section = nullptr;
                                const int   line = ctx->GetLineNumber(level, nullptr, &section);
                                fprintf(events, "frame\t%u\t", unsigned(level));
                                field(strcmp(f->GetName(), TopLevel) == 0 ? "the top level"
                                                                          : f->GetDeclaration(true, true, false));
                                fputc('\t', events);
                                field(section);
                                fprintf(events, "\t%d\n", line);
                        }
                        return;
                }

                int levelOf(const char *text) {
                        if (strcmp(text, "g") == 0) return -1;
                        return int(strtol(text, nullptr, 10));
                }

                void writeVars(asIScriptContext *ctx, const char *levelText, const char *path) {
                        asIScriptEngine *engine = engineOf(ctx);
                        const int        level = levelOf(levelText);
                        if (level >= int(ctx->GetCallstackSize())) {
                                fputs("error\tno such frame\n", events);
                                return;
                        }
                        if (path[0] == 0) {
                                eachVariable(ctx, level, [&](const char *name, const Value &v) {
                                        writeVar(engine, name, v);
                                        return true;
                                });
                                return;
                        }
                        Value       v;
                        const char *error = resolve(ctx, level, path, v);
                        if (error != nullptr) {
                                fprintf(events, "error\t%s\n", error);
                                return;
                        }
                        eachChild(engine, v, [&](const char *name, const Value &child) {
                                writeVar(engine, name, child);
                                return true;
                        });
                        return;
                }

                void writeValue(asIScriptContext *ctx, const char *levelText, const char *path) {
                        const int level = levelOf(levelText);
                        if (level >= int(ctx->GetCallstackSize())) {
                                fputs("error\tno such frame\n", events);
                                return;
                        }
                        Value       v;
                        const char *error = resolve(ctx, level, path, v);
                        if (error != nullptr)
                                fprintf(events, "error\t%s\n", error);
                        else
                                writeVar(engineOf(ctx), path, v);
                        return;
                }

                // Where the context is now: a step's start.
                void markStop(asIScriptContext *ctx) {
                        stopLine = ctx->GetLineNumber(0, nullptr, &stopSection);
                        stopDepth = ctx->GetCallstackSize();
                        lastSection = stopSection;
                        lastLine = stopLine;
                        lastDepth = stopDepth;
                        return;
                }

                // One command. @return True if it resumes the script (only while @p stopped).
                bool handle(char *line, asIScriptContext *ctx, bool stopped) {
                        char *f[4] = {line, nullptr, nullptr, nullptr};
                        for (int i = 1; i < 4; ++i) {
                                char *tab = f[i - 1] != nullptr ? strchr(f[i - 1], '\t') : nullptr;
                                if (tab == nullptr) break;
                                *tab = 0;
                                f[i] = tab + 1;
                        }
                        const char *command = f[0];
                        if (strcmp(command, "break") == 0 || strcmp(command, "clear") == 0) {
                                if (f[1] != nullptr && f[2] != nullptr)
                                        setBreak(f[1], int(strtol(f[2], nullptr, 10)), command[0] == 'b');
                                return false;
                        }
                        if (strcmp(command, "pause") == 0) {
                                if (!stopped) mode = Mode::Pause;
                                return false;
                        }
                        const bool resume = strcmp(command, "go") == 0 || strcmp(command, "step") == 0 ||
                                            strcmp(command, "next") == 0 || strcmp(command, "out") == 0;
                        if (resume) {
                                if (!stopped) return false;
                                mode = command[0] == 'g'   ? Mode::Run
                                       : command[0] == 'o' ? Mode::Out
                                       : command[0] == 'n' ? Mode::Over
                                                           : Mode::Into;
                                if (ctx != nullptr)
                                        markStop(ctx);
                                else {
                                        stopSection = nullptr; // before the first line: any line stops a step
                                        stopLine = -1;
                                        stopDepth = UINT_MAX;
                                }
                                return true;
                        }
                        // The questions: answered while stopped, each ended by `end`.
                        if (!stopped || ctx == nullptr)
                                fputs("error\tnot stopped\n", events);
                        else if (strcmp(command, "stack") == 0)
                                writeStack(ctx);
                        else if (strcmp(command, "vars") == 0 && f[1] != nullptr)
                                writeVars(ctx, f[1], f[2] != nullptr ? f[2] : "");
                        else if (strcmp(command, "value") == 0 && f[1] != nullptr && f[2] != nullptr)
                                writeValue(ctx, f[1], f[2]);
                        else
                                fputs("error\tunknown command\n", events);
                        fputs("end\n", events);
                        fflush(events);
                        return false;
                }

                // Stopped: said so, then the editor's commands until one resumes.
                void stop(asIScriptContext *ctx, const char *reason, const char *message) {
                        inStop = true;
                        const char *section = nullptr;
                        const int   line = ctx != nullptr ? ctx->GetLineNumber(0, nullptr, &section) : 0;
                        fprintf(events, "stopped\t%s\t", reason);
                        field(section);
                        fprintf(events, "\t%d", line);
                        if (message != nullptr) {
                                fputc('\t', events);
                                field(message);
                        }
                        fputc('\n', events);
                        fflush(events);
                        fflush(stdout);
                        for (;;) {
                                char      command[1024];
                                const int r = readCommand(true, command, sizeof(command));
                                if (r < 0) {
                                        detach();
                                        break;
                                }
                                if (handle(command, ctx, true)) break;
                        }
                        inStop = false;
                        return;
                }

                void lineCallback(asIScriptContext *ctx, void *) {
                        if (detached || inStop) return;
                        if (--countdown <= 0) {
                                countdown = PollEvery;
                                char command[1024];
                                for (;;) {
                                        const int r = readCommand(false, command, sizeof(command));
                                        if (r < 0) {
                                                detach();
                                                return;
                                        }
                                        if (r == 0) break;
                                        (void)handle(command, ctx, false);
                                }
                        }
                        if (mode == Mode::Run && breakCount == 0) return;
                        const char  *section = nullptr;
                        const int    line = ctx->GetLineNumber(0, nullptr, &section);
                        const asUINT depth = ctx->GetCallstackSize();
                        const bool   moved = line != lastLine || section != lastSection || depth != lastDepth;
                        lastSection = section;
                        lastLine = line;
                        lastDepth = depth;
                        const bool  elsewhere = line != stopLine || section != stopSection;
                        const char *reason = nullptr;
                        switch (mode) {
                                case Mode::Pause: reason = "pause"; break;
                                case Mode::Into:
                                        if (depth != stopDepth || elsewhere) reason = "step";
                                        break;
                                case Mode::Over:
                                        if (depth < stopDepth || (depth == stopDepth && elsewhere)) reason = "step";
                                        break;
                                case Mode::Out:
                                        if (depth < stopDepth) reason = "step";
                                        break;
                                case Mode::Run: break;
                        }
                        if (reason == nullptr && moved && isBreak(section, line)) reason = "break";
                        if (reason != nullptr) stop(ctx, reason, nullptr);
                        return;
                }

                // An exception the script won't catch: stopped where it was raised, its variables
                // still there to see; it unwinds once resumed.
                void exceptionCallback(asIScriptContext *ctx, void *) {
                        if (detached || inStop || ctx->WillExceptionBeCaught()) return;
                        stop(ctx, "exception", ctx->GetExceptionString());
                        return;
                }

        } // namespace

        bool startDebugger(int fd, FILE *reportFile) {
                if (fd < 0 || reportFile == nullptr) return false;
                commandFd = fd;
                events = reportFile;
                detached = false;
                return true;
        }

        bool isDebugging() {
                return events != nullptr;
        }

        void debugContext(asIScriptContext *ctx) {
                if (detached) return;
                ctx->SetLineCallback(asFUNCTION(lineCallback), nullptr, asCALL_CDECL);
                ctx->SetExceptionCallback(asFUNCTION(exceptionCallback), nullptr, asCALL_CDECL);
                // Nothing runs until the editor says how: its breakpoints come first.
                for (;;) {
                        char      command[1024];
                        const int r = readCommand(true, command, sizeof(command));
                        if (r < 0) {
                                detach();
                                return;
                        }
                        if (handle(command, nullptr, true)) return;
                }
        }

} // namespace angel
