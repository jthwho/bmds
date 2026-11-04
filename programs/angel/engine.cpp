/**
 * @file      engine.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The script engine as every host makes it: the language's settings, each one explicit, since
 * they are part of the scripts' ABI (devplan/scripting.md §4), not left to the library's
 * defaults. angel and Preso's decks (devplan/preso.md §7.9) run the same language.
 */

#include "angel.h"

namespace angel {

        asIScriptEngine *createEngine(MessageCallback message, void *context, bool lineCues) {
                asIScriptEngine *engine = asCreateScriptEngine();
                if (engine == nullptr) return nullptr;
                if (message != nullptr) engine->SetMessageCallback(asFUNCTION(message), context, asCALL_CDECL);
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
                for (const Setting &s : settings) {
                        if (engine->SetEngineProperty(s.property, s.value) < 0) {
                                engine->ShutDownAndRelease();
                                return nullptr;
                        }
                }
                if (lineCues) engine->SetEngineProperty(asEP_BUILD_WITHOUT_LINE_CUES, 0);
                return engine;
        }

} // namespace angel
