/**
 * @file      fonts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's fonts for scripts (devplan/fonts.md §4f): `Font`, opened by name, by family,
 * size and style, or by role, its metrics and measuring; `fontFamilies()`. Screen, Image and
 * Canvas draw with one (graphics.cpp, fontHandle()). A program has 16 fonts open at most, so
 * fonts that come out the same (the same name back from the system) share one object.
 */

#include <new>
#include <stdlib.h>
#include <string.h>
#include <bmds/game.h>
#include <bmds/sys.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

namespace angel {

        namespace {

                class Font {
                        public:
                                int32_t          handle = -1;
                                bmds_font_info_t info{};
                                char             family[64] = "";
                                char             style[32] = "";

                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }

                                // The open font named the same as @p name back from the system, with a
                                // reference added; or nullptr.
                                static Font *find(const char *name) {
                                        for (Font *f = _first; f != nullptr; f = f->_next)
                                                if (strcmp(f->info.name, name) == 0) {
                                                        f->addRef();
                                                        return f;
                                                }
                                        return nullptr;
                                }

                                // A new one for @p font's handle (taken), its info read; or nullptr.
                                static Font *make(int32_t font) {
                                        Font *f = new (std::nothrow) Font();
                                        if (f == nullptr) {
                                                bmds_font_close(font);
                                                return nullptr;
                                        }
                                        f->handle = font;
                                        f->info.size = sizeof(f->info);
                                        (void)bmds_font_info(font, &f->info);
                                        bmds_font_face_t face;
                                        memset(&face, 0, sizeof(face));
                                        face.size = sizeof(face);
                                        if (bmds_font_list(f->info.face, &face) == 0) {
                                                memcpy(f->family, face.family, sizeof(f->family));
                                                memcpy(f->style, face.style_name, sizeof(f->style));
                                        }
                                        f->_next = _first;
                                        _first = f;
                                        return f;
                                }

                        private:
                                ~Font() {
                                        for (Font **at = &_first; *at != nullptr; at = &(*at)->_next)
                                                if (*at == this) {
                                                        *at = _next;
                                                        break;
                                                }
                                        bmds_font_close(handle);
                                }

                                static Font *_first;
                                Font        *_next = nullptr;
                                int          _refs = 1;
                };
                Font *Font::_first = nullptr;

                const String &argString(asIScriptGeneric *gen, asUINT n) {
                        return *static_cast<const String *>(gen->GetArgAddress(n));
                }

                Font *self(asIScriptGeneric *gen) {
                        return static_cast<Font *>(gen->GetObject());
                }

                // The font @p spec names, shared with an open one that came out the same; or nullptr
                // with an exception (@p what names it in the message).
                Font *open(const bmds_font_spec_t &spec, const char *what) {
                        const int32_t font = bmds_font_open(&spec);
                        if (font < 0) {
                                if (font == -BMDS_ERANGE)
                                        raise("No font '%s': its size must be 4 to 255 pixels", what);
                                else if (font == -BMDS_EMFILE)
                                        raise("Can't open font '%s': a script has 16 fonts open at most", what);
                                else if (font == -BMDS_ENOMEM)
                                        raise("Out of memory");
                                else
                                        raise("No font '%s' (a family, a size, bold, italic: \"IBM Plex Sans 14 "
                                              "bold\"; or a role: ui, mono, title, terminal)",
                                              what);
                                return nullptr;
                        }
                        bmds_font_info_t info;
                        memset(&info, 0, sizeof(info));
                        info.size = sizeof(info);
                        if (bmds_font_info(font, &info) == 0) {
                                Font *same = Font::find(info.name);
                                if (same != nullptr) {
                                        bmds_font_close(font);
                                        return same;
                                }
                        }
                        Font *f = Font::make(font);
                        if (f == nullptr) raise("Out of memory");
                        return f;
                }

                void fontByName(asIScriptGeneric *gen) {
                        const String    &name = argString(gen, 0);
                        bmds_font_spec_t spec;
                        memset(&spec, 0, sizeof(spec));
                        spec.size = sizeof(spec);
                        spec.name = name.cStr();
                        gen->SetReturnAddress(open(spec, name.cStr()));
                }

                void fontByFamily(asIScriptGeneric *gen) {
                        const String &family = argString(gen, 0);
                        const int     size = int(gen->GetArgDWord(1));
                        const int     style = int(gen->GetArgDWord(2));
                        if (style < 0 || style > int(BMDS_FONT_BOLD | BMDS_FONT_ITALIC)) {
                                raise("No font style %d (FontStyle: Regular, Bold, Italic, Bold | Italic)", style);
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        Text what;
                        what.appendf("%s %d", family.cStr(), size);
                        if (size < 1) {
                                raise("No font '%s': its size must be 4 to 255 pixels", what.data());
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        bmds_font_spec_t spec;
                        memset(&spec, 0, sizeof(spec));
                        spec.size = sizeof(spec);
                        spec.family = family.cStr();
                        spec.pixel_size = uint32_t(size);
                        spec.style = uint32_t(style);
                        gen->SetReturnAddress(open(spec, what.data()));
                }

                void fontSized(asIScriptGeneric *gen) {
                        const Font *f = self(gen);
                        const int   size = int(gen->GetArgDWord(0));
                        Text        what;
                        what.appendf("%s %d", f->family, size);
                        if (size < 1) {
                                raise("No font '%s': its size must be 4 to 255 pixels", what.data());
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        bmds_font_spec_t spec;
                        memset(&spec, 0, sizeof(spec));
                        spec.size = sizeof(spec);
                        spec.family = f->family;
                        spec.pixel_size = uint32_t(size);
                        spec.style = f->info.style & uint32_t(BMDS_FONT_BOLD | BMDS_FONT_ITALIC);
                        gen->SetReturnAddress(open(spec, what.data()));
                }

                void fontAddRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }
                void fontRelease(asIScriptGeneric *gen) {
                        self(gen)->release();
                }

                template <size_t N> void returnText(asIScriptGeneric *gen, const char (&text)[N]) {
                        new (gen->GetAddressOfReturnLocation()) String(text, strnlen(text, N));
                }
                void fontName(asIScriptGeneric *gen) {
                        returnText(gen, self(gen)->info.name);
                }
                void fontFamily(asIScriptGeneric *gen) {
                        returnText(gen, self(gen)->family);
                }
                void fontStyle(asIScriptGeneric *gen) {
                        returnText(gen, self(gen)->style);
                }
                void fontSize(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->info.pixel_size);
                }
                void fontAscent(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self(gen)->info.ascent));
                }
                void fontDescent(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self(gen)->info.descent));
                }
                void fontLineHeight(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self(gen)->info.line_height));
                }
                void fontMaxAdvance(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(self(gen)->info.max_advance));
                }
                void fontBold(asIScriptGeneric *gen) {
                        const Font *f = self(gen);
                        gen->SetReturnByte((f->info.style & BMDS_FONT_BOLD) != 0 ||
                                           (f->info.flags & BMDS_FONT_MADE_UP_BOLD) != 0);
                }
                void fontItalic(asIScriptGeneric *gen) {
                        gen->SetReturnByte((self(gen)->info.style & BMDS_FONT_ITALIC) != 0);
                }
                void fontBitmap(asIScriptGeneric *gen) {
                        gen->SetReturnByte((self(gen)->info.flags & BMDS_FONT_BITMAP) != 0);
                }
                void fontMonospace(asIScriptGeneric *gen) {
                        gen->SetReturnByte((self(gen)->info.flags & BMDS_FONT_FIXED) != 0);
                }

                void fontWidth(asIScriptGeneric *gen) {
                        Text t;
                        formatValue(gen->GetEngine(), t, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        gen->SetReturnDWord(asDWORD(bmds_game_text_width(self(gen)->handle, t.data())));
                }

                void fontFit(asIScriptGeneric *gen) {
                        const String &text = argString(gen, 0);
                        const int     max = int(gen->GetArgDWord(1));
                        if (max <= 0 || text.length() == 0) {
                                gen->SetReturnDWord(0);
                                return;
                        }
                        bmds_text_extent_t e;
                        memset(&e, 0, sizeof(e));
                        e.size = sizeof(e);
                        const int32_t r = bmds_text_measure(self(gen)->handle, text.cStr(), uint32_t(text.length()),
                                                            uint32_t(max), &e);
                        gen->SetReturnDWord(r < 0 ? 0 : e.bytes);
                }

                int compareNames(const void *a, const void *b) {
                        return strcmp(*static_cast<char *const *>(a), *static_cast<char *const *>(b));
                }

                void fontFamilies(asIScriptGeneric *gen) {
                        char           **names = nullptr;
                        size_t           count = 0, capacity = 0;
                        bmds_font_face_t face;
                        for (uint32_t i = 0;; ++i) {
                                memset(&face, 0, sizeof(face));
                                face.size = sizeof(face);
                                if (bmds_font_list(i, &face) < 0) break;
                                bool seen = false;
                                for (size_t j = 0; j < count && !seen; ++j) seen = strcmp(names[j], face.family) == 0;
                                if (seen) continue;
                                if (count == capacity) {
                                        capacity = capacity != 0 ? capacity * 2 : 16;
                                        char **grown = static_cast<char **>(realloc(names, capacity * sizeof(char *)));
                                        if (grown == nullptr) break;
                                        names = grown;
                                }
                                names[count] = strdup(face.family);
                                if (names[count] != nullptr) ++count;
                        }
                        qsort(names, count, sizeof(char *), compareNames);
                        CScriptArray *array = CScriptArray::Create(stringArrayType(gen->GetEngine()));
                        for (size_t i = 0; i < count; ++i) {
                                if (array != nullptr) {
                                        String name(names[i]);
                                        array->InsertLast(&name);
                                }
                                free(names[i]);
                        }
                        free(names);
                        if (array == nullptr) raise("Out of memory");
                        gen->SetReturnAddress(array);
                }

        } // namespace

        int32_t fontHandle(const void *font) {
                return font != nullptr ? static_cast<const Font *>(font)->handle : BMDS_GAME_FONT_DEFAULT;
        }

        void releaseFont(void *font) {
                if (font != nullptr) static_cast<Font *>(font)->release();
                return;
        }

        const char *fontName(const void *font) {
                return static_cast<const Font *>(font)->info.name;
        }

        void *fontNamed(const char *name) {
                bmds_font_spec_t spec;
                memset(&spec, 0, sizeof(spec));
                spec.size = sizeof(spec);
                spec.name = name;
                return open(spec, name);
        }

        void registerFonts(asIScriptEngine *engine) {
                check(engine->RegisterEnum("FontStyle"), "FontStyle");
                check(engine->RegisterEnumValue("FontStyle", "Regular", 0), "FontStyle::Regular");
                check(engine->RegisterEnumValue("FontStyle", "Bold", BMDS_FONT_BOLD), "FontStyle::Bold");
                check(engine->RegisterEnumValue("FontStyle", "Italic", BMDS_FONT_ITALIC), "FontStyle::Italic");
                check(engine->RegisterEnum("TextSmoothing"), "TextSmoothing");
                check(engine->RegisterEnumValue("TextSmoothing", "Default", 0), "TextSmoothing::Default");
                check(engine->RegisterEnumValue("TextSmoothing", "Smooth", BMDS_TEXT_SMOOTH), "TextSmoothing::Smooth");
                check(engine->RegisterEnumValue("TextSmoothing", "Sharp", BMDS_TEXT_SHARP), "TextSmoothing::Sharp");

                const char *F = "Font";
                check(engine->RegisterObjectType(F, 0, asOBJ_REF), F);
                behaviour(engine, F, asBEHAVE_FACTORY, "Font@ f(const string &in name)", fontByName,
                          "A system font by name: a family, a size, bold, italic (\"IBM Plex Sans 14 bold\", "
                          "\"unscii 32\"), or a role, the desktop's own: ui, mono, title, terminal (\"ui\", "
                          "\"mono 20\"). A family the system hasn't got gives its nearest.");
                behaviour(engine, F, asBEHAVE_FACTORY, "Font@ f(const string &in family, int size, int style = 0)",
                          fontByFamily,
                          "A system font from its family, its size in pixels and its style (FontStyle: Bold, "
                          "Italic, Bold | Italic).");
                behaviour(engine, F, asBEHAVE_ADDREF, "void f()", fontAddRef, nullptr);
                behaviour(engine, F, asBEHAVE_RELEASE, "void f()", fontRelease, nullptr);
                method(engine, F, "string get_name() const property", fontName,
                       "What it is, as a name for Font(): \"IBM Plex Sans 14 bold\".");
                method(engine, F, "string get_family() const property", fontFamily,
                       "Its family (\"IBM Plex Sans\"): the one asked for, or the nearest the system has.");
                method(engine, F, "string get_style() const property", fontStyle,
                       "Its face's style name (\"Bold Italic\").");
                method(engine, F, "int get_size() const property", fontSize, "Its size in pixels.");
                method(engine, F, "Font@ sized(int pixels) const", fontSized,
                       "The same family and style at another size in pixels.");
                method(engine, F, "int get_ascent() const property", fontAscent,
                       "Pixels from a line's top to its baseline.");
                method(engine, F, "int get_descent() const property", fontDescent, "Pixels below the baseline.");
                method(engine, F, "int get_lineHeight() const property", fontLineHeight,
                       "Pixels from one line's top to the next's.");
                method(engine, F, "int get_maxAdvance() const property", fontMaxAdvance,
                       "The widest letter's width in pixels.");
                method(engine, F, "bool get_bold() const property", fontBold, "It is bold.");
                method(engine, F, "bool get_italic() const property", fontItalic, "It is italic.");
                method(engine, F, "bool get_bitmap() const property", fontBitmap,
                       "A bitmap font (unscii): sharp, at multiples of its size.");
                method(engine, F, "bool get_monospace() const property", fontMonospace,
                       "Every letter is as wide as the others.");
                method(engine, F, "int width(const ?&in value) const", fontWidth,
                       "How wide text() writes a value (text, a number...) in this font, in pixels: its widest "
                       "line.");
                method(engine, F, "int fit(const string &in text, int maxWidth) const", fontFit,
                       "How many bytes of text's first line fit in maxWidth pixels (whole letters).");
                def(engine, "array<string>@ fontFamilies()", fontFamilies,
                    "The font families the system has, sorted (for Font()).");
                return;
        }

} // namespace angel
