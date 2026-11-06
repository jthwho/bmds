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
 * `file:line:column: error: message`; `--report-fd N` writes them to descriptor N as well, a
 * tab-separated line each, and an uncaught exception too (for an editor's Run). `--serve` is the
 * editor's checker (devplan/editor.md §9): requests on stdin, answers on stdout. `--debug N` is
 * its debugger (§17, debug.cpp): commands on descriptor N, events on --report-fd's. A script
 * of a scene (Preso's: SceneRect, scene.play()...) plays on a stage, a window of its own
 * (devplan/preso.md §8c, programs/preso/script/stage.cpp).
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../preso/script/stage.h"
#include "angel.h"
#ifdef ANGEL_BMDS
#include "../preso/bmds/bmdsplatform.h"
#else
#include "../preso/headless/headless.h"
#endif

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
                fprintf(stderr, "usage: angel [-c] [--report-fd N [--debug N]] SCRIPT [ARGUMENT...]\n"
                                "       angel --api | --api-help | --serve | --version\n"
                                "  -c              check the script: report its errors, run nothing\n"
                                "  --report-fd N   errors and an uncaught exception to descriptor N too\n"
                                "  --debug N       debugger commands on descriptor N, events on --report-fd's\n"
                                "  --api           the script API's declarations, one a line\n"
                                "  --api-help      the same, each with its help\n"
                                "  --serve         answer an editor's requests (check, api) on stdin\n");
                return;
        }

        // ---- Diagnostics

        FILE *reportFile = nullptr; // --report-fd's
        bool  serving = false;      // --serve: diagnostics are the answer's

        // A field of a tab-separated line: tabs and newlines would split it.
        void field(FILE *f, const char *text) {
                for (const char *p = text != nullptr ? text : ""; *p != 0; ++p)
                        fputc(*p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p, f);
                return;
        }

        void reportLine(FILE *f, const char *prefix, const char *severity, const char *file, int line, int column,
                        const char *text) {
                if (prefix != nullptr) fprintf(f, "%s\t", prefix);
                field(f, severity);
                fputc('\t', f);
                field(f, file);
                fprintf(f, "\t%d\t%d\t", line, column);
                field(f, text);
                fputc('\n', f);
                return;
        }

        // Every diagnostic: as a compiler writes it on stderr, a line on --report-fd's too; when
        // serving, a line of the answer instead.
        void diagnostic(const char *severity, const char *file, int line, int column, const char *text) {
                if (serving) {
                        reportLine(stdout, "diag", severity, file, line, column, text);
                        return;
                }
                fprintf(stderr, "%s:%d:%d: %s: %s\n", file, line, column, severity, text);
                if (reportFile != nullptr) {
                        reportLine(reportFile, nullptr, severity, file, line, column, text);
                        fflush(reportFile);
                }
                return;
        }

        // A stage's error (its script failed while it played).
        void stageReport(const char *file, int line, const char *message) {
                diagnostic("error", file, line, 1, message);
                return;
        }

        void message(const asSMessageInfo *msg, void *) {
                const char *kind = "error";
                if (msg->type == asMSGTYPE_WARNING)
                        kind = "warning";
                else if (msg->type == asMSGTYPE_INFORMATION) {
                        // "Compiling void f()" heads every function's messages: noise here.
                        if (strncmp(msg->message, "Compiling ", 10) == 0) return;
                        kind = "note";
                }
                diagnostic(kind, msg->section, msg->row, msg->col, msg->message);
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
                if (reportFile != nullptr) {
                        reportLine(reportFile, nullptr, "exception", section != nullptr ? section : "?", line, column,
                                   ctx->GetExceptionString());
                        fflush(reportFile);
                }
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
                Text text;
                text.appendf(format, detail);
                diagnostic("error", path, line, 1, text.data());
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

        // The sources forgotten (a check's, before the next).
        void resetSources() {
                for (int i = 0; i < sourceCount; ++i) {
                        free(sources[i].path);
                        free(sources[i].canonical);
                        free(sources[i].code);
                }
                sourceCount = 0;
                loadErrors = 0;
                return;
        }

        // The script from memory (an editor's unsaved text), its includes from the files beside
        // @p path.
        bool loadText(const char *path, const char *code, size_t length) {
                char  resolved[PATH_MAX];
                char *canonical = realpath(path, resolved) != nullptr ? strdup(resolved) : strdup(path);
                char *copy = static_cast<char *>(malloc(length + 1));
                if (canonical == nullptr || copy == nullptr) {
                        free(canonical);
                        free(copy);
                        return false;
                }
                memcpy(copy, code, length);
                copy[length] = 0;
                sources[sourceCount++] = Source{strdup(path), canonical, copy, length};
                preprocess(0);
                return true;
        }

        // ---- The engine

        asIScriptEngine *makeEngine() {
                asIScriptEngine *engine = angel::createEngine(message, nullptr, false);
                if (engine == nullptr) {
                        fprintf(stderr, "angel: no script engine (out of memory, or a setting refused)\n");
                        exit(70);
                }
                angel::registerString(engine);
                angel::registerCore(engine);
                angel::registerConsole(engine);
                angel::registerFiles(engine);
                angel::registerSystem(engine);
                angel::registerFonts(engine);
                angel::registerGraphics(engine);
                angel::registerSound(engine);
                angel::registerNetwork(engine);
                angel::registerGui(engine);
                angel::registerServices(engine); // after the GUI: its callbacks' funcdefs
                angel::registerProcess(engine);
                angel::registerFolders(engine);
                angel::registerAudio(engine);
                preso::registerSceneApi(engine); // after Font, Event and Callback, which it names
                return engine;
        }

        // ---- Running

        // What runs: the top-level statements or main(); null (and why said) if neither will do.
        asIScriptFunction *entryOf(asIScriptModule *module) {
                asIScriptFunction *top = module->GetFunctionByName(TopLevel);
                asIScriptFunction *main = module->GetFunctionByName("main");
                if (top != nullptr && main != nullptr) {
                        const char *section = nullptr;
                        int         line = 0, column = 0;
                        main->GetDeclaredAt(&section, &line, &column);
                        diagnostic("error", section != nullptr ? section : sources[0].path, line, column,
                                   "a script with statements outside functions runs them; it can't have a main() "
                                   "as well");
                        return nullptr;
                }
                asIScriptFunction *entry = top != nullptr ? top : main;
                if (entry == nullptr && preso::Stage::hasDraw(module)) return nullptr; // a scene's draw() only
                if (entry == nullptr) {
                        diagnostic("error", sources[0].path, 1, 1,
                                   "nothing to run: no statements outside functions, and no main()");
                        return nullptr;
                }
                if (entry->GetParamCount() != 0 ||
                    (entry->GetReturnTypeId() != asTYPEID_VOID && entry->GetReturnTypeId() != asTYPEID_INT32)) {
                        const char *section = nullptr;
                        int         line = 0, column = 0;
                        entry->GetDeclaredAt(&section, &line, &column);
                        diagnostic("error", section != nullptr ? section : sources[0].path, line, column,
                                   "main() must be 'void main()' or 'int main()'");
                        return nullptr;
                }
                return entry;
        }

        int run(asIScriptEngine *engine, asIScriptModule *module, preso::Stage &stage) {
                asIScriptFunction *entry = entryOf(module);
                if (entry == nullptr && !preso::Stage::hasDraw(module)) return ExitFailure;
                int status = 0;
                if (entry != nullptr) {
                        asIScriptContext *ctx = engine->CreateContext();
                        if (ctx == nullptr || ctx->Prepare(entry) < 0) {
                                fprintf(stderr, "angel: can't start the script\n");
                                return 70;
                        }
                        if (angel::isDebugging()) angel::debugContext(ctx);
                        const int r = ctx->Execute();
                        if (r == asEXECUTION_FINISHED) {
                                if (entry->GetReturnTypeId() == asTYPEID_INT32) status = int(ctx->GetReturnDWord());
                        } else if (r == asEXECUTION_EXCEPTION) {
                                fflush(stdout);
                                reportExceptionHere(ctx);
                                status = ExitFailure;
                        } else if (r != asEXECUTION_ABORTED || !stage.isWanted()) {
                                fprintf(stderr, "angel: the script stopped (%d)\n", r);
                                status = ExitFailure;
                        }
                        ctx->Release();
                }
                // A scene: its first scene call stopped the run, or a draw() is left to show. The
                // stage plays it from its start.
                if (status == 0 && (stage.isWanted() || preso::Stage::hasDraw(module))) {
                        fflush(stdout);
                        status = stage.play(module);
                        stage.finish();
                }
                // Windows stay up, the picture to be seen, until closed.
                fflush(stdout);
                angel::finishSound();
                angel::finishGui(engine); // its windows (and screens it serves) served until the last closes
                angel::waitForWindows();
                return status;
        }

        // ---- Serving an editor (devplan/editor.md §9)

        // The script's own names, for completion: `symbol KIND NAME DECLARATION FILE LINE`.
        void printSymbols(asIScriptEngine *engine, asIScriptModule *module) {
                for (asUINT i = 0; i < module->GetFunctionCount(); ++i) {
                        asIScriptFunction *f = module->GetFunctionByIndex(i);
                        if (f == nullptr || strcmp(f->GetName(), TopLevel) == 0) continue;
                        const char *section = nullptr;
                        int         line = 0;
                        f->GetDeclaredAt(&section, &line, nullptr);
                        printf("symbol\tfunction\t");
                        field(stdout, f->GetName());
                        putchar('\t');
                        field(stdout, f->GetDeclaration(true, true, true));
                        putchar('\t');
                        field(stdout, section);
                        printf("\t%d\n", line);
                }
                for (asUINT i = 0; i < module->GetGlobalVarCount(); ++i) {
                        const char *name = nullptr;
                        if (module->GetGlobalVar(i, &name) < 0) continue;
                        printf("symbol\tvariable\t");
                        field(stdout, name);
                        putchar('\t');
                        field(stdout, module->GetGlobalVarDeclaration(i, true));
                        printf("\t\t0\n");
                }
                for (asUINT i = 0; i < module->GetObjectTypeCount(); ++i) {
                        asITypeInfo *type = module->GetObjectTypeByIndex(i);
                        printf("symbol\tclass\t");
                        field(stdout, type->GetName());
                        printf("\tclass ");
                        field(stdout, type->GetName());
                        printf("\t\t0\n");
                        for (asUINT m = 0; m < type->GetMethodCount(); ++m) {
                                asIScriptFunction *f = type->GetMethodByIndex(m);
                                printf("symbol\tmethod\t%s.", type->GetName());
                                field(stdout, f->GetName());
                                putchar('\t');
                                field(stdout, f->GetDeclaration(true, true, true));
                                printf("\t\t0\n");
                        }
                        for (asUINT p = 0; p < type->GetPropertyCount(); ++p) {
                                const char *name = nullptr;
                                type->GetProperty(p, &name);
                                printf("symbol\tproperty\t%s.", type->GetName());
                                field(stdout, name);
                                putchar('\t');
                                field(stdout, type->GetPropertyDeclaration(p, true));
                                printf("\t\t0\n");
                        }
                }
                for (asUINT i = 0; i < module->GetEnumCount(); ++i) {
                        asITypeInfo *e = module->GetEnumByIndex(i);
                        printf("symbol\tenum\t");
                        field(stdout, e->GetName());
                        printf("\tenum ");
                        field(stdout, e->GetName());
                        printf("\t\t0\n");
                        for (asUINT v = 0; v < e->GetEnumValueCount(); ++v) {
                                int         value = 0;
                                const char *name = e->GetEnumValueByIndex(v, &value);
                                printf("symbol\tconstant\t%s::", e->GetName());
                                field(stdout, name);
                                printf("\t%s::", e->GetName());
                                field(stdout, name);
                                printf(" = %d\t\t0\n", value);
                        }
                }
                (void)engine;
                return;
        }

        // Requests on stdin, a line each (`check PATH LENGTH` and the text's LENGTH bytes; `api`;
        // `quit`); each answer's lines on stdout, then `end`. One engine for all: a check builds
        // the text and runs nothing (globals aren't initialized either).
        int serve() {
                serving = true;
                static char  argv0[] = "script";
                static char *argv[] = {argv0, nullptr};
                angel::setArguments(1, argv);
                asIScriptEngine *engine = makeEngine();
                angel::registerGuiClasses(engine, nullptr, nullptr, 0); // an editor's: every class
                engine->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, 0);
                char header[1024];
                while (fgets(header, sizeof(header), stdin) != nullptr) {
                        size_t n = strlen(header);
                        while (n > 0 && (header[n - 1] == '\n' || header[n - 1] == '\r')) header[--n] = 0;
                        char *tab = strchr(header, '\t');
                        if (tab != nullptr) *tab = 0;
                        if (strcmp(header, "quit") == 0) break;
                        if (strcmp(header, "api") == 0) {
                                angel::printApiRecords(engine);
                        } else if (strcmp(header, "check") == 0 && tab != nullptr) {
                                char *path = tab + 1;
                                char *lengthText = strchr(path, '\t');
                                if (lengthText != nullptr) *lengthText++ = 0;
                                const long length = lengthText != nullptr ? strtol(lengthText, nullptr, 10) : -1;
                                char *code = length >= 0 ? static_cast<char *>(malloc(size_t(length) + 1)) : nullptr;
                                if (code == nullptr || fread(code, 1, size_t(length), stdin) != size_t(length)) {
                                        free(code);
                                        printf("error\tbad check request\nend\n");
                                        fflush(stdout);
                                        if (code == nullptr && length >= 0) continue;
                                        break; // the stream is out of step: done
                                }
                                resetSources();
                                if (loadText(path, code, size_t(length))) {
                                        asIScriptModule *module = engine->GetModule("check", asGM_ALWAYS_CREATE);
                                        bool             added = true;
                                        for (int i = 0; i < sourceCount && added; ++i)
                                                added = module->AddScriptSection(sources[i].path, sources[i].code,
                                                                                 sources[i].length) >= 0;
                                        if (added && module->Build() >= 0) {
                                                (void)entryOf(module);
                                                printSymbols(engine, module);
                                        }
                                        module->Discard();
                                }
                                free(code);
                        } else {
                                printf("error\tunknown request\n");
                        }
                        printf("end\n");
                        fflush(stdout);
                }
                resetSources();
                angel::releaseGui();
                angel::releaseArguments();
                engine->ShutDownAndRelease();
                return 0;
        }

} // namespace

void angel::reportException(asIScriptContext *ctx) {
        reportExceptionHere(ctx);
        return;
}

int main(int argc, char **argv) {
        bool checkOnly = false;
        int  first = 1;
        int  debugFd = -1;
        if (argc >= 2 && (strcmp(argv[1], "--api") == 0 || strcmp(argv[1], "--api-help") == 0)) {
                asIScriptEngine *engine = makeEngine();
                angel::registerGuiClasses(engine, nullptr, nullptr, 0);
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
        if (argc >= 2 && strcmp(argv[1], "--serve") == 0) return serve();
        for (;;) {
                if (first < argc && strcmp(argv[first], "-c") == 0) {
                        checkOnly = true;
                        ++first;
                } else if (first + 1 < argc && strcmp(argv[first], "--report-fd") == 0) {
                        char      *end = nullptr;
                        const long fd = strtol(argv[first + 1], &end, 10);
                        reportFile = end != argv[first + 1] && *end == 0 && fd > 2 ? fdopen(int(fd), "w") : nullptr;
                        if (reportFile == nullptr) {
                                fprintf(stderr, "angel: --report-fd %s: not a descriptor open for writing\n",
                                        argv[first + 1]);
                                return ExitUsage;
                        }
                        first += 2;
                } else if (first + 1 < argc && strcmp(argv[first], "--debug") == 0) {
                        char      *end = nullptr;
                        const long fd = strtol(argv[first + 1], &end, 10);
                        if (end == argv[first + 1] || *end != 0 || fd <= 2) {
                                fprintf(stderr, "angel: --debug %s: not a descriptor\n", argv[first + 1]);
                                return ExitUsage;
                        }
                        debugFd = int(fd);
                        first += 2;
                } else {
                        break;
                }
        }
        if (debugFd >= 0 && !angel::startDebugger(debugFd, reportFile)) {
                fprintf(stderr, "angel: --debug needs --report-fd (its events go there)\n");
                return ExitUsage;
        }
        if (first >= argc || argv[first][0] == '-') {
                usage();
                return ExitUsage;
        }
        angel::setArguments(argc - first, argv + first);
        if (load(argv[first], nullptr, 0) < 0 || loadErrors != 0) return ExitFailure;
#ifdef ANGEL_BMDS
        preso::BmdsPlatform platform;
#else
        preso::HeadlessPlatform platform;
#endif
        preso::Stage     stage(platform, argv[first], stageReport);
        asIScriptEngine *engine = makeEngine();
        stage.attach(engine);
        // Debugged: built with line cues, where the line callback is called.
        if (angel::isDebugging()) engine->SetEngineProperty(asEP_BUILD_WITHOUT_LINE_CUES, 0);
        asIScriptModule *module = engine->GetModule("script", asGM_ALWAYS_CREATE);
        // The script first: only its section may have top-level statements.
        for (int i = 0; i < sourceCount; ++i)
                if (module->AddScriptSection(sources[i].path, sources[i].code, sources[i].length) < 0) return 70;
        {
                // The GUI's classes the sources name, before they are built.
                const char *texts[MaxIncludes];
                size_t      lengths[MaxIncludes];
                for (int i = 0; i < sourceCount; ++i) {
                        texts[i] = sources[i].code;
                        lengths[i] = sources[i].length;
                }
                angel::registerGuiClasses(engine, texts, lengths, size_t(sourceCount));
        }
        int status = ExitFailure;
        if (module->Build() >= 0) status = checkOnly ? 0 : run(engine, module, stage);
        fflush(stdout);
        module->Discard();
        angel::releaseGui();
        angel::releaseArguments();
        engine->ShutDownAndRelease();
        angel::restoreTerminal();
        return status;
}
