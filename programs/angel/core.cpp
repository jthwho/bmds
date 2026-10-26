/**
 * @file      core.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The core of the script API: print and input, math, random numbers, time, the command
 * line (`args`), the environment, exit, assert, and script exceptions (throw).
 */

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

namespace angel {

        namespace {

                int    argumentCount = 0;
                char **argumentValues = nullptr;

                CScriptArray *arguments = nullptr;
                int           stringTypeId = 0;
                const float   pi = 3.14159265358979323846f;
                int64_t       startMs = 0;

                int64_t monotonicMs() {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
                }

                // ---- Formatting values (print)

                // An object's own text: its `string toString() const` (or not const), run nested.
                bool callToString(asIScriptEngine *engine, Text &out, void *object, asITypeInfo *type) {
                        asIScriptFunction *f = type->GetMethodByDecl("string toString() const");
                        if (f == nullptr) f = type->GetMethodByDecl("string toString()");
                        if (f == nullptr) return false;
                        asIScriptContext *ctx = asGetActiveContext();
                        bool              nested = false;
                        if (ctx != nullptr && ctx->GetEngine() == engine && ctx->PushState() >= 0)
                                nested = true;
                        else
                                ctx = engine->RequestContext();
                        if (ctx == nullptr) return false;
                        bool done = false;
                        if (ctx->Prepare(f) >= 0 && ctx->SetObject(object) >= 0 &&
                            ctx->Execute() == asEXECUTION_FINISHED) {
                                const String *s = static_cast<const String *>(ctx->GetReturnObject());
                                if (s != nullptr) out.append(s->data(), s->length());
                                done = true;
                        }
                        if (nested) {
                                const asEContextState state = ctx->GetState();
                                ctx->PopState();
                                if (state == asEXECUTION_ABORTED) ctx->Abort();
                        } else
                                engine->ReturnContext(ctx);
                        return done;
                }

                void appendEnum(Text &out, asITypeInfo *type, int value) {
                        for (asUINT i = 0; i < type->GetEnumValueCount(); ++i) {
                                int         v = 0;
                                const char *name = type->GetEnumValueByIndex(i, &v);
                                if (v == value) {
                                        out.append(name);
                                        return;
                                }
                        }
                        out.appendf("%d", value);
                        return;
                }

        } // namespace

        void formatValue(asIScriptEngine *engine, Text &out, const void *address, int typeId, bool quoted) {
                if (stringTypeId == 0) stringTypeId = engine->GetTypeIdByDecl("string");
                if ((typeId & asTYPEID_OBJHANDLE) != 0) {
                        address = *static_cast<void *const *>(address);
                        if (address == nullptr) {
                                out.append("null");
                                return;
                        }
                        typeId &= ~asTYPEID_OBJHANDLE;
                }
                switch (typeId) {
                        case asTYPEID_VOID: return;
                        case asTYPEID_BOOL: out.append(*static_cast<const bool *>(address) ? "true" : "false"); return;
                        case asTYPEID_INT8: out.appendf("%d", *static_cast<const int8_t *>(address)); return;
                        case asTYPEID_INT16: out.appendf("%d", *static_cast<const int16_t *>(address)); return;
                        case asTYPEID_INT32: out.appendf("%ld", long(*static_cast<const int32_t *>(address))); return;
                        case asTYPEID_INT64:
                                out.appendf("%lld", (long long)*static_cast<const int64_t *>(address));
                                return;
                        case asTYPEID_UINT8: out.appendf("%u", *static_cast<const uint8_t *>(address)); return;
                        case asTYPEID_UINT16: out.appendf("%u", *static_cast<const uint16_t *>(address)); return;
                        case asTYPEID_UINT32:
                                out.appendf("%lu", (unsigned long)*static_cast<const uint32_t *>(address));
                                return;
                        case asTYPEID_UINT64:
                                out.appendf("%llu", (unsigned long long)*static_cast<const uint64_t *>(address));
                                return;
                        case asTYPEID_FLOAT: appendFloat(out, *static_cast<const float *>(address)); return;
                        case asTYPEID_DOUBLE: appendDouble(out, *static_cast<const double *>(address)); return;
                        default: break;
                }
                if (typeId == stringTypeId) {
                        const String *s = static_cast<const String *>(address);
                        if (quoted) out.append("\"");
                        out.append(s->data(), s->length());
                        if (quoted) out.append("\"");
                        return;
                }
                asITypeInfo *type = engine->GetTypeInfoById(typeId);
                if (type == nullptr) {
                        out.append("?");
                        return;
                }
                if ((type->GetFlags() & asOBJ_ENUM) != 0) {
                        appendEnum(out, type, *static_cast<const int *>(address));
                        return;
                }
                if (strcmp(type->GetName(), "array") == 0 && (type->GetFlags() & asOBJ_TEMPLATE) != 0) {
                        const CScriptArray *array = static_cast<const CScriptArray *>(address);
                        const int           element = array->GetElementTypeId();
                        out.append("[");
                        for (asUINT i = 0; i < array->GetSize(); ++i) {
                                if (i != 0) out.append(", ");
                                formatValue(engine, out, array->At(i), element, true);
                        }
                        out.append("]");
                        return;
                }
                if (callToString(engine, out, const_cast<void *>(address), type)) return;
                out.appendf("<%s>", type->GetName());
                return;
        }

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

                // ---- print and input

                void writeOut(const Text &text) {
                        fwrite(text.data(), 1, text.length(), stdout);
                        return;
                }
                void print(asIScriptGeneric *gen) {
                        Text text;
                        formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        writeOut(text);
                }
                void println(asIScriptGeneric *gen) {
                        Text text;
                        formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        text.append("\n");
                        writeOut(text);
                }
                void printNewline(asIScriptGeneric *) {
                        fputc('\n', stdout);
                }
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

                // ---- Math (float, double and int overloads)

                template <float (*F)(float), double (*D)(double)> void math1f(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(F(gen->GetArgFloat(0)));
                }
                template <float (*F)(float), double (*D)(double)> void math1d(asIScriptGeneric *gen) {
                        gen->SetReturnDouble(D(gen->GetArgDouble(0)));
                }
                void powf2(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(powf(gen->GetArgFloat(0), gen->GetArgFloat(1)));
                }
                void powd2(asIScriptGeneric *gen) {
                        gen->SetReturnDouble(pow(gen->GetArgDouble(0), gen->GetArgDouble(1)));
                }
                void atan2f2(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(atan2f(gen->GetArgFloat(0), gen->GetArgFloat(1)));
                }
                void atan2d2(asIScriptGeneric *gen) {
                        gen->SetReturnDouble(atan2(gen->GetArgDouble(0), gen->GetArgDouble(1)));
                }
                void absInt(asIScriptGeneric *gen) {
                        const int v = int(gen->GetArgDWord(0));
                        gen->SetReturnDWord(asDWORD(v < 0 ? -v : v));
                }
                void minInt(asIScriptGeneric *gen) {
                        const int a = int(gen->GetArgDWord(0)), b = int(gen->GetArgDWord(1));
                        gen->SetReturnDWord(asDWORD(a < b ? a : b));
                }
                void maxInt(asIScriptGeneric *gen) {
                        const int a = int(gen->GetArgDWord(0)), b = int(gen->GetArgDWord(1));
                        gen->SetReturnDWord(asDWORD(a > b ? a : b));
                }
                void clampInt(asIScriptGeneric *gen) {
                        const int v = int(gen->GetArgDWord(0)), lo = int(gen->GetArgDWord(1)),
                                  hi = int(gen->GetArgDWord(2));
                        gen->SetReturnDWord(asDWORD(v < lo ? lo : v > hi ? hi : v));
                }
                void minFloat(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(fminf(gen->GetArgFloat(0), gen->GetArgFloat(1)));
                }
                void maxFloat(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(fmaxf(gen->GetArgFloat(0), gen->GetArgFloat(1)));
                }
                void clampFloat(asIScriptGeneric *gen) {
                        const float v = gen->GetArgFloat(0), lo = gen->GetArgFloat(1), hi = gen->GetArgFloat(2);
                        gen->SetReturnFloat(v < lo ? lo : v > hi ? hi : v);
                }
                void minDouble(asIScriptGeneric *gen) {
                        gen->SetReturnDouble(fmin(gen->GetArgDouble(0), gen->GetArgDouble(1)));
                }
                void maxDouble(asIScriptGeneric *gen) {
                        gen->SetReturnDouble(fmax(gen->GetArgDouble(0), gen->GetArgDouble(1)));
                }
                void clampDouble(asIScriptGeneric *gen) {
                        const double v = gen->GetArgDouble(0), lo = gen->GetArgDouble(1), hi = gen->GetArgDouble(2);
                        gen->SetReturnDouble(v < lo ? lo : v > hi ? hi : v);
                }

                // ---- Random numbers: xoshiro128**, seeded from the system's random pool.

                uint32_t state[4];
                bool     seeded = false;

                uint32_t rotl(uint32_t x, int k) {
                        return (x << k) | (x >> (32 - k));
                }
                void seed(uint32_t value) {
                        // splitmix32 spreads one number over the state.
                        for (uint32_t &s : state) {
                                value += 0x9e3779b9u;
                                uint32_t z = value;
                                z = (z ^ (z >> 16)) * 0x85ebca6bu;
                                z = (z ^ (z >> 13)) * 0xc2b2ae35u;
                                s = z ^ (z >> 16);
                        }
                        seeded = true;
                        return;
                }
                uint32_t next() {
                        if (!seeded) {
                                uint32_t value = 0;
                                if (getrandom(&value, sizeof(value), 0) != sizeof(value))
                                        value = uint32_t(time(nullptr));
                                seed(value);
                        }
                        const uint32_t result = rotl(state[1] * 5, 7) * 9;
                        const uint32_t t = state[1] << 9;
                        state[2] ^= state[0];
                        state[3] ^= state[1];
                        state[1] ^= state[2];
                        state[0] ^= state[3];
                        state[2] ^= t;
                        state[3] = rotl(state[3], 11);
                        return result;
                }
                void randomInt(asIScriptGeneric *gen) {
                        int low = int(gen->GetArgDWord(0)), high = int(gen->GetArgDWord(1));
                        if (low > high) {
                                const int t = low;
                                low = high;
                                high = t;
                        }
                        const uint64_t span = uint64_t(int64_t(high) - int64_t(low)) + 1;
                        // Without the bias of a plain modulo: values past the last whole span drawn again.
                        const uint64_t limit = (uint64_t(1) << 32) - ((uint64_t(1) << 32) % span);
                        uint64_t       r;
                        do r = next();
                        while (r >= limit);
                        gen->SetReturnDWord(asDWORD(int64_t(low) + int64_t(r % span)));
                }
                void randomFloat(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(float(next() >> 8) * (1.0f / 16777216.0f));
                }
                void randomSeed(asIScriptGeneric *gen) {
                        seed(gen->GetArgDWord(0));
                }

                // ---- Time

                void millis(asIScriptGeneric *gen) {
                        gen->SetReturnQWord(asQWORD(monotonicMs() - startMs));
                }
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
                void assertTrue(asIScriptGeneric *gen) {
                        if (gen->GetArgByte(0) != 0) return;
                        const String *message = static_cast<const String *>(gen->GetArgAddress(1));
                        if (message->length() != 0)
                                raise("Assertion failed: %s", message->cStr());
                        else
                                raise("Assertion failed");
                }
                void throwException(asIScriptGeneric *gen) {
                        const String     *message = static_cast<const String *>(gen->GetArgAddress(0));
                        asIScriptContext *ctx = asGetActiveContext();
                        if (ctx != nullptr) ctx->SetException(message->cStr());
                }
                void exceptionInfo(asIScriptGeneric *gen) {
                        asIScriptContext *ctx = asGetActiveContext();
                        const char       *text = ctx != nullptr ? ctx->GetExceptionString() : nullptr;
                        new (gen->GetAddressOfReturnLocation()) String(text != nullptr ? text : "");
                }

        } // namespace

        void registerCore(asIScriptEngine *engine) {
                startMs = monotonicMs();

                def(engine, "void print(const ?&in)", print, "Shows a value (text, a number, an array...).");
                def(engine, "void println(const ?&in)", println, "Shows a value, then starts a new line.");
                def(engine, "void println()", printNewline, "Starts a new line.");
                def(engine, "string input()", input,
                    "Reads a line typed (without its Enter; \"\" at the end of the input).");
                def(engine, "string input(const string &in prompt)", inputPrompt,
                    "Shows the prompt, then reads a line.");

                property(engine, "const float PI", const_cast<float *>(&pi), "3.14159..., half a turn in radians.");
                def(engine, "float sqrt(float)", math1f<sqrtf, sqrt>, "The square root.");
                def(engine, "double sqrt(double)", math1d<sqrtf, sqrt>, "The square root.");
                def(engine, "float sin(float)", math1f<sinf, sin>, "The sine of an angle in radians.");
                def(engine, "double sin(double)", math1d<sinf, sin>, "The sine of an angle in radians.");
                def(engine, "float cos(float)", math1f<cosf, cos>, "The cosine of an angle in radians.");
                def(engine, "double cos(double)", math1d<cosf, cos>, "The cosine of an angle in radians.");
                def(engine, "float tan(float)", math1f<tanf, tan>, "The tangent of an angle in radians.");
                def(engine, "double tan(double)", math1d<tanf, tan>, "The tangent of an angle in radians.");
                def(engine, "float asin(float)", math1f<asinf, asin>, "The angle (radians) whose sine this is.");
                def(engine, "double asin(double)", math1d<asinf, asin>, "The angle (radians) whose sine this is.");
                def(engine, "float acos(float)", math1f<acosf, acos>, "The angle (radians) whose cosine this is.");
                def(engine, "double acos(double)", math1d<acosf, acos>, "The angle (radians) whose cosine this is.");
                def(engine, "float atan(float)", math1f<atanf, atan>, "The angle (radians) whose tangent this is.");
                def(engine, "double atan(double)", math1d<atanf, atan>, "The angle (radians) whose tangent this is.");
                def(engine, "float atan2(float y, float x)", atan2f2, "The angle (radians) of the point (x, y).");
                def(engine, "double atan2(double y, double x)", atan2d2, "The angle (radians) of the point (x, y).");
                def(engine, "float pow(float base, float exponent)", powf2, "base to the power of exponent.");
                def(engine, "double pow(double base, double exponent)", powd2, "base to the power of exponent.");
                def(engine, "float exp(float)", math1f<expf, exp>, "e to the power of the number.");
                def(engine, "double exp(double)", math1d<expf, exp>, "e to the power of the number.");
                def(engine, "float log(float)", math1f<logf, log>, "The natural logarithm.");
                def(engine, "double log(double)", math1d<logf, log>, "The natural logarithm.");
                def(engine, "float log10(float)", math1f<log10f, log10>, "The logarithm in base 10.");
                def(engine, "double log10(double)", math1d<log10f, log10>, "The logarithm in base 10.");
                def(engine, "float floor(float)", math1f<floorf, floor>, "Rounded down to a whole number.");
                def(engine, "double floor(double)", math1d<floorf, floor>, "Rounded down to a whole number.");
                def(engine, "float ceil(float)", math1f<ceilf, ceil>, "Rounded up to a whole number.");
                def(engine, "double ceil(double)", math1d<ceilf, ceil>, "Rounded up to a whole number.");
                def(engine, "float round(float)", math1f<roundf, round>, "Rounded to the nearest whole number.");
                def(engine, "double round(double)", math1d<roundf, round>, "Rounded to the nearest whole number.");
                def(engine, "float abs(float)", math1f<fabsf, fabs>, "Without its sign.");
                def(engine, "double abs(double)", math1d<fabsf, fabs>, "Without its sign.");
                def(engine, "int abs(int)", absInt, "Without its sign.");
                def(engine, "int min(int, int)", minInt, "The smaller.");
                def(engine, "int max(int, int)", maxInt, "The larger.");
                def(engine, "int clamp(int value, int low, int high)", clampInt,
                    "The value kept between low and high.");
                def(engine, "float min(float, float)", minFloat, "The smaller.");
                def(engine, "float max(float, float)", maxFloat, "The larger.");
                def(engine, "float clamp(float value, float low, float high)", clampFloat,
                    "The value kept between low and high.");
                def(engine, "double min(double, double)", minDouble, "The smaller.");
                def(engine, "double max(double, double)", maxDouble, "The larger.");
                def(engine, "double clamp(double value, double low, double high)", clampDouble,
                    "The value kept between low and high.");

                def(engine, "int random(int low, int high)", randomInt,
                    "A whole number from low to high, both included.");
                def(engine, "float random()", randomFloat, "A number from 0 up to (not including) 1.");
                def(engine, "void randomSeed(uint)", randomSeed,
                    "Starts the random numbers from a seed: the same seed, the same numbers.");

                def(engine, "uint64 millis()", millis, "Milliseconds since the script started.");
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
                def(engine, "void assert(bool condition, const string &in message = \"\")", assertTrue,
                    "Stops the script with an error if the condition isn't true.");
                def(engine, "void throw(const string &in message)", throwException,
                    "Raises an exception: a catch block gets it, or the script stops with the message.");
                def(engine, "string getExceptionInfo()", exceptionInfo, "In a catch block: the exception's message.");
                return;
        }

} // namespace angel
