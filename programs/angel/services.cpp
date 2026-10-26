/**
 * @file      services.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's services for scripts (devplan/guiprograms.md §14): `Setting` (the system's
 * configuration: read, set, saved, a script's own declared, watched), `system` (the host, the
 * memory, the CPU's load, the network's state and changes), `DateTime` (the date in any time
 * zone, with the zone database's history), and the script's folders (`appDir()`, `dataDir()`).
 *
 * Watches are descriptors the loop polls (gui.cpp's sources): a setting's change, or the
 * network's, is called back while `app.run()` runs. A watched setting is kept (a reference)
 * until its callback is taken away, as a GUI object is kept until destroyed.
 */

#include <errno.h>
#include <limits.h>
#include <new>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/sys.h>
#else
#include <ifaddrs.h>
#include <pwd.h>
#include <net/if.h>
#endif

namespace angel {

        namespace {

                const String &argString(asIScriptGeneric *gen, int i) {
                        return *static_cast<const String *>(gen->GetArgAddress(asUINT(i)));
                }

                void returnString(asIScriptGeneric *gen, const char *text, size_t length) {
                        new (gen->GetAddressOfReturnLocation()) String(text, length);
                }

                void returnString(asIScriptGeneric *gen, const char *text) {
                        returnString(gen, text, strlen(text));
                }

                // ---- Setting

                constexpr size_t MaxWatched = 32;

                class Setting {
                        public:
                                String             key;
                                int32_t            id = -1; // the system's, found when first needed
                                LoopSource         watch{-1, nullptr, nullptr, LoopHold::Run};
                                asIScriptFunction *onChanged = nullptr;

                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }

                                // Its id (-1: no such setting, yet).
                                int32_t find() {
#ifdef ANGEL_BMDS
                                        if (id < 0) id = bmds_sysconf_find(key.cStr());
                                        if (id < 0) id = -1;
#endif
                                        return id;
                                }

                                // Its watch ended and its callback let go of.
                                void unwatch() {
                                        if (watch.fd >= 0) {
                                                removeLoopSource(&watch);
                                                close(watch.fd);
                                                watch.fd = -1;
                                        }
                                        if (onChanged != nullptr) onChanged->Release();
                                        onChanged = nullptr;
                                        return;
                                }

                        private:
                                ~Setting() { unwatch(); }
                                int _refs = 1;
                };

                Setting *watched[MaxWatched]; // a reference each
                size_t   watchedCount = 0;

                Setting *self(asIScriptGeneric *gen) {
                        return static_cast<Setting *>(gen->GetObject());
                }

                // A text field of @p s's (`BMDS_SYSCONF_FIELD_...`), "" if there's none.
                void returnField(asIScriptGeneric *gen, Setting *s, uint32_t field) {
#ifdef ANGEL_BMDS
                        char          text[BMDS_SYSCONF_VALUE_MAX + 1];
                        const int32_t n = s->find() >= 0 ? bmds_sysconf_get(s->id, field, text, sizeof(text)) : -1;
                        returnString(gen, text, n > 0 ? size_t(n) : 0);
#else
                        (void)s;
                        (void)field;
                        returnString(gen, "", 0);
#endif
                }

                // Its flags (`BMDS_SYSCONF_IS_DEFAULT`...) and type; false if there's no such setting.
                bool infoOf(Setting *s, uint32_t &flags, uint32_t &type) {
#ifdef ANGEL_BMDS
                        bmds_sysconf_info_t info;
                        memset(&info, 0, sizeof(info));
                        info.size = sizeof(info);
                        if (s->find() < 0 || bmds_sysconf_info(s->id, &info) < 0) return false;
                        flags = info.flags;
                        type = info.type;
                        return true;
#else
                        (void)s;
                        (void)flags;
                        (void)type;
                        return false;
#endif
                }

                // A change of @p s's (@p op, @p value), its error raised.
                void change(Setting *s, uint32_t op, const char *value) {
#ifdef ANGEL_BMDS
                        if (s->find() < 0) {
                                raise("No setting '%s'", s->key.cStr());
                                return;
                        }
                        const int32_t r = bmds_sysconf_set(s->id, op, value);
                        if (r >= 0) return;
                        if (op == BMDS_SYSCONF_OP_SET && r == -BMDS_EINVAL)
                                raise("Can't set '%s' to '%s': not of its type", s->key.cStr(), value);
                        else if (op == BMDS_SYSCONF_OP_SET && r == -BMDS_ERANGE)
                                raise("Can't set '%s' to '%s': outside its limits", s->key.cStr(), value);
                        else
                                raise("Can't change '%s': %s", s->key.cStr(), strerror(-r));
#else
                        (void)op;
                        (void)value;
                        raise("No setting '%s'", s->key.cStr());
#endif
                        return;
                }

                void settingMake(asIScriptGeneric *gen) {
                        Setting *s = new (std::nothrow) Setting();
                        if (s == nullptr) {
                                raise("Out of memory");
                        } else {
                                s->key = argString(gen, 0);
                                (void)s->find();
                        }
                        gen->SetReturnAddress(s);
                }

#ifdef ANGEL_BMDS
                // A declaration's type: a word, `int MIN MAX`, or an enum's words (`a|b|c`).
                bool parseType(const char *text, bmds_sysconf_param_t &p) {
                        static constexpr struct {
                                        const char *name;
                                        uint32_t    type;
                        } Types[] = {{"bool", BMDS_SYSCONF_TYPE_BOOL},   {"string", BMDS_SYSCONF_TYPE_STRING},
                                     {"color", BMDS_SYSCONF_TYPE_COLOR}, {"path", BMDS_SYSCONF_TYPE_PATH},
                                     {"paths", BMDS_SYSCONF_TYPE_PATHS}, {"secret", BMDS_SYSCONF_TYPE_SECRET}};
                        if (strchr(text, '|') != nullptr) {
                                p.type = BMDS_SYSCONF_TYPE_ENUM;
                                p.choices = text;
                                return true;
                        }
                        for (const auto &t : Types) {
                                if (strcmp(text, t.name) != 0) continue;
                                p.type = t.type;
                                return true;
                        }
                        if (strncmp(text, "int", 3) != 0 || (text[3] != 0 && text[3] != ' ')) return false;
                        p.type = BMDS_SYSCONF_TYPE_INT;
                        p.min = INT32_MIN;
                        p.max = INT32_MAX;
                        if (text[3] == 0) return true;
                        char      *end = nullptr;
                        const long min = strtol(text + 4, &end, 10);
                        if (end == text + 4 || *end != ' ') return false;
                        const char *from = end + 1;
                        const long  max = strtol(from, &end, 10);
                        if (end == from || *end != 0 || min > max) return false;
                        p.min = int32_t(min);
                        p.max = int32_t(max);
                        return true;
                }
#endif

                void settingDeclare(asIScriptGeneric *gen) {
                        const String &key = argString(gen, 0);
                        const String &type = argString(gen, 1);
#ifdef ANGEL_BMDS
                        bmds_sysconf_param_t p;
                        memset(&p, 0, sizeof(p));
                        p.size = sizeof(p);
                        p.key = key.cStr();
                        p.default_value = argString(gen, 2).cStr();
                        p.help = argString(gen, 3).length() != 0 ? argString(gen, 3).cStr() : nullptr;
                        if (!parseType(type.cStr(), p)) {
                                raise("Can't declare '%s': no type '%s' (bool, int, int MIN MAX, string, color, path, "
                                      "paths, secret, or an enum's words: a|b|c)",
                                      key.cStr(), type.cStr());
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        const int32_t id = bmds_sysconf_declare(&p);
                        if (id < 0) {
                                if (id == -BMDS_EEXIST)
                                        raise("Can't declare '%s': it's there with another type", key.cStr());
                                else if (id == -BMDS_ENOSPC)
                                        raise("Can't declare '%s': too many declared", key.cStr());
                                else
                                        raise("Can't declare '%s' (a key is owner/name in lowercase; the default "
                                              "must be of the type)",
                                              key.cStr());
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        Setting *s = new (std::nothrow) Setting();
                        if (s == nullptr) {
                                raise("Out of memory");
                        } else {
                                s->key = key;
                                s->id = id;
                        }
                        gen->SetReturnAddress(s);
#else
                        (void)type;
                        raise("Can't declare '%s': no settings here", key.cStr());
                        gen->SetReturnAddress(nullptr);
#endif
                }

                void settingAddRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }
                void settingRelease(asIScriptGeneric *gen) {
                        self(gen)->release();
                }
                void settingKey(asIScriptGeneric *gen) {
                        const String &key = self(gen)->key;
                        returnString(gen, key.data(), key.length());
                }
                void settingExists(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->find() >= 0);
                }
                void settingValue(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        returnField(gen, self(gen), BMDS_SYSCONF_FIELD_VALUE);
#else
                        returnField(gen, self(gen), 0);
#endif
                }
                void settingSetValue(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        change(self(gen), BMDS_SYSCONF_OP_SET, argString(gen, 0).cStr());
#else
                        change(self(gen), 0, argString(gen, 0).cStr());
#endif
                }
                void settingDefault(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        returnField(gen, self(gen), BMDS_SYSCONF_FIELD_DEFAULT);
#else
                        returnField(gen, self(gen), 0);
#endif
                }
                void settingHelp(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        returnField(gen, self(gen), BMDS_SYSCONF_FIELD_HELP);
#else
                        returnField(gen, self(gen), 0);
#endif
                }
                void settingChoices(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        returnField(gen, self(gen), BMDS_SYSCONF_FIELD_CHOICES);
#else
                        returnField(gen, self(gen), 0);
#endif
                }
                void settingType(asIScriptGeneric *gen) {
                        static constexpr const char *Names[] = {"bool", "int",  "string", "color",
                                                                "enum", "path", "paths",  "secret"};
                        uint32_t                     flags = 0, type = 0;
                        const bool                   known = infoOf(self(gen), flags, type);
                        returnString(gen, known && type < sizeof(Names) / sizeof(Names[0]) ? Names[type] : "");
                }
                void flagOf(asIScriptGeneric *gen, uint32_t flag) {
                        uint32_t flags = 0, type = 0;
                        gen->SetReturnByte(infoOf(self(gen), flags, type) && (flags & flag) != 0);
                }
#ifdef ANGEL_BMDS
                void settingIsDefault(asIScriptGeneric *gen) {
                        flagOf(gen, BMDS_SYSCONF_IS_DEFAULT);
                }
                void settingIsSaved(asIScriptGeneric *gen) {
                        flagOf(gen, BMDS_SYSCONF_IS_SAVED);
                }
                void settingChanged(asIScriptGeneric *gen) {
                        flagOf(gen, BMDS_SYSCONF_CHANGED);
                }
                void settingSave(asIScriptGeneric *gen) {
                        change(self(gen), BMDS_SYSCONF_OP_SAVE, nullptr);
                }
                void settingRevert(asIScriptGeneric *gen) {
                        change(self(gen), BMDS_SYSCONF_OP_REVERT, nullptr);
                }
                void settingReset(asIScriptGeneric *gen) {
                        change(self(gen), BMDS_SYSCONF_OP_SETDEF, nullptr);
                }
#else
                void settingIsDefault(asIScriptGeneric *gen) {
                        flagOf(gen, 1);
                }
                void settingIsSaved(asIScriptGeneric *gen) {
                        flagOf(gen, 2);
                }
                void settingChanged(asIScriptGeneric *gen) {
                        flagOf(gen, 4);
                }
                void settingSave(asIScriptGeneric *gen) {
                        change(self(gen), 0, nullptr);
                }
                void settingRevert(asIScriptGeneric *gen) {
                        change(self(gen), 0, nullptr);
                }
                void settingReset(asIScriptGeneric *gen) {
                        change(self(gen), 0, nullptr);
                }
#endif

#ifdef ANGEL_BMDS
                // The loop: a watched setting changed (or more changed than the watch kept).
                void settingReady(LoopSource *source) {
                        Setting      *s = static_cast<Setting *>(source->context);
                        struct pollfd p = {source->fd, POLLIN, 0};
                        if (poll(&p, 1, 0) <= 0) return; // its read would wait
                        uint32_t      ids[16];
                        const ssize_t n = read(source->fd, ids, sizeof(ids));
                        bool          mine = false;
                        for (ssize_t i = 0; i < n / ssize_t(sizeof(uint32_t)); ++i)
                                mine = mine || int32_t(ids[i]) == s->id || ids[i] == 0xffffffffu;
                        if (!mine || s->onChanged == nullptr) return;
#ifdef ANGEL_BMDS
                        char          text[BMDS_SYSCONF_VALUE_MAX + 1];
                        const int32_t length = bmds_sysconf_get(s->id, BMDS_SYSCONF_FIELD_VALUE, text, sizeof(text));
                        const CallArg arg{text, length > 0 ? size_t(length) : 0, 0};
                        asIScriptFunction *fn = s->onChanged;
                        s->addRef();
                        fn->AddRef();
                        callScript(fn, &arg, 1);
                        fn->Release();
                        s->release();
#endif
                }
#endif

                void unkeepSetting(Setting *s) {
                        for (size_t i = 0; i < watchedCount; ++i) {
                                if (watched[i] != s) continue;
                                watched[i] = watched[--watchedCount];
                                s->unwatch();
                                s->release();
                                return;
                        }
                        return;
                }

                void settingOnChanged(asIScriptGeneric *gen) {
                        Setting           *s = self(gen);
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (fn == nullptr) {
                                unkeepSetting(s);
                                return;
                        }
                        if (s->onChanged != nullptr) {
                                s->onChanged->Release();
                                s->onChanged = fn;
                                return;
                        }
#ifdef ANGEL_BMDS
                        if (s->find() < 0) {
                                fn->Release();
                                raise("No setting '%s'", s->key.cStr());
                                return;
                        }
                        if (watchedCount == MaxWatched) {
                                fn->Release();
                                raise("Too many settings watched (%u)", unsigned(MaxWatched));
                                return;
                        }
                        const int32_t fd = bmds_sysconf_watch(s->key.cStr(), BMDS_O_CLOEXEC);
                        if (fd < 0) {
                                fn->Release();
                                raise("Can't watch '%s': %s", s->key.cStr(), strerror(-fd));
                                return;
                        }
                        s->watch.fd = fd;
                        s->watch.ready = settingReady;
                        s->watch.context = s;
                        s->onChanged = fn;
                        if (!addLoopSource(&s->watch)) {
                                s->unwatch();
                                return;
                        }
                        s->addRef();
                        watched[watchedCount++] = s;
#else
                        fn->Release();
                        raise("No setting '%s'", s->key.cStr());
#endif
                }

                // settings(prefix): the keys under it.
                void settingsList(asIScriptGeneric *gen) {
                        CScriptArray *keys = CScriptArray::Create(stringArrayType(gen->GetEngine()));
#ifdef ANGEL_BMDS
                        const char *prefix = argString(gen, 0).cStr();
                        for (int32_t id = bmds_sysconf_next(prefix, -1); id >= 0; id = bmds_sysconf_next(prefix, id)) {
                                char          key[BMDS_SYSCONF_KEY_MAX + 1];
                                const int32_t n = bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_KEY, key, sizeof(key));
                                if (n < 0) continue;
                                String k(key, size_t(n));
                                keys->InsertLast(&k);
                        }
#endif
                        gen->SetReturnObject(keys);
                        keys->Release();
                }

                // ---- system

                int systemObject; // `system`'s address: the type has no state of its own

                asIScriptFunction *onNetwork = nullptr;
                LoopSource         networkWatch{-1, nullptr, nullptr, LoopHold::Run};

#ifdef ANGEL_BMDS
                bool systemInfo(bmds_system_info_t &info) {
                        memset(&info, 0, sizeof(info));
                        info.size = sizeof(info);
                        return bmds_system_info(&info) == 0;
                }
#endif

                void systemHostName(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_system_info_t info;
                        returnString(gen, systemInfo(info) ? info.hostname : "");
#else
                        char name[256] = "";
                        (void)gethostname(name, sizeof(name) - 1);
                        returnString(gen, name);
#endif
                }

                void systemUserName(asIScriptGeneric *gen) {
                        const char *name = getenv("USER");
                        if (name == nullptr || *name == 0) name = getenv("LOGNAME");
#ifndef ANGEL_BMDS
                        // The board's login sets USER; getpwuid() would bring 2 KB of passwd and nscd code.
                        if (name == nullptr || *name == 0) {
                                const struct passwd *pw = getpwuid(getuid());
                                name = pw != nullptr ? pw->pw_name : nullptr;
                        }
#endif
                        returnString(gen, name != nullptr ? name : "");
                }

                void systemBoard(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_system_info_t info;
                        returnString(gen, systemInfo(info) ? info.board : "");
#else
                        returnString(gen, "");
#endif
                }

                void systemVersion(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_system_info_t info;
                        returnString(gen, systemInfo(info) ? info.version : "");
#else
                        returnString(gen, "");
#endif
                }

                void systemCores(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_system_info_t info;
                        gen->SetReturnDWord(systemInfo(info) ? info.cores : 1);
#else
                        const long n = sysconf(_SC_NPROCESSORS_ONLN);
                        gen->SetReturnDWord(uint32_t(n > 0 ? n : 1));
#endif
                }

                void systemCpuMhz(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_system_info_t info;
                        gen->SetReturnDWord(systemInfo(info) ? info.cpu_mhz : 0);
#else
                        gen->SetReturnDWord(0);
#endif
                }

#ifdef ANGEL_BMDS
                bool memInfo(bmds_mem_info_t &info) {
                        memset(&info, 0, sizeof(info));
                        info.size = sizeof(info);
                        return bmds_mem_info(0, &info) == 0;
                }
#endif

                void systemMemoryTotal(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_mem_info_t info;
                        gen->SetReturnQWord(memInfo(info) ? info.total_bytes : 0);
#else
                        const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
                        gen->SetReturnQWord(pages > 0 && size > 0 ? uint64_t(pages) * uint64_t(size) : 0);
#endif
                }

                void systemMemoryFree(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_mem_info_t info;
                        gen->SetReturnQWord(memInfo(info) ? info.free_bytes : 0);
#else
                        const long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
                        gen->SetReturnQWord(pages > 0 && size > 0 ? uint64_t(pages) * uint64_t(size) : 0);
#endif
                }

                void systemProgramMemory(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        bmds_mem_info_t info;
                        gen->SetReturnQWord(memInfo(info) ? info.program_bytes : 0);
#else
                        gen->SetReturnQWord(0);
#endif
                }

#ifdef ANGEL_BMDS
                // The cores' busy-time counters (`cpu.coreN.busy_us`, totals in µs), found again when
                // the registry changes, and their last reading.
                constexpr size_t MaxCores = 8;
                struct CpuCounters {
                                uint32_t ids[MaxCores];
                                size_t   count = 0;
                                uint32_t generation = 0;
                                bool     found = false;
                                int64_t  busy = 0; // the cores' total at the last reading
                                int64_t  atNs = 0; // its time (0: none yet)
                };
                CpuCounters cpu;

                void findCpuCounters(uint32_t generation) {
                        cpu.count = 0;
                        cpu.generation = generation;
                        cpu.found = true;
                        cpu.atNs = 0;
                        bmds_counter_t c;
                        for (uint32_t i = 0;; ++i) {
                                memset(&c, 0, sizeof(c));
                                c.size = sizeof(c);
                                if (bmds_stats_list(i, &c) < 0) break;
                                // cpu.coreN.busy_us (no sscanf: its 2.4 KB of vfscanf for this).
                                const char  *tail = ".busy_us";
                                const size_t length = strlen(c.name), tailLength = strlen(tail);
                                if (strncmp(c.name, "cpu.core", 8) != 0 || length <= 8 + tailLength ||
                                    strcmp(c.name + length - tailLength, tail) != 0)
                                        continue;
                                if (cpu.count < MaxCores) cpu.ids[cpu.count++] = c.id;
                        }
                        return;
                }
#endif

                // The cores' busy share since it was last asked (the first time: since boot); -1 when
                // CPU time isn't counted.
                void systemCpuLoad(asIScriptGeneric *gen) {
                        float load = -1;
#ifdef ANGEL_BMDS
                        bmds_stats_status_t status;
                        memset(&status, 0, sizeof(status));
                        status.size = sizeof(status);
                        if (bmds_stats_status(&status) == 0 && (status.flags & BMDS_STATS_CPU_COUNTED) != 0) {
                                if (!cpu.found || status.generation != cpu.generation)
                                        findCpuCounters(status.generation);
                                int64_t values[MaxCores];
                                int64_t now = 0;
                                if (cpu.count != 0 && bmds_stats_read(cpu.ids, uint32_t(cpu.count), values, nullptr,
                                                                      &now) == int32_t(cpu.count)) {
                                        int64_t busy = 0;
                                        bool    valid = true;
                                        for (size_t i = 0; i < cpu.count; ++i) {
                                                valid = valid && values[i] != BMDS_COUNTER_UNAVAILABLE;
                                                busy += values[i];
                                        }
                                        const int64_t spanUs = (now - cpu.atNs) / 1000 * int64_t(cpu.count);
                                        if (valid && spanUs > 0) {
                                                load = float(busy - cpu.busy) / float(spanUs);
                                                load = load < 0 ? 0 : load > 1 ? 1 : load;
                                        }
                                        if (valid) {
                                                cpu.busy = busy;
                                                cpu.atNs = now;
                                        }
                                }
                        }
#endif
                        gen->SetReturnFloat(load);
                }

                // An interface is up with its link and an address (loopback aside).
                void systemOnline(asIScriptGeneric *gen) {
                        bool online = false;
#ifdef ANGEL_BMDS
                        for (uint32_t i = 0; !online; ++i) {
                                bmds_net_info_t info;
                                memset(&info, 0, sizeof(info));
                                info.size = sizeof(info);
                                if (bmds_net_info(i, &info) < 0) break;
                                online = (info.flags & BMDS_NET_LOOPBACK) == 0 && info.address != 0 &&
                                         (info.flags & (BMDS_NET_UP | BMDS_NET_LINK)) == (BMDS_NET_UP | BMDS_NET_LINK);
                        }
#else
                        ifaddrs *list = nullptr;
                        if (getifaddrs(&list) == 0) {
                                for (const ifaddrs *i = list; i != nullptr && !online; i = i->ifa_next)
                                        online = i->ifa_addr != nullptr && i->ifa_addr->sa_family == AF_INET &&
                                                 (i->ifa_flags & IFF_LOOPBACK) == 0 &&
                                                 (i->ifa_flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING);
                                freeifaddrs(list);
                        }
#endif
                        gen->SetReturnByte(online);
                }

#ifdef ANGEL_BMDS
                void networkReady(LoopSource *source) {
                        struct pollfd p = {source->fd, POLLIN, 0};
                        if (poll(&p, 1, 0) <= 0) return;
                        uint32_t changes = 0;
                        if (read(source->fd, &changes, sizeof(changes)) != ssize_t(sizeof(changes))) return;
                        if (onNetwork == nullptr) return;
                        asIScriptFunction *fn = onNetwork;
                        fn->AddRef();
                        callScript(fn, nullptr, 0);
                        fn->Release();
                }
#endif

                void stopNetworkWatch() {
                        if (networkWatch.fd >= 0) {
                                removeLoopSource(&networkWatch);
                                close(networkWatch.fd);
                                networkWatch.fd = -1;
                        }
                        if (onNetwork != nullptr) onNetwork->Release();
                        onNetwork = nullptr;
                        return;
                }

                void systemOnNetworkChanged(asIScriptGeneric *gen) {
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (fn == nullptr) {
                                stopNetworkWatch();
                                return;
                        }
                        if (onNetwork != nullptr) onNetwork->Release();
                        onNetwork = fn;
#ifdef ANGEL_BMDS
                        if (networkWatch.fd >= 0) return;
                        const int32_t fd = bmds_net_watch(BMDS_O_CLOEXEC);
                        if (fd < 0) {
                                stopNetworkWatch();
                                raise("Can't watch the network: %s", strerror(-fd));
                                return;
                        }
                        networkWatch.fd = fd;
                        networkWatch.ready = networkReady;
                        if (!addLoopSource(&networkWatch)) stopNetworkWatch();
#endif
                }

                // ---- DateTime: a moment as it reads in a time zone

                struct DateTime {
                                int64_t seconds; // Unix time
                                int32_t offset;  // seconds east of UTC
                                int16_t year;
                                int16_t yearDay; // 0 up
                                int8_t  month;   // 1 up
                                int8_t  day;
                                int8_t  hour;
                                int8_t  minute;
                                int8_t  second;
                                int8_t  weekday; // 0 Sunday
                                char    abbreviation[10];
                                char    zone[48]; // as given ("": the program's)
                };

                // `TZ` switched to a zone while it lives (""/null: left as it is).
                class ZoneSwitch {
                        public:
                                explicit ZoneSwitch(const char *zone) {
                                        if (zone == nullptr || *zone == 0) return;
                                        const char *old = getenv("TZ");
                                        _old = old != nullptr ? strdup(old) : nullptr;
                                        _hadOld = old != nullptr;
                                        setenv("TZ", zone, 1);
                                        tzset();
                                        _switched = true;
                                }
                                ~ZoneSwitch() {
                                        if (!_switched) return;
                                        if (_hadOld && _old != nullptr)
                                                setenv("TZ", _old, 1);
                                        else
                                                unsetenv("TZ");
                                        tzset();
                                        free(_old);
                                }
                                ZoneSwitch(const ZoneSwitch &) = delete;
                                ZoneSwitch &operator=(const ZoneSwitch &) = delete;

                        private:
                                char *_old = nullptr;
                                bool  _hadOld = false;
                                bool  _switched = false;
                };

                void fillFrom(DateTime &d, const struct tm &t, int64_t seconds, const char *zone) {
                        d.seconds = seconds;
                        d.offset = int32_t(t.tm_gmtoff);
                        d.year = int16_t(t.tm_year + 1900);
                        d.yearDay = int16_t(t.tm_yday);
                        d.month = int8_t(t.tm_mon + 1);
                        d.day = int8_t(t.tm_mday);
                        d.hour = int8_t(t.tm_hour);
                        d.minute = int8_t(t.tm_min);
                        d.second = int8_t(t.tm_sec);
                        d.weekday = int8_t(t.tm_wday);
                        snprintf(d.abbreviation, sizeof(d.abbreviation), "%s", t.tm_zone != nullptr ? t.tm_zone : "");
                        snprintf(d.zone, sizeof(d.zone), "%s", zone != nullptr ? zone : "");
                        return;
                }

                void fill(DateTime &d, int64_t seconds, const char *zone) {
                        ZoneSwitch   in(zone);
                        const time_t t = time_t(seconds);
                        struct tm    parts;
                        memset(&parts, 0, sizeof(parts));
                        localtime_r(&t, &parts);
                        fillFrom(d, parts, seconds, zone);
                        return;
                }

                void dateNow(asIScriptGeneric *gen) {
                        struct timespec now;
                        clock_gettime(CLOCK_REALTIME, &now);
                        fill(*new (gen->GetObject()) DateTime(), int64_t(now.tv_sec), "");
                }

                void dateFromSeconds(asIScriptGeneric *gen) {
                        fill(*new (gen->GetObject()) DateTime(), int64_t(gen->GetArgQWord(0)),
                             argString(gen, 1).cStr());
                }

                void dateFromParts(asIScriptGeneric *gen) {
                        DateTime   *d = new (gen->GetObject()) DateTime();
                        const char *zone = argString(gen, 6).cStr();
                        ZoneSwitch  in(zone);
                        struct tm   parts;
                        memset(&parts, 0, sizeof(parts));
                        parts.tm_year = int(gen->GetArgDWord(0)) - 1900;
                        parts.tm_mon = int(gen->GetArgDWord(1)) - 1;
                        parts.tm_mday = int(gen->GetArgDWord(2));
                        parts.tm_hour = int(gen->GetArgDWord(3));
                        parts.tm_min = int(gen->GetArgDWord(4));
                        parts.tm_sec = int(gen->GetArgDWord(5));
                        parts.tm_isdst = -1;
                        const time_t t = mktime(&parts); // normalized: 32 October is 1 November
                        fillFrom(*d, parts, int64_t(t), zone);
                }

                const DateTime *date(asIScriptGeneric *gen) {
                        return static_cast<const DateTime *>(gen->GetObject());
                }

                void dateYear(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->year));
                }
                void dateMonth(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->month));
                }
                void dateDay(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->day));
                }
                void dateHour(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->hour));
                }
                void dateMinute(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->minute));
                }
                void dateSecond(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->second));
                }
                void dateWeekday(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->weekday));
                }
                void dateYearDay(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->yearDay));
                }
                void dateSeconds(asIScriptGeneric *gen) {
                        gen->SetReturnQWord(uint64_t(date(gen)->seconds));
                }
                void dateOffset(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(date(gen)->offset));
                }
                void dateZone(asIScriptGeneric *gen) {
                        returnString(gen, date(gen)->abbreviation);
                }

                void dateInZone(asIScriptGeneric *gen) {
                        DateTime d;
                        fill(d, date(gen)->seconds, argString(gen, 0).cStr());
                        gen->SetReturnObject(&d);
                }

                void formatDate(const DateTime &d, const char *pattern, Text &out) {
                        struct tm parts;
                        memset(&parts, 0, sizeof(parts));
                        parts.tm_year = d.year - 1900;
                        parts.tm_mon = d.month - 1;
                        parts.tm_mday = d.day;
                        parts.tm_hour = d.hour;
                        parts.tm_min = d.minute;
                        parts.tm_sec = d.second;
                        parts.tm_wday = d.weekday;
                        parts.tm_yday = d.yearDay;
                        parts.tm_gmtoff = d.offset;
                        parts.tm_zone = d.abbreviation;
                        // %Z put in here: musl's strftime only names a zone of its own tables.
                        Text own;
                        for (const char *c = pattern; *c != 0; ++c) {
                                if (c[0] == '%' && c[1] == 'Z') {
                                        own.append(d.abbreviation);
                                        ++c;
                                } else if (c[0] == '%' && c[1] != 0) {
                                        own.append(c, 2);
                                        ++c;
                                } else {
                                        own.append(c, 1);
                                }
                        }
                        char         text[256];
                        const size_t n = own.length() != 0 ? strftime(text, sizeof(text), own.data(), &parts) : 0;
                        out.append(text, n);
                        return;
                }

                void dateFormat(asIScriptGeneric *gen) {
                        Text text;
                        formatDate(*date(gen), argString(gen, 0).cStr(), text);
                        returnString(gen, text.data(), text.length());
                }

                void dateToString(asIScriptGeneric *gen) {
                        Text text;
                        formatDate(*date(gen), "%Y-%m-%d %H:%M:%S %Z", text);
                        returnString(gen, text.data(), text.length());
                }

                // ---- The script's folders

                char *scriptFolder = nullptr; // absolute, found at registration (before any chdir)

                void findScriptFolder() {
                        free(scriptFolder);
                        scriptFolder = nullptr;
                        const char *path = scriptPath();
                        Text        folder;
                        if (path == nullptr) path = "";
                        if (path[0] != '/') {
                                char cwd[PATH_MAX];
                                if (getcwd(cwd, sizeof(cwd)) != nullptr) folder.append(cwd);
                                if (folder.length() == 0 || folder.data()[folder.length() - 1] != '/')
                                        folder.append("/", 1);
                        }
                        const char *slash = strrchr(path, '/');
                        if (slash != nullptr) folder.append(path, size_t(slash - path));
                        if (folder.length() == 0) folder.append("/", 1);
                        scriptFolder = folder.release();
                        // "/home/me/" left with its slash: none at the end but the root's.
                        if (scriptFolder != nullptr) {
                                size_t n = strlen(scriptFolder);
                                while (n > 1 && scriptFolder[n - 1] == '/') scriptFolder[--n] = 0;
                        }
                        return;
                }

                void appDir(asIScriptGeneric *gen) {
                        const char *dir = getenv("BMDS_APP_DIR");
                        if (dir == nullptr || *dir == 0) dir = scriptFolder != nullptr ? scriptFolder : "";
                        returnString(gen, dir);
                }

                // Makes @p path and the folders above it. @return 0, or an errno.
                int makePath(char *path) {
                        for (char *p = path + 1;; ++p) {
                                if (*p != '/' && *p != 0) continue;
                                const char saved = *p;
                                *p = 0;
                                const int r = mkdir(path, 0755);
                                const int e = errno;
                                *p = saved;
                                if (r != 0 && e != EEXIST) return e;
                                if (saved == 0) return 0;
                        }
                }

                void dataDir(asIScriptGeneric *gen) {
                        const char *home = getenv("HOME");
                        if (home == nullptr || *home == 0) home = "/home";
                        const char *id = getenv("BMDS_APP_ID");
                        Text        name;
                        if (id != nullptr && *id != 0) {
                                name.append(id);
                        } else {
                                const char *path = scriptPath();
                                const char *base = path != nullptr ? strrchr(path, '/') : nullptr;
                                base = base != nullptr ? base + 1 : path != nullptr ? path : "";
                                const char *dot = strrchr(base, '.');
                                name.append(base, dot != nullptr && dot != base ? size_t(dot - base) : strlen(base));
                                if (name.length() == 0) name.append("angel");
                        }
                        Text dir;
                        dir.appendf("%s/.local/share/%s", home, name.data());
                        char     *path = dir.release();
                        const int e = path != nullptr ? makePath(path) : ENOMEM;
                        if (e != 0) raise("Can't make '%s': %s", path != nullptr ? path : "", strerror(e));
                        returnString(gen, path != nullptr ? path : "");
                        free(path);
                }

        } // namespace

        void registerServices(asIScriptEngine *engine) {
                findScriptFolder();

                check(engine->RegisterObjectType("Setting", 0, asOBJ_REF), "Setting");
                behaviour(engine, "Setting", asBEHAVE_FACTORY, "Setting@ f(const string &in key)", settingMake,
                          "A system setting by its key (wm/palette/desktop); exists says whether it's there.");
                behaviour(engine, "Setting", asBEHAVE_FACTORY,
                          "Setting@ f(const string &in key, const string &in type, const string &in defaultValue, "
                          "const string &in help = \"\")",
                          settingDeclare,
                          "Declares the script's own setting (key: owner/name; type: bool, int, int MIN MAX, string, "
                          "color, path, paths, secret, or an enum's words a|b|c), its saved value applied.");
                behaviour(engine, "Setting", asBEHAVE_ADDREF, "void f()", settingAddRef, nullptr);
                behaviour(engine, "Setting", asBEHAVE_RELEASE, "void f()", settingRelease, nullptr);
                method(engine, "Setting", "string get_key() const property", settingKey, "Its key.");
                method(engine, "Setting", "bool get_exists() const property", settingExists,
                       "There is such a setting.");
                method(engine, "Setting", "string get_value() const property", settingValue,
                       "Its value (\"\" if there's none).");
                method(engine, "Setting", "void set_value(const string &in value) property", settingSetValue,
                       "Changes it now (checked against its type; not saved).");
                method(engine, "Setting", "string get_defaultValue() const property", settingDefault, "Its default.");
                method(engine, "Setting", "string get_help() const property", settingHelp,
                       "One line saying what it is.");
                method(engine, "Setting", "string get_type() const property", settingType,
                       "bool, int, string, color, enum, path, paths or secret.");
                method(engine, "Setting", "string get_choices() const property", settingChoices,
                       "An enum's words, | between (\"\" for other types).");
                method(engine, "Setting", "bool get_isDefault() const property", settingIsDefault,
                       "Its value is the default.");
                method(engine, "Setting", "bool get_isSaved() const property", settingIsSaved,
                       "A value is saved for it.");
                method(engine, "Setting", "bool get_changed() const property", settingChanged,
                       "Its value isn't what a restart would give (changed, not saved).");
                method(engine, "Setting", "void save()", settingSave, "Saves its value: a restart keeps it.");
                method(engine, "Setting", "void revert()", settingRevert,
                       "Back to its saved value (none saved: its default).");
                method(engine, "Setting", "void reset()", settingReset, "Back to its default (not saved).");
                method(engine, "Setting", "void onChanged(StringCallback@ callback)", settingOnChanged,
                       "Calls back with its new value when it changes, while app.run() runs; null stops.");
                def(engine, "string[]@ settings(const string &in prefix = \"\")", settingsList,
                    "The settings' keys under a prefix (wm/palette), all with none.");

                check(engine->RegisterObjectType("System", 0, asOBJ_REF | asOBJ_NOCOUNT), "System");
                method(engine, "System", "string get_hostName() const property", systemHostName,
                       "This computer's name.");
                method(engine, "System", "string get_userName() const property", systemUserName, "The user's name.");
                method(engine, "System", "string get_board() const property", systemBoard, "The board's name.");
                method(engine, "System", "string get_version() const property", systemVersion, "The system's version.");
                method(engine, "System", "int get_cores() const property", systemCores, "CPU cores.");
                method(engine, "System", "int get_cpuMhz() const property", systemCpuMhz, "Their clock, in MHz.");
                method(engine, "System", "int64 get_memoryTotal() const property", systemMemoryTotal,
                       "Bytes of the memory programs use.");
                method(engine, "System", "int64 get_memoryFree() const property", systemMemoryFree,
                       "Bytes of it free now.");
                method(engine, "System", "int64 get_programMemory() const property", systemProgramMemory,
                       "Bytes this script's program holds.");
                method(engine, "System", "float get_cpuLoad() const property", systemCpuLoad,
                       "The cores' busy share since it was last asked (0 to 1; the first time since the start); -1 "
                       "when CPU time isn't counted.");
                method(engine, "System", "bool get_online() const property", systemOnline,
                       "The network is there: an interface up with its link and an address.");
                method(engine, "System", "void onNetworkChanged(Callback@ callback)", systemOnNetworkChanged,
                       "Calls back when the network changes (a link, an address), while app.run() runs; null stops.");
                property(engine, "System system", &systemObject,
                         "The system: its names, memory, CPU load and network.");

                check(engine->RegisterObjectType("DateTime", sizeof(DateTime),
                                                 asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS),
                      "DateTime");
                behaviour(engine, "DateTime", asBEHAVE_CONSTRUCT, "void f()", dateNow, "Now, in the program's zone.");
                behaviour(engine, "DateTime", asBEHAVE_CONSTRUCT, "void f(int64 seconds, const string &in zone = \"\")",
                          dateFromSeconds,
                          "A moment (seconds since 1970 UTC) in a zone (Europe/Berlin, or POSIX rules; \"\": the "
                          "program's).");
                behaviour(engine, "DateTime", asBEHAVE_CONSTRUCT,
                          "void f(int year, int month, int day, int hour = 0, int minute = 0, int second = 0, "
                          "const string &in zone = \"\")",
                          dateFromParts, "A date and time as it reads in a zone (\"\": the program's).");
                method(engine, "DateTime", "int get_year() const property", dateYear, "Its year.");
                method(engine, "DateTime", "int get_month() const property", dateMonth, "Its month, 1 to 12.");
                method(engine, "DateTime", "int get_day() const property", dateDay, "Its day of the month, 1 up.");
                method(engine, "DateTime", "int get_hour() const property", dateHour, "Its hour, 0 to 23.");
                method(engine, "DateTime", "int get_minute() const property", dateMinute, "Its minute.");
                method(engine, "DateTime", "int get_second() const property", dateSecond, "Its second.");
                method(engine, "DateTime", "int get_weekday() const property", dateWeekday,
                       "Its day of the week (0: Sunday).");
                method(engine, "DateTime", "int get_yearDay() const property", dateYearDay,
                       "Its day of the year (0: 1 January).");
                method(engine, "DateTime", "int64 get_seconds() const property", dateSeconds,
                       "Seconds since 1970 UTC.");
                method(engine, "DateTime", "int get_offset() const property", dateOffset,
                       "Its zone's offset: seconds east of UTC.");
                method(engine, "DateTime", "string get_zone() const property", dateZone,
                       "Its zone's abbreviation (CEST).");
                method(engine, "DateTime", "DateTime inZone(const string &in zone) const", dateInZone,
                       "The same moment in another zone.");
                method(engine, "DateTime", "string format(const string &in pattern = \"%Y-%m-%d %H:%M:%S\") const",
                       dateFormat, "As text: strftime's pattern (%A, %d %B %Y...).");
                method(engine, "DateTime", "string toString() const", dateToString,
                       "As print() shows it: 2026-10-25 15:26:15 PDT.");

                def(engine, "string appDir()", appDir,
                    "The application's folder; the script's own if it isn't in one.");
                def(engine, "string dataDir()", dataDir,
                    "The folder where the script keeps what it writes ($HOME/.local/share/NAME), made if needed.");
                return;
        }

        void releaseServices() {
                while (watchedCount != 0) unkeepSetting(watched[watchedCount - 1]);
                stopNetworkWatch();
                free(scriptFolder);
                scriptFolder = nullptr;
                return;
        }

} // namespace angel
