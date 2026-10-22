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

#ifdef ANGEL_BMDS
#include <bmds/sys.h>
#endif

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

                // A setting's current value ("" if there's no such setting).
                void setting(asIScriptGeneric *gen) {
                        const String *key = static_cast<const String *>(gen->GetArgAddress(0));
                        String       *value = new (gen->GetAddressOfReturnLocation()) String();
#ifdef ANGEL_BMDS
                        const int32_t id = bmds_sysconf_find(key->cStr());
                        if (id < 0) return;
                        char          text[BMDS_SYSCONF_VALUE_MAX + 1];
                        const int32_t n = bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, text, sizeof(text));
                        if (n >= 0) value->assign(text, size_t(n));
#else
                        (void)key;
                        (void)value;
#endif
                }

                // A notification (a balloon on the desktop).
                void notify(asIScriptGeneric *gen) {
                        const String *title = static_cast<const String *>(gen->GetArgAddress(0));
                        const String *body = static_cast<const String *>(gen->GetArgAddress(1));
#ifdef ANGEL_BMDS
                        bmds_notification_t n{};
                        n.size = sizeof(n);
                        n.urgency = BMDS_NOTIFY_NORMAL;
                        n.timeout_ms = BMDS_NOTIFY_DEFAULT_TIMEOUT;
                        snprintf(n.title, sizeof(n.title), "%s", title->cStr());
                        snprintf(n.body, sizeof(n.body), "%s", body->cStr());
                        const int32_t r = bmds_notify_send(-1, &n);
                        if (r < 0) raise("Can't notify: %s", strerror(-r));
#else
                        (void)title;
                        (void)body;
#endif
                }

        } // namespace

        void registerSystem(asIScriptEngine *engine) {
                def(engine, "int run(const string &in command)", run,
                    "Runs a shell command, its output on the screen; its exit status (0: it worked).");
                def(engine, "string output(const string &in command)", output,
                    "Runs a shell command; what it printed.");
                def(engine, "string setting(const string &in key)", setting,
                    "A system setting's value (\"\" if there's no such setting).");
                def(engine, "void notify(const string &in title, const string &in text)", notify,
                    "Shows a notification on the desktop.");
                return;
        }

} // namespace angel
