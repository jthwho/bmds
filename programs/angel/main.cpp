/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * angel: runs AngelScript scripts with the whole system as their API (devplan/scripting.md).
 * A script runs from top to bottom (statements outside functions, AS_TOP_LEVEL_STATEMENTS),
 * or, made only of declarations, through its main(). Decimal literals are float
 * (AS_FLOAT_LITERALS: 1.5d for a double). The first line may be `#!`; `#include "file"` adds
 * another file (its declarations only). Errors are reported as compilers do,
 * `file:line:column: error: message`.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "angel.h"

#ifndef ANGEL_VERSION
#define ANGEL_VERSION "1"
#endif

namespace {

        using angel::Text;

        constexpr int ExitFailure = 1;
        constexpr int ExitUsage = 2;
        constexpr int MaxIncludes = 64;

        // The function the top-level statements are compiled into (as_config.h).
        constexpr const char *TopLevel = "$top";

        void usage() {
                fprintf(stderr, "usage: angel [-c] SCRIPT [ARGUMENT...]\n"
                                "       angel --api | --api-help | --version\n"
                                "  -c          check the script: report its errors, run nothing\n"
                                "  --api       the script API's declarations, one a line\n"
                                "  --api-help  the same, each with its help\n");
                return;
        }

        // ---- Diagnostics

        void message(const asSMessageInfo *msg, void *) {
                const char *kind = "error";
                if (msg->type == asMSGTYPE_WARNING)
                        kind = "warning";
                else if (msg->type == asMSGTYPE_INFORMATION) {
                        // "Compiling void f()" heads every function's messages: noise here.
                        if (strncmp(msg->message, "Compiling ", 10) == 0) return;
                        kind = "note";
                }
                fprintf(stderr, "%s:%d:%d: %s: %s\n", msg->section, msg->row, msg->col, kind, msg->message);
                return;
        }

        // A function as the call stack names it (the top level isn't a function to the writer).
        void appendFunction(Text &out, asIScriptFunction *f) {
                if (f == nullptr)
                        out.append("?");
                else if (strcmp(f->GetName(), TopLevel) == 0)
                        out.append("the top level");
                else
                        out.append(f->GetDeclaration(true, true, false));
                return;
        }

        // After a null pointer: the globals still waiting for their declaration to be reached
        // (initialized where they stand, among the statements: until then, objects have none).
        void noteUnreachedGlobals(asIScriptContext *ctx) {
                asIScriptFunction *f = ctx->GetExceptionFunction();
                asIScriptModule   *module = f != nullptr ? f->GetModule() : nullptr;
                if (module == nullptr || strcmp(ctx->GetExceptionString(), "Null pointer access") != 0) return;
                for (asUINT i = 0; i < module->GetGlobalVarCount(); ++i) {
                        const char *name = nullptr;
                        int         typeId = 0;
                        if (module->GetGlobalVar(i, &name, nullptr, &typeId) < 0) continue;
                        if ((typeId & asTYPEID_MASK_OBJECT) == 0 || (typeId & asTYPEID_OBJHANDLE) != 0) continue;
                        if (module->GetAddressOfGlobalVar(i) != nullptr) continue;
                        fprintf(stderr,
                                "  note: '%s' has no value yet: a variable declared among the statements gets "
                                "it when the script reaches its declaration\n",
                                name);
                }
                return;
        }

        void reportExceptionHere(asIScriptContext *ctx) {
                int         column = 0;
                const char *section = nullptr;
                const int   line = ctx->GetExceptionLineNumber(&column, &section);
                fprintf(stderr, "%s:%d:%d: error: %s\n", section != nullptr ? section : "?", line, column,
                        ctx->GetExceptionString());
                const asUINT frames = ctx->GetCallstackSize();
                for (asUINT n = 0; n < frames && frames > 1; ++n) {
                        Text where;
                        appendFunction(where, ctx->GetFunction(n));
                        int         col = 0;
                        const char *file = nullptr;
                        const int   row = ctx->GetLineNumber(n, &col, &file);
                        if (file != nullptr)
                                fprintf(stderr, "  %s %s, %s:%d\n", n == 0 ? "in" : "called from", where.data(), file,
                                        row);
                        else
                                fprintf(stderr, "  %s %s\n", n == 0 ? "in" : "called from", where.data());
                }
                noteUnreachedGlobals(ctx);
                return;
        }

        // ---- Loading scripts

        struct Source {
                        char  *path;      // as written (for messages)
                        char  *canonical; // to include each file once
                        char  *code;
                        size_t length;
        };

        Source sources[MaxIncludes];
        int    sourceCount = 0;
        int    loadErrors = 0;

        char *readFile(const char *path, size_t &length) {
                FILE *f = fopen(path, "rb");
                if (f == nullptr) return nullptr;
                Text text;
                char chunk[4096];
                for (;;) {
                        const size_t n = fread(chunk, 1, sizeof(chunk), f);
                        if (n == 0) break;
                        text.append(chunk, n);
                }
                const bool failed = ferror(f) != 0;
                fclose(f);
                if (failed) return nullptr;
                length = text.length();
                char *code = text.release();
                return code != nullptr ? code : strdup("");
        }

        // @p name relative to the directory of @p from (unless absolute).
        char *relativeTo(const char *from, const char *name, size_t nameLength) {
                Text path;
                if (name[0] != '/') {
                        const char *slash = strrchr(from, '/');
                        if (slash != nullptr) path.append(from, size_t(slash - from) + 1);
                }
                path.append(name, nameLength);
                return path.release();
        }

        void loadError(const char *path, int line, const char *format, const char *detail) {
                fprintf(stderr, "%s:%d:1: error: ", path, line);
                fprintf(stderr, format, detail);
                fputc('\n', stderr);
                ++loadErrors;
                return;
        }

        int load(const char *path, const char *includedFrom, int includedAt);

        // Blanks @p length bytes at @p p, keeping the newlines (so the lines stay where they are).
        void blank(char *p, size_t length) {
                for (size_t i = 0; i < length; ++i)
                        if (p[i] != '\n') p[i] = ' ';
                return;
        }

        // The directives of a source: a first line `#!` and `#include "file"` lines, blanked
        // out once read (comments, strings and heredocs are skipped over to find line starts).
        void preprocess(int index) {
                char        *code = sources[index].code;
                const size_t length = sources[index].length;
                const char  *path = sources[index].path;
                bool         blockComment = false, heredoc = false;
                int          line = 1;
                size_t       i = 0;
                while (i < length) {
                        // At a line's start.
                        size_t j = i;
                        if (!blockComment && !heredoc) {
                                while (j < length && (code[j] == ' ' || code[j] == '\t')) ++j;
                                if (j < length && code[j] == '#') {
                                        size_t end = j;
                                        while (end < length && code[end] != '\n') ++end;
                                        if (line == 1 && j == 0 && end > 1 && code[1] == '!') {
                                                blank(code, end);
                                        } else if (end - j >= 8 && strncmp(code + j, "#include", 8) == 0) {
                                                const char *q1 = end - j > 8 ? static_cast<const char *>(memchr(
                                                                                       code + j + 8, '"', end - j - 8))
                                                                             : nullptr;
                                                const char *q2 =
                                                        q1 != nullptr
                                                                ? static_cast<const char *>(memchr(
                                                                          q1 + 1, '"', size_t(code + end - q1 - 1)))
                                                                : nullptr;
                                                if (q2 == nullptr || q2 == q1 + 1) {
                                                        loadError(path, line, "%s",
                                                                  "#include needs a file name in quotes: #include "
                                                                  "\"file.as\"");
                                                } else {
                                                        char *name = relativeTo(path, q1 + 1, size_t(q2 - q1 - 1));
                                                        blank(code + j, end - j);
                                                        if (name != nullptr) {
                                                                load(name, path, line);
                                                                free(name);
                                                        }
                                                        code = sources[index].code; // unchanged: sources don't move
                                                }
                                                blank(code + j, end - j);
                                        } else {
                                                loadError(path, line, "%s",
                                                          "unknown directive (there is only #include \"file\")");
                                        }
                                        i = end;
                                        continue;
                                }
                        }
                        // The rest of the line: comments and strings.
                        for (; j < length && code[j] != '\n'; ++j) {
                                if (blockComment) {
                                        if (code[j] == '*' && j + 1 < length && code[j + 1] == '/') {
                                                blockComment = false;
                                                ++j;
                                        }
                                } else if (heredoc) {
                                        if (j + 2 < length && strncmp(code + j, "\"\"\"", 3) == 0) {
                                                heredoc = false;
                                                j += 2;
                                        }
                                } else if (code[j] == '/' && j + 1 < length && code[j + 1] == '/') {
                                        while (j < length && code[j] != '\n') ++j;
                                        break;
                                } else if (code[j] == '/' && j + 1 < length && code[j + 1] == '*') {
                                        blockComment = true;
                                        ++j;
                                } else if (j + 2 < length && strncmp(code + j, "\"\"\"", 3) == 0) {
                                        heredoc = true;
                                        j += 2;
                                } else if (code[j] == '"' || code[j] == '\'') {
                                        const char quote = code[j];
                                        for (++j; j < length && code[j] != quote && code[j] != '\n'; ++j)
                                                if (code[j] == '\\' && j + 1 < length && code[j + 1] != '\n') ++j;
                                        if (j >= length || code[j] == '\n') break;
                                }
                        }
                        if (j < length && code[j] == '\n') {
                                ++line;
                                ++j;
                        }
                        i = j;
                }
                return;
        }

        // Loads @p path (once) and the files it includes; returns its index, or -1.
        int load(const char *path, const char *includedFrom, int includedAt) {
                char  resolved[PATH_MAX];
                char *canonical = realpath(path, resolved) != nullptr ? strdup(resolved) : strdup(path);
                for (int i = 0; i < sourceCount; ++i)
                        if (strcmp(sources[i].canonical, canonical) == 0) {
                                free(canonical);
                                return i;
                        }
                if (sourceCount == MaxIncludes) {
                        loadError(includedFrom, includedAt, "too many files included (at most %s)", "64");
                        free(canonical);
                        return -1;
                }
                size_t length = 0;
                char  *code = readFile(path, length);
                if (code == nullptr) {
                        if (includedFrom != nullptr) {
                                Text text;
                                text.appendf("%s: %s", path, strerror(errno));
                                loadError(includedFrom, includedAt, "can't include %s", text.data());
                        } else
                                fprintf(stderr, "angel: %s: %s\n", path, strerror(errno));
                        free(canonical);
                        ++loadErrors;
                        return -1;
                }
                const int index = sourceCount++;
                sources[index] = Source{strdup(path), canonical, code, length};
                preprocess(index);
                return index;
        }

        // ---- The engine

        asIScriptEngine *makeEngine() {
                asIScriptEngine *engine = asCreateScriptEngine();
                if (engine == nullptr) {
                        fprintf(stderr, "angel: no script engine (out of memory?)\n");
                        exit(70);
                }
                engine->SetMessageCallback(asFUNCTION(message), nullptr, asCALL_CDECL);
                // The language's settings, every one explicit: they are part of the scripts' ABI
                // (devplan/scripting.md §4), not left to the library's defaults.
                struct Setting {
                                asEEngineProp property;
                                asPWORD       value;
                };
                static constexpr Setting settings[] = {
                        {asEP_ALLOW_UNSAFE_REFERENCES, 0},
                        {asEP_OPTIMIZE_BYTECODE, 1},
                        {asEP_COPY_SCRIPT_SECTIONS, 1},
                        {asEP_MAX_STACK_SIZE, 1024 * 1024},
                        {asEP_USE_CHARACTER_LITERALS, 1},
                        {asEP_ALLOW_MULTILINE_STRINGS, 1},
                        {asEP_ALLOW_IMPLICIT_HANDLE_TYPES, 0},
                        {asEP_BUILD_WITHOUT_LINE_CUES, 1},
                        {asEP_INIT_GLOBAL_VARS_AFTER_BUILD, 1},
                        {asEP_REQUIRE_ENUM_SCOPE, 0},
                        {asEP_SCRIPT_SCANNER, 1},
                        {asEP_INCLUDE_JIT_INSTRUCTIONS, 0},
                        {asEP_STRING_ENCODING, 0},
                        {asEP_PROPERTY_ACCESSOR_MODE, 3},
                        {asEP_EXPAND_DEF_ARRAY_TO_TMPL, 0},
                        {asEP_AUTO_GARBAGE_COLLECT, 1},
                        {asEP_DISALLOW_GLOBAL_VARS, 0},
                        {asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT, 0},
                        {asEP_COMPILER_WARNINGS, 1},
                        {asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE, 0},
                        {asEP_ALTER_SYNTAX_NAMED_ARGS, 0},
                        {asEP_DISABLE_INTEGER_DIVISION, 0},
                        {asEP_DISALLOW_EMPTY_LIST_ELEMENTS, 1},
                        {asEP_PRIVATE_PROP_AS_PROTECTED, 0},
                        {asEP_ALLOW_UNICODE_IDENTIFIERS, 0},
                        {asEP_HEREDOC_TRIM_MODE, 1},
                        {asEP_MAX_NESTED_CALLS, 100},
                        {asEP_GENERIC_CALL_MODE, 1},
                        {asEP_INIT_STACK_SIZE, 4096},
                        {asEP_INIT_CALL_STACK_SIZE, 10},
                        {asEP_MAX_CALL_STACK_SIZE, 50000},
                        {asEP_IGNORE_DUPLICATE_SHARED_INTF, 0},
                        {asEP_NO_DEBUG_OUTPUT, 1},
                        {asEP_DISABLE_SCRIPT_CLASS_GC, 0},
                        {asEP_ALWAYS_IMPL_DEFAULT_COPY, 0},
                        {asEP_ALWAYS_IMPL_DEFAULT_COPY_CONSTRUCT, 0},
                        {asEP_MEMBER_INIT_MODE, 1},
                        {asEP_BOOL_CONVERSION_MODE, 0},
                        {asEP_FOREACH_SUPPORT, 1},
                };
                for (const Setting &s : settings)
                        if (engine->SetEngineProperty(s.property, s.value) < 0) {
                                fprintf(stderr, "angel: engine property %d refused\n", int(s.property));
                                exit(70);
                        }
                angel::registerString(engine);
                angel::registerCore(engine);
                angel::registerConsole(engine);
                angel::registerFiles(engine);
                angel::registerSystem(engine);
                angel::registerGraphics(engine);
                return engine;
        }

        // ---- Running

        int run(asIScriptEngine *engine, asIScriptModule *module) {
                asIScriptFunction *top = module->GetFunctionByName(TopLevel);
                asIScriptFunction *main = module->GetFunctionByName("main");
                if (top != nullptr && main != nullptr) {
                        const char *section = nullptr;
                        int         line = 0, column = 0;
                        main->GetDeclaredAt(&section, &line, &column);
                        fprintf(stderr,
                                "%s:%d:%d: error: a script with statements outside functions runs them; it can't "
                                "have a main() as well\n",
                                section != nullptr ? section : sources[0].path, line, column);
                        return ExitFailure;
                }
                asIScriptFunction *entry = top != nullptr ? top : main;
                if (entry == nullptr) {
                        fprintf(stderr,
                                "%s:1:1: error: nothing to run: no statements outside functions, and no main()\n",
                                sources[0].path);
                        return ExitFailure;
                }
                if (entry->GetParamCount() != 0 ||
                    (entry->GetReturnTypeId() != asTYPEID_VOID && entry->GetReturnTypeId() != asTYPEID_INT32)) {
                        const char *section = nullptr;
                        int         line = 0, column = 0;
                        entry->GetDeclaredAt(&section, &line, &column);
                        fprintf(stderr, "%s:%d:%d: error: main() must be 'void main()' or 'int main()'\n",
                                section != nullptr ? section : sources[0].path, line, column);
                        return ExitFailure;
                }
                asIScriptContext *ctx = engine->CreateContext();
                if (ctx == nullptr || ctx->Prepare(entry) < 0) {
                        fprintf(stderr, "angel: can't start the script\n");
                        return 70;
                }
                const int r = ctx->Execute();
                int       status = 0;
                if (r == asEXECUTION_FINISHED) {
                        if (entry->GetReturnTypeId() == asTYPEID_INT32) status = int(ctx->GetReturnDWord());
                } else if (r == asEXECUTION_EXCEPTION) {
                        fflush(stdout);
                        reportExceptionHere(ctx);
                        status = ExitFailure;
                } else {
                        fprintf(stderr, "angel: the script stopped (%d)\n", r);
                        status = ExitFailure;
                }
                ctx->Release();
                // Windows stay up, the picture to be seen, until closed.
                fflush(stdout);
                angel::waitForWindows();
                return status;
        }

} // namespace

void angel::reportException(asIScriptContext *ctx) {
        reportExceptionHere(ctx);
        return;
}

int main(int argc, char **argv) {
        bool checkOnly = false;
        int  first = 1;
        if (argc >= 2 && (strcmp(argv[1], "--api") == 0 || strcmp(argv[1], "--api-help") == 0)) {
                asIScriptEngine *engine = makeEngine();
                angel::printApi(engine, strcmp(argv[1], "--api-help") == 0);
                engine->ShutDownAndRelease();
                return 0;
        }
        if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
                printf("angel %s (AngelScript %s%s)\n", ANGEL_VERSION, ANGELSCRIPT_VERSION_STRING,
#ifdef AS_FLOAT_LITERALS
                       ", decimal literals are float"
#else
                       ", decimal literals are double"
#endif
                );
                return 0;
        }
        if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
                checkOnly = true;
                first = 2;
        }
        if (first >= argc || argv[first][0] == '-') {
                usage();
                return ExitUsage;
        }
        angel::setArguments(argc - first, argv + first);
        if (load(argv[first], nullptr, 0) < 0 || loadErrors != 0) return ExitFailure;
        asIScriptEngine *engine = makeEngine();
        asIScriptModule *module = engine->GetModule("script", asGM_ALWAYS_CREATE);
        // The script first: only its section may have top-level statements.
        for (int i = 0; i < sourceCount; ++i)
                if (module->AddScriptSection(sources[i].path, sources[i].code, sources[i].length) < 0) return 70;
        int status = ExitFailure;
        if (module->Build() >= 0) status = checkOnly ? 0 : run(engine, module);
        fflush(stdout);
        module->Discard();
        angel::releaseArguments();
        engine->ShutDownAndRelease();
        angel::restoreTerminal();
        return status;
}
