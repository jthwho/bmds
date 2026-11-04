/**
 * @file      program.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The program's part of the core script API, which a script run as a program has and a deck's
 * doesn't (devplan/preso.md §7.9): input, the command line (`args`), the environment, exit and
 * sleep. registerCore() is it and the language's part (core.cpp) together.
 */

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

namespace angel {

        namespace {

                int           argumentCount = 0;
                char        **argumentValues = nullptr;
                CScriptArray *arguments = nullptr;

        } // namespace

        void setArguments(int count, char **values) {
                argumentCount = count;
                argumentValues = values;
                return;
        }

        const char *scriptPath() {
                return argumentCount > 0 ? argumentValues[0] : nullptr;
        }

        void releaseArguments() {
                if (arguments != nullptr) arguments->Release();
                arguments = nullptr;
                return;
        }

        namespace {

                // ---- input

                // A line from stdin, without its newline ("" at the end of the input).
                void readLine(asIScriptGeneric *gen) {
                        fflush(stdout);
                        cookedTerminal();
                        Text line;
                        char chunk[256];
                        while (fgets(chunk, sizeof(chunk), stdin) != nullptr) {
                                size_t     n = strlen(chunk);
                                const bool end = n != 0 && chunk[n - 1] == '\n';
                                if (end) --n;
                                line.append(chunk, n);
                                if (end) break;
                        }
                        size_t n = line.length();
                        if (n != 0 && line.data()[n - 1] == '\r') --n;
                        new (gen->GetAddressOfReturnLocation()) String(line.data(), n);
                }
                void input(asIScriptGeneric *gen) {
                        readLine(gen);
                }
                void inputPrompt(asIScriptGeneric *gen) {
                        const String *prompt = static_cast<const String *>(gen->GetArgAddress(0));
                        fwrite(prompt->data(), 1, prompt->length(), stdout);
                        readLine(gen);
                }

                // ---- Time

                void sleepMs(asIScriptGeneric *gen) {
                        fflush(stdout);
                        const uint32_t  ms = gen->GetArgDWord(0);
                        struct timespec t = {time_t(ms / 1000), long(ms % 1000) * 1000000L};
                        while (nanosleep(&t, &t) != 0) {}
                }

                // ---- The program

                void exitScript(asIScriptGeneric *gen) {
                        restoreTerminal();
                        exit(int(gen->GetArgDWord(0)));
                }
                void env(asIScriptGeneric *gen) {
                        const String *name = static_cast<const String *>(gen->GetArgAddress(0));
                        const char   *value = getenv(name->cStr());
                        new (gen->GetAddressOfReturnLocation()) String(value != nullptr ? value : "");
                }

        } // namespace

        void registerProgram(asIScriptEngine *engine) {
                def(engine, "string input()", input,
                    "Reads a line typed (without its Enter; \"\" at the end of the input).");
                def(engine, "string input(const string &in prompt)", inputPrompt,
                    "Shows the prompt, then reads a line.");
                def(engine, "void sleep(uint ms)", sleepMs, "Waits so many milliseconds.");
                arguments = CScriptArray::Create(stringArrayType(engine), asUINT(argumentCount));
                for (int i = 0; i < argumentCount; ++i) {
                        String *s = static_cast<String *>(arguments->At(asUINT(i)));
                        *s = String(argumentValues[i]);
                }
                property(engine, "const array<string> args", arguments,
                         "The command line: args[0] is the script, then its arguments.");
                def(engine, "string env(const string &in name)", env,
                    "An environment variable (\"\" if it isn't set).");
                def(engine, "void exit(int status = 0)", exitScript,
                    "Ends the script, with an exit status (0: all went well).");
                return;
        }

        void registerCore(asIScriptEngine *engine) {
                registerLanguage(engine);
                registerProgram(engine);
                return;
        }

} // namespace angel
