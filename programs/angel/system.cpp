/**
 * @file      system.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Other programs: run() starts a shell command with its output on the terminal, output()
 * gives back what it printed.
 */

#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "angel.h"

namespace angel {

        namespace {

                int statusOf(int raw) {
                        if (raw < 0) return -1;
                        if (WIFEXITED(raw)) return WEXITSTATUS(raw);
                        if (WIFSIGNALED(raw)) return 128 + WTERMSIG(raw);
                        return -1;
                }

                void run(asIScriptGeneric *gen) {
                        const String *command = static_cast<const String *>(gen->GetArgAddress(0));
                        fflush(stdout);
                        cookedTerminal();
                        gen->SetReturnDWord(asDWORD(statusOf(system(command->cStr()))));
                }

                void output(asIScriptGeneric *gen) {
                        const String *command = static_cast<const String *>(gen->GetArgAddress(0));
                        fflush(stdout);
                        cookedTerminal();
                        Text  text;
                        FILE *p = popen(command->cStr(), "r");
                        if (p == nullptr) {
                                raise("Can't run '%s': %s", command->cStr(), strerror(errno));
                        } else {
                                char   chunk[1024];
                                size_t n;
                                while ((n = fread(chunk, 1, sizeof(chunk), p)) != 0) text.append(chunk, n);
                                pclose(p);
                        }
                        new (gen->GetAddressOfReturnLocation()) String(text.data(), text.length());
                }

        } // namespace

        void registerSystem(asIScriptEngine *engine) {
                def(engine, "int run(const string &in command)", run,
                    "Runs a shell command, its output on the screen; its exit status (0: it worked).");
                def(engine, "string output(const string &in command)", output,
                    "Runs a shell command; what it printed.");
                return;
        }

} // namespace angel
