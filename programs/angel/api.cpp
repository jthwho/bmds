/**
 * @file      api.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Registering the script API, and listing it: `angel --api` prints every declaration the
 * engine holds, sorted (the ABI lock, tests/scripts/api.lock, is checked against it);
 * `--api-help` adds each one's line of help (the documentation, the IDE's F1).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "angel.h"

namespace angel {

        namespace {

                struct Help {
                                char       *decl;
                                const char *help;
                };

                Help  *helps = nullptr;
                size_t helpCount = 0;
                size_t helpCapacity = 0;

                void remember(const char *decl, const char *help) {
                        if (help == nullptr || help[0] == 0) return;
                        if (helpCount == helpCapacity) {
                                const size_t capacity = helpCapacity != 0 ? helpCapacity * 2 : 128;
                                Help        *grown = static_cast<Help *>(realloc(helps, capacity * sizeof(Help)));
                                if (grown == nullptr) return;
                                helps = grown;
                                helpCapacity = capacity;
                        }
                        helps[helpCount].decl = strdup(decl);
                        helps[helpCount].help = help;
                        if (helps[helpCount].decl != nullptr) ++helpCount;
                        return;
                }

                void rememberFunction(asIScriptEngine *engine, int id, const char *help) {
                        asIScriptFunction *f = engine->GetFunctionById(id);
                        if (f != nullptr) remember(f->GetDeclaration(true, true, false), help);
                        return;
                }

                const char *helpOf(const char *decl) {
                        for (size_t i = 0; i < helpCount; ++i)
                                if (strcmp(helps[i].decl, decl) == 0) return helps[i].help;
                        return nullptr;
                }

                // The lines of the listing.
                struct Lines {
                                char **items = nullptr;
                                size_t count = 0;
                                size_t capacity = 0;

                                void add(const Text &line) {
                                        if (count == capacity) {
                                                const size_t grow = capacity != 0 ? capacity * 2 : 256;
                                                char       **grown =
                                                        static_cast<char **>(realloc(items, grow * sizeof(char *)));
                                                if (grown == nullptr) return;
                                                items = grown;
                                                capacity = grow;
                                        }
                                        char *copy = strdup(line.data());
                                        if (copy != nullptr) items[count++] = copy;
                                        return;
                                }
                };

                int compareLines(const void *a, const void *b) {
                        return strcmp(*static_cast<char *const *>(a), *static_cast<char *const *>(b));
                }

                void globalPropertyDecl(asIScriptEngine *engine, asUINT index, Text &out) {
                        const char *name = nullptr, *ns = nullptr;
                        int         typeId = 0;
                        bool        isConst = false;
                        engine->GetGlobalPropertyByIndex(index, &name, &ns, &typeId, &isConst);
                        out.clear();
                        if (isConst) out.append("const ");
                        out.append(engine->GetTypeDeclaration(typeId, true));
                        out.append(" ");
                        if (ns != nullptr && ns[0] != 0) out.appendf("%s::", ns);
                        out.append(name);
                        return;
                }

                void addFunction(Lines &lines, asIScriptFunction *f, bool help) {
                        if (f == nullptr) return;
                        Text        line;
                        const char *decl = f->GetDeclaration(true, true, false);
                        line.append(decl);
                        const char *h = help ? helpOf(decl) : nullptr;
                        if (h != nullptr) line.appendf("\t%s", h);
                        lines.add(line);
                        return;
                }

        } // namespace

        void check(int r, const char *what) {
                if (r >= 0) return;
                fprintf(stderr, "angel: registering %s failed (%d)\n", what, r);
                exit(70);
        }

        void def(asIScriptEngine *engine, const char *decl, asGENFUNC_t function, const char *help) {
                const int r = engine->RegisterGlobalFunction(decl, asFUNCTION(function), asCALL_GENERIC);
                check(r, decl);
                rememberFunction(engine, r, help);
                return;
        }

        void method(asIScriptEngine *engine, const char *type, const char *decl, asGENFUNC_t function,
                    const char *help) {
                const int r = engine->RegisterObjectMethod(type, decl, asFUNCTION(function), asCALL_GENERIC);
                check(r, decl);
                rememberFunction(engine, r, help);
                return;
        }

        void behaviour(asIScriptEngine *engine, const char *type, asEBehaviours behaviour, const char *decl,
                       asGENFUNC_t function, const char *help) {
                const int r =
                        engine->RegisterObjectBehaviour(type, behaviour, decl, asFUNCTION(function), asCALL_GENERIC);
                check(r, decl);
                rememberFunction(engine, r, help);
                return;
        }

        void property(asIScriptEngine *engine, const char *decl, void *address, const char *help) {
                const int r = engine->RegisterGlobalProperty(decl, address);
                check(r, decl);
                // Its declaration as the listing writes it (the engine's own spelling).
                const asUINT count = engine->GetGlobalPropertyCount();
                if (count != 0) {
                        Text line;
                        globalPropertyDecl(engine, count - 1, line);
                        remember(line.data(), help);
                }
                return;
        }

        void printApi(asIScriptEngine *engine, bool help) {
                Lines lines;
                Text  line;
                for (asUINT i = 0; i < engine->GetObjectTypeCount(); ++i) {
                        asITypeInfo *type = engine->GetObjectTypeByIndex(i);
                        line.clear();
                        line.appendf("%s %s", (type->GetFlags() & asOBJ_VALUE) != 0 ? "value" : "class",
                                     engine->GetTypeDeclaration(type->GetTypeId(), true));
                        if (type->GetSubTypeCount() != 0) {
                                line.clear();
                                line.appendf("%s %s<", (type->GetFlags() & asOBJ_VALUE) != 0 ? "value" : "class",
                                             type->GetName());
                                for (asUINT s = 0; s < type->GetSubTypeCount(); ++s)
                                        line.appendf("%s%s", s != 0 ? ", " : "", type->GetSubType(s)->GetName());
                                line.append(">");
                        }
                        lines.add(line);
                        for (asUINT m = 0; m < type->GetFactoryCount(); ++m)
                                addFunction(lines, type->GetFactoryByIndex(m), help);
                        for (asUINT m = 0; m < type->GetBehaviourCount(); ++m) {
                                asEBehaviours      kind;
                                asIScriptFunction *f = type->GetBehaviourByIndex(m, &kind);
                                // The reference counting and GC behaviours aren't the scripts' API.
                                if (kind == asBEHAVE_CONSTRUCT || kind == asBEHAVE_LIST_CONSTRUCT ||
                                    kind == asBEHAVE_LIST_FACTORY)
                                        addFunction(lines, f, help);
                        }
                        for (asUINT m = 0; m < type->GetMethodCount(); ++m)
                                addFunction(lines, type->GetMethodByIndex(m), help);
                        for (asUINT p = 0; p < type->GetPropertyCount(); ++p) {
                                line.clear();
                                line.appendf("%s::%s", type->GetName(), type->GetPropertyDeclaration(p, true));
                                lines.add(line);
                        }
                }
                for (asUINT i = 0; i < engine->GetEnumCount(); ++i) {
                        asITypeInfo *e = engine->GetEnumByIndex(i);
                        const char  *ns = e->GetNamespace();
                        Text         prefix;
                        if (ns != nullptr && ns[0] != 0) prefix.appendf("%s::", ns);
                        prefix.append(e->GetName());
                        line.clear();
                        line.appendf("enum %s", prefix.data());
                        lines.add(line);
                        for (asUINT v = 0; v < e->GetEnumValueCount(); ++v) {
                                int         value = 0;
                                const char *name = e->GetEnumValueByIndex(v, &value);
                                line.clear();
                                line.appendf("%s::%s = %d", prefix.data(), name, value);
                                lines.add(line);
                        }
                }
                for (asUINT i = 0; i < engine->GetFuncdefCount(); ++i) {
                        asITypeInfo *f = engine->GetFuncdefByIndex(i);
                        line.clear();
                        line.appendf("funcdef %s", f->GetFuncdefSignature()->GetDeclaration(true, true, false));
                        lines.add(line);
                }
                for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); ++i)
                        addFunction(lines, engine->GetGlobalFunctionByIndex(i), help);
                for (asUINT i = 0; i < engine->GetGlobalPropertyCount(); ++i) {
                        globalPropertyDecl(engine, i, line);
                        const char *h = help ? helpOf(line.data()) : nullptr;
                        if (h != nullptr) line.appendf("\t%s", h);
                        lines.add(line);
                }
                qsort(lines.items, lines.count, sizeof(char *), compareLines);
                for (size_t i = 0; i < lines.count; ++i) {
                        puts(lines.items[i]);
                        free(lines.items[i]);
                }
                free(lines.items);
                return;
        }

        namespace {

                // A field of a record: tabs and newlines would split it, so they are spaces.
                void field(const char *text) {
                        for (const char *p = text != nullptr ? text : ""; *p != 0; ++p)
                                putchar(*p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p);
                        return;
                }

                void record(const char *kind, const char *name, const char *decl, const char *help) {
                        fputs("api\t", stdout);
                        field(kind);
                        putchar('\t');
                        field(name);
                        putchar('\t');
                        field(decl);
                        putchar('\t');
                        field(help);
                        putchar('\n');
                        return;
                }

                void functionRecord(const char *kind, const char *owner, asIScriptFunction *f) {
                        if (f == nullptr) return;
                        Text        name;
                        const char *ns = f->GetNamespace();
                        if (owner != nullptr)
                                name.appendf("%s.", owner);
                        else if (ns != nullptr && ns[0] != 0)
                                name.appendf("%s::", ns);
                        name.append(f->GetName());
                        const char *decl = f->GetDeclaration(true, true, false);
                        record(kind, name.data(), decl, helpOf(decl));
                        return;
                }

        } // namespace

        void printApiRecords(asIScriptEngine *engine) {
                Text line;
                for (asUINT i = 0; i < engine->GetObjectTypeCount(); ++i) {
                        asITypeInfo *type = engine->GetObjectTypeByIndex(i);
                        line.clear();
                        line.appendf("class %s", engine->GetTypeDeclaration(type->GetTypeId(), true));
                        record("type", type->GetName(), line.data(), nullptr);
                        for (asUINT m = 0; m < type->GetFactoryCount(); ++m)
                                functionRecord("constructor", nullptr, type->GetFactoryByIndex(m));
                        for (asUINT m = 0; m < type->GetMethodCount(); ++m)
                                functionRecord("method", type->GetName(), type->GetMethodByIndex(m));
                        for (asUINT p = 0; p < type->GetPropertyCount(); ++p) {
                                const char *name = nullptr;
                                type->GetProperty(p, &name);
                                Text full;
                                full.appendf("%s.%s", type->GetName(), name != nullptr ? name : "");
                                line.clear();
                                line.appendf("%s::%s", type->GetName(), type->GetPropertyDeclaration(p, true));
                                record("property", full.data(), line.data(), nullptr);
                        }
                }
                for (asUINT i = 0; i < engine->GetEnumCount(); ++i) {
                        asITypeInfo *e = engine->GetEnumByIndex(i);
                        const char  *ns = e->GetNamespace();
                        Text         prefix;
                        if (ns != nullptr && ns[0] != 0) prefix.appendf("%s::", ns);
                        prefix.append(e->GetName());
                        line.clear();
                        line.appendf("enum %s", prefix.data());
                        record("enum", prefix.data(), line.data(), nullptr);
                        for (asUINT v = 0; v < e->GetEnumValueCount(); ++v) {
                                int         value = 0;
                                const char *name = e->GetEnumValueByIndex(v, &value);
                                Text        full, decl;
                                full.appendf("%s::%s", prefix.data(), name);
                                decl.appendf("%s = %d", full.data(), value);
                                record("constant", full.data(), decl.data(), nullptr);
                        }
                }
                for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); ++i)
                        functionRecord("function", nullptr, engine->GetGlobalFunctionByIndex(i));
                for (asUINT i = 0; i < engine->GetGlobalPropertyCount(); ++i) {
                        const char *name = nullptr, *ns = nullptr;
                        engine->GetGlobalPropertyByIndex(i, &name, &ns);
                        Text full;
                        if (ns != nullptr && ns[0] != 0) full.appendf("%s::", ns);
                        full.append(name != nullptr ? name : "");
                        globalPropertyDecl(engine, i, line);
                        record("variable", full.data(), line.data(), helpOf(line.data()));
                }
                return;
        }

} // namespace angel
