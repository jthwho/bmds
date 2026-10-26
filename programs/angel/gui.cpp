/**
 * @file      gui.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The GUI for scripts (devplan/guiprograms.md §13): every class of the GUI service's schema
 * (sdk/gui/classes.def, included here as the service includes it) a script type, its properties
 * as properties, its methods as methods and its events as `onName(callback)`; `app`, the
 * connection (opened at the first object), with its loop and timers. The bindings are built from
 * the schema when angel is, so `angel --api` lists them with or without a display.
 *
 * Every object a script makes is kept here until it is destroyed (by `destroy()`, its parent's,
 * or the script's end), not until the script lets go of it: a button made in a function stays in
 * its window. Callbacks run nested in the script's context, as a `Game`'s methods do; after the
 * script's end, in a context of their own.
 *
 * An image property (`iconImage`) takes angel's own `Image`: its pixels are sent as a GUI image
 * the first time it is used, and again when it has been drawn on since. The one-line dialogs
 * (`alert()`, `ask()`, `askText()`, `chooseFile()`, `chooseSaveFile()`, `chooseColor()`) wait for
 * their answer, serving the GUI's other events and the timers meanwhile, the script's window blocked.
 *
 * A `Canvas` has a `Screen`'s drawing methods (graphics.cpp's), drawn straight into a surface of the
 * service's and shown by the loop before it waits. The loop also serves the events of screens
 * given an `onEvent` callback, beside the GUI's.
 */

#include <errno.h>
#include <new>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <bmds/gui.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/sys.h>
#endif

namespace angel {

        namespace {

                // ---- The schema (classes.def), as tables

                namespace gt {

                        enum Tag : uint8_t {
                                None,
                                Int,
                                Long,
                                Bool,
                                String,
                                Atom,
                                Id,
                                Rect,
                                Color,
                                List,
                                Bytes,
                                Picture // not the schema's: an Id naming an image (angel's Image)
                        };

                        struct ClassDef {
                                        const char *name;
                                        const char *parent; // "None": none
                                        bool        concrete;
                        };
                        struct PropDef {
                                        const char *cls;
                                        const char *name;
                                        Tag         tag;
                                        bool        readable;
                                        bool        writable;
                        };
                        struct MethodDef {
                                        const char *cls;
                                        const char *name;
                                        Tag         result;
                                        Tag         args[6];
                                        uint8_t     count;
                        };
                        struct EventDef {
                                        const char *cls;
                                        const char *name;
                                        bool        byDefault; // sent unless turned off
                                        Tag         values[6];
                                        uint8_t     count;
                        };

                        template <typename... T> constexpr uint8_t countOf(T...) {
                                return uint8_t(sizeof...(T));
                        }

#define ANGEL_KIND_Concrete true
#define ANGEL_KIND_Abstract false
#define ANGEL_READ_RW true
#define ANGEL_READ_RO true
#define ANGEL_READ_WO false
#define ANGEL_WRITE_RW true
#define ANGEL_WRITE_RO false
#define ANGEL_WRITE_WO true
#define ANGEL_ON_On true
#define ANGEL_ON_Off false

#define GUI_CLASS(C, P, K) {#C, #P, ANGEL_KIND_##K},
#define GUI_PROP(C, N, T, A)
#define GUI_VALUES(C, N, ...)
#define GUI_METHOD(C, N, R, ...)
#define GUI_EVENT(C, N, D, ...)
                        constexpr ClassDef Classes[] = {
#include "../../sdk/gui/classes.def"
                        };
#undef GUI_CLASS
#undef GUI_PROP
#define GUI_CLASS(C, P, K)
#define GUI_PROP(C, N, T, A) {#C, #N, T, ANGEL_READ_##A, ANGEL_WRITE_##A},
                        constexpr PropDef Props[] = {
#include "../../sdk/gui/classes.def"
                        };
#undef GUI_PROP
#undef GUI_METHOD
#define GUI_PROP(C, N, T, A)
#define GUI_METHOD(C, N, R, ...) {#C, #N, R, {__VA_ARGS__}, countOf(__VA_ARGS__)},
                        constexpr MethodDef Methods[] = {
#include "../../sdk/gui/classes.def"
                        };
#undef GUI_METHOD
#undef GUI_EVENT
#define GUI_METHOD(C, N, R, ...)
#define GUI_EVENT(C, N, D, ...) {#C, #N, ANGEL_ON_##D, {__VA_ARGS__}, countOf(__VA_ARGS__)},
                        constexpr EventDef Events[] = {
#include "../../sdk/gui/classes.def"
                        };
#undef GUI_CLASS
#undef GUI_PROP
#undef GUI_VALUES
#undef GUI_METHOD
#undef GUI_EVENT

                } // namespace gt

                using gt::Tag;

                const gt::ClassDef *classNamed(const char *name) {
                        for (const gt::ClassDef &c : gt::Classes)
                                if (strcmp(c.name, name) == 0) return &c;
                        return nullptr;
                }
                const gt::ClassDef *parentOf(const gt::ClassDef *c) {
                        return c != nullptr && strcmp(c->parent, "None") != 0 ? classNamed(c->parent) : nullptr;
                }
                // angel's own Image is the picture a GUI image is made from (5b): not bound as a class.
                bool bound(const gt::ClassDef &c) {
                        return strcmp(c.name, "Image") != 0;
                }
                // An Id member that names an image (`iconImage`, `setIconImage`): angel's Image.
                bool imageMember(const char *member) {
                        return strstr(member, "mage") != nullptr;
                }
                Tag effective(Tag tag, const char *member) {
                        return tag == gt::Id && imageMember(member) ? gt::Picture : tag;
                }
                // An Id member's script type: an image's, a window's owner, else any GUI object.
                const char *idType(const char *member) {
                        if (imageMember(member)) return "Image@";
                        if (strcmp(member, "owner") == 0) return "Window@";
                        return "GuiObject@";
                }

                // ---- The script's side of a rectangle

                struct ScriptRect {
                                int32_t x = 0, y = 0, width = 0, height = 0;
                };

                // ---- Objects

                class GuiRef;

                struct Handler {
                                const gt::EventDef *event = nullptr;
                                uint32_t            atom = 0;
                                asIScriptFunction  *fn = nullptr;
                };

                class GuiRef {
                        public:
                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                }

                                uint32_t            id = 0;
                                uint32_t            parent = 0;
                                const gt::ClassDef *cls = nullptr;
                                bool                alive = true;
                                bool                shown = false; // a window's, as last seen
                                uint32_t            shownAt = 0;   // when it was last shown (a count)
                                Handler            *handlers = nullptr;
                                uint32_t            handlerCount = 0;
                                void               *picture = nullptr; // a Canvas's, once drawn on (graphics.cpp's)

                                // Its callbacks let go (destroyed, or the runtime's end).
                                void dropHandlers() {
                                        for (uint32_t i = 0; i < handlerCount; ++i)
                                                if (handlers[i].fn != nullptr) handlers[i].fn->Release();
                                        free(handlers);
                                        handlers = nullptr;
                                        handlerCount = 0;
                                        return;
                                }

                        private:
                                ~GuiRef() { dropHandlers(); }
                                int _refs = 1; // the registry's
                };

                bmds_gui_t       *gui = nullptr;
                bool              noGui = false; // the connection was tried and refused
                asIScriptEngine  *theEngine = nullptr;
                asIScriptContext *ownContext = nullptr; // callbacks after the script's end
                GuiRef          **objects = nullptr;    // every live object, the registry's reference each
                size_t            objectCount = 0;
                size_t            objectCapacity = 0;
                bool              quitting = false;
                bool              looping = false;
                bool              quitCalled = false;   // app.quit() since run() last started
                uint32_t          showCount = 0;        // windows' show()s, to find the last
                char             *lastFolder = nullptr; // where the file dialogs were last

                GuiRef *find(uint32_t id) {
                        for (size_t i = 0; i < objectCount; ++i)
                                if (objects[i]->id == id) return objects[i];
                        return nullptr;
                }

                void keep(GuiRef *o) {
                        if (objectCount == objectCapacity) {
                                const size_t capacity = objectCapacity != 0 ? objectCapacity * 2 : 64;
                                auto **grown = static_cast<GuiRef **>(realloc(objects, capacity * sizeof(GuiRef *)));
                                if (grown == nullptr) {
                                        raise("Out of memory for GUI objects");
                                        return;
                                }
                                objects = grown;
                                objectCapacity = capacity;
                        }
                        objects[objectCount++] = o;
                        return;
                }

                // Object @p id and its children gone (the service destroys children with their parent).
                void forget(uint32_t id) {
                        for (size_t i = objectCount; i-- > 0;) {
                                if (i >= objectCount || objects[i]->parent != id) continue;
                                forget(objects[i]->id);
                        }
                        for (size_t i = 0; i < objectCount; ++i) {
                                if (objects[i]->id != id) continue;
                                GuiRef *o = objects[i];
                                objects[i] = objects[--objectCount];
                                o->alive = false;
                                o->shown = false;
                                o->dropHandlers();
                                if (o->picture != nullptr) canvasRelease(o->picture); // its surface closed
                                o->picture = nullptr;
                                o->release();
                                break;
                        }
                        return;
                }

                bool anyShown() {
                        for (size_t i = 0; i < objectCount; ++i)
                                if (objects[i]->shown) return true;
                        return false;
                }

                // ---- Requests remembered, so the service's errors name the script line that made them

                struct Made {
                                uint32_t    sequence = 0;
                                int         line = 0;
                                const char *section = nullptr;
                                const char *cls = nullptr;
                                const char *what = nullptr;
                };
                constexpr size_t RequestRing = 128;
                Made             made[RequestRing];
                size_t           madeNext = 0;

                // Request @p what of @p cls: the one just made, or (@p next) the next, for a request that
                // waits for its answer (its error may come while it waits).
                void remember(const char *cls, const char *what, bool next = false) {
                        Made &m = made[madeNext++ % RequestRing];
                        m.sequence = bmds_gui_sequence(gui) + (next ? 1 : 0);
                        m.cls = cls;
                        m.what = what;
                        m.line = 0;
                        m.section = nullptr;
                        if (asIScriptContext *ctx = asGetActiveContext(); ctx != nullptr)
                                m.line = ctx->GetLineNumber(0, nullptr, &m.section);
                        return;
                }

                void onError(void *, const bmds_gui_error_t *e) {
                        fflush(stdout);
                        for (const Made &m : made) {
                                if (m.what == nullptr || m.sequence != e->sequence) continue;
                                fprintf(stderr, "%s:%d: GUI error: %s.%s: %s\n", m.section != nullptr ? m.section : "?",
                                        m.line, m.cls, m.what, bmds_gui_error_name(e->code));
                                return;
                        }
                        fprintf(stderr, "angel: GUI error: request %u: %s\n", unsigned(e->sequence),
                                bmds_gui_error_name(e->code));
                        return;
                }

                // The connection, opened at the first object; null (an exception raised) if there is none.
                bmds_gui_t *connection() {
                        if (gui != nullptr) return gui;
                        static int why = 0; // the first attempt's errno (it isn't made again)
                        if (!noGui) {
                                gui = bmds_gui_connect(nullptr);
                                if (gui != nullptr) bmds_gui_on_error(gui, onError, nullptr);
                                noGui = gui == nullptr;
                                why = gui == nullptr ? errno : 0;
                        }
                        if (gui != nullptr) return gui;
                        // Another machine named (BMDS_GUI_DISPLAY): why it couldn't be reached.
                        const char *display = getenv("BMDS_GUI_DISPLAY");
                        if (display != nullptr && display[0] != 0 && strcmp(display, "local") != 0)
                                raise("Can't reach the GUI at %s: %s", display,
                                      why == EACCES ? "refused: no cookie, or not its (BMDS_GUI_COOKIE)"
                                                    : strerror(why));
                        else
                                raise("No GUI here: the window system isn't running");
                        return gui;
                }

                // ---- Images: angel's Image as a GUI image, made at its first use and sent again when it
                // was drawn on since, kept until the runtime's end (a reference to the Image each)

                struct Icon {
                                void    *image = nullptr;
                                uint32_t id = 0;
                                uint32_t generation = 0;
                                int      width = 0; // as sent (0: not yet)
                                int      height = 0;
                };
                Icon  *icons = nullptr;
                size_t iconCount = 0;
                size_t iconCapacity = 0;

                constexpr int MaxIcon = 512; // the protocol's most, each way

                // The GUI image showing @p image, its pixels up to date; 0 (an exception raised) if none.
                uint32_t iconOf(void *image) {
                        const ImageInfo info = imageInfo(image);
                        if (info.width > MaxIcon || info.height > MaxIcon) {
                                raise("An icon is at most %d pixels each way, not %d x %d", MaxIcon, info.width,
                                      info.height);
                                return 0;
                        }
                        Icon *icon = nullptr;
                        for (size_t i = 0; i < iconCount && icon == nullptr; ++i)
                                if (icons[i].image == image) icon = &icons[i];
                        if (icon == nullptr) {
                                if (iconCount == iconCapacity) {
                                        const size_t capacity = iconCapacity != 0 ? iconCapacity * 2 : 8;
                                        auto *grown = static_cast<Icon *>(realloc(icons, capacity * sizeof(Icon)));
                                        if (grown == nullptr) {
                                                raise("Out of memory for an icon");
                                                return 0;
                                        }
                                        icons = grown;
                                        iconCapacity = capacity;
                                }
                                const uint32_t id = bmds_gui_new(gui, "Image", 0);
                                remember("Image", "new");
                                if (id == 0) {
                                        raise("Can't make an icon");
                                        return 0;
                                }
                                imageAddRef(image);
                                icon = &icons[iconCount++];
                                *icon = Icon{image, id, 0, 0, 0};
                        }
                        if (icon->width != info.width || icon->height != info.height ||
                            icon->generation != info.generation) {
                                auto *argb = static_cast<uint32_t *>(
                                        malloc(size_t(info.width) * size_t(info.height) * sizeof(uint32_t)));
                                if (argb == nullptr) {
                                        raise("Out of memory for a %d x %d icon", info.width, info.height);
                                        return 0;
                                }
                                imageArgb(image, argb);
                                (void)bmds_gui_put_image(gui, icon->id, uint32_t(info.width), uint32_t(info.height),
                                                         argb);
                                remember("Image", "pixels");
                                free(argb);
                                icon->width = info.width;
                                icon->height = info.height;
                                icon->generation = info.generation;
                        }
                        return icon->id;
                }

                // ---- What each registration does: one for each member of the schema, shared by the
                // classes that have it (a parent's member on each child)

                struct Binding {
                                const char          *cls = nullptr; // the member's class, for messages
                                const char          *name = nullptr;
                                Tag                  tag = gt::None; // a property's
                                const gt::MethodDef *method = nullptr;
                                const gt::EventDef  *event = nullptr;
                                uint32_t             atom = 0; // the member's name, interned at first use

                                uint32_t atomOf() {
                                        if (atom == 0) atom = bmds_gui_atom(gui, name);
                                        return atom;
                                }
                };

                GuiRef *self(asIScriptGeneric *gen) {
                        auto *o = static_cast<GuiRef *>(gen->GetObject());
                        if (!o->alive) {
                                raise("This %s was destroyed", o->cls->name);
                                return nullptr;
                        }
                        return gui != nullptr ? o : nullptr;
                }

                // A script value as the protocol's (strings and lists into @p strings, freed by the caller).
                bmds_gui_value_t toValue(Tag tag, asIScriptGeneric *gen, int arg, const char **&strings) {
                        switch (tag) {
                                case gt::Int: return bmds_gui_int(int32_t(gen->GetArgDWord(asUINT(arg))));
                                case gt::Long: return bmds_gui_long(int64_t(gen->GetArgQWord(asUINT(arg))));
                                case gt::Bool: return bmds_gui_bool(gen->GetArgByte(asUINT(arg)) != 0);
                                case gt::Color: return bmds_gui_color(gen->GetArgDWord(asUINT(arg)));
                                case gt::String:
                                        return bmds_gui_string(
                                                static_cast<const String *>(gen->GetArgAddress(asUINT(arg)))->cStr());
                                case gt::Bytes: {
                                        const auto *s = static_cast<const String *>(gen->GetArgAddress(asUINT(arg)));
                                        return bmds_gui_bytes(s->data(), s->length());
                                }
                                case gt::Atom: {
                                        const auto *s = static_cast<const String *>(gen->GetArgAddress(asUINT(arg)));
                                        return bmds_gui_atom_value(bmds_gui_atom(gui, s->cStr()));
                                }
                                case gt::Rect: {
                                        const auto *r =
                                                static_cast<const ScriptRect *>(gen->GetArgAddress(asUINT(arg)));
                                        return bmds_gui_rect(r->x, r->y, uint32_t(r->width), uint32_t(r->height));
                                }
                                case gt::Id: {
                                        auto          *o = static_cast<GuiRef *>(gen->GetArgObject(asUINT(arg)));
                                        const uint32_t id = o != nullptr && o->alive ? o->id : 0;
                                        if (o != nullptr) o->release(); // the argument's reference
                                        return bmds_gui_id(id);
                                }
                                case gt::Picture: {
                                        void          *image = gen->GetArgObject(asUINT(arg));
                                        const uint32_t id = image != nullptr ? iconOf(image) : 0;
                                        if (image != nullptr) imageRelease(image); // the argument's reference
                                        return bmds_gui_id(id);
                                }
                                case gt::List: {
                                        const auto *a =
                                                static_cast<const CScriptArray *>(gen->GetArgAddress(asUINT(arg)));
                                        const asUINT n = a != nullptr ? a->GetSize() : 0;
                                        strings = static_cast<const char **>(malloc((n + 1) * sizeof(char *)));
                                        if (strings == nullptr) return bmds_gui_int(0);
                                        for (asUINT i = 0; i < n; ++i)
                                                strings[i] = static_cast<const String *>(a->At(i))->cStr();
                                        return bmds_gui_strings(gui, strings, n);
                                }
                                default: return bmds_gui_int(0);
                        }
                }

                CScriptArray *listOf(const bmds_gui_value_t &v) {
                        CScriptArray *a = CScriptArray::Create(stringArrayType(theEngine));
                        for (uint32_t i = 0; a != nullptr && i < v.count; ++i) {
                                bmds_gui_value_t item;
                                if (bmds_gui_list_item(&v, i, &item) != 0) break;
                                String s(item.text, item.length);
                                a->InsertLast(&s);
                        }
                        return a;
                }

                // A protocol value as what a getter or a method returns.
                void returnValue(Tag tag, asIScriptGeneric *gen, const bmds_gui_value_t &v) {
                        switch (tag) {
                                case gt::Int: gen->SetReturnDWord(uint32_t(int32_t(v.number))); break;
                                case gt::Long: gen->SetReturnQWord(uint64_t(v.number)); break;
                                case gt::Bool: gen->SetReturnByte(v.number != 0); break;
                                case gt::Color: gen->SetReturnDWord(uint32_t(v.number) & 0xffffffu); break;
                                case gt::String:
                                case gt::Bytes:
                                        new (gen->GetAddressOfReturnLocation())
                                                String(v.text != nullptr ? v.text : "", v.length);
                                        break;
                                case gt::Atom: {
                                        const char *name = bmds_gui_atom_name(gui, uint32_t(v.number));
                                        new (gen->GetAddressOfReturnLocation()) String(name != nullptr ? name : "");
                                        break;
                                }
                                case gt::Rect: {
                                        ScriptRect r;
                                        r.x = v.x;
                                        r.y = v.y;
                                        r.width = int32_t(v.width);
                                        r.height = int32_t(v.height);
                                        new (gen->GetAddressOfReturnLocation()) ScriptRect(r);
                                        break;
                                }
                                case gt::Id: {
                                        GuiRef *o = find(uint32_t(v.number));
                                        if (o != nullptr) o->addRef();
                                        gen->SetReturnAddress(o);
                                        break;
                                }
                                case gt::Picture: {
                                        void *image = nullptr;
                                        for (size_t i = 0; i < iconCount && image == nullptr; ++i)
                                                if (icons[i].id == uint32_t(v.number)) image = icons[i].image;
                                        if (image != nullptr) imageAddRef(image);
                                        gen->SetReturnAddress(image);
                                        break;
                                }
                                case gt::List: gen->SetReturnAddress(listOf(v)); break;
                                default: break;
                        }
                        return;
                }

                // The zero of a type, returned when there is no answer.
                void returnNothing(Tag tag, asIScriptGeneric *gen) {
                        bmds_gui_value_t v;
                        memset(&v, 0, sizeof(v));
                        returnValue(tag, gen, v);
                        return;
                }

                void propertyGet(asIScriptGeneric *gen) {
                        auto   *b = static_cast<Binding *>(gen->GetAuxiliary());
                        GuiRef *o = self(gen);
                        if (o == nullptr) {
                                returnNothing(b->tag, gen);
                                return;
                        }
                        const uint32_t   name = b->atomOf();
                        bmds_gui_value_t v;
                        remember(b->cls, b->name, true);
                        if (bmds_gui_get(gui, o->id, &name, 1, &v) != 0) {
                                returnNothing(b->tag, gen);
                                return;
                        }
                        returnValue(b->tag, gen, v);
                        return;
                }

                void propertySet(asIScriptGeneric *gen) {
                        auto   *b = static_cast<Binding *>(gen->GetAuxiliary());
                        GuiRef *o = self(gen);
                        if (o == nullptr) return;
                        const char    **strings = nullptr;
                        bmds_gui_prop_t p;
                        p.name = b->atomOf();
                        p.value = toValue(b->tag, gen, 0, strings);
                        (void)bmds_gui_set(gui, o->id, &p, 1);
                        remember(b->cls, b->name);
                        free(strings);
                        return;
                }

                void methodCall(asIScriptGeneric *gen) {
                        auto                *b = static_cast<Binding *>(gen->GetAuxiliary());
                        const gt::MethodDef *m = b->method;
                        GuiRef              *o = self(gen);
                        const Tag            result = effective(m->result, m->name);
                        if (o == nullptr) {
                                if (result != gt::None) returnNothing(result, gen);
                                return;
                        }
                        bmds_gui_value_t args[6];
                        const char     **strings[6] = {};
                        for (uint8_t i = 0; i < m->count; ++i)
                                args[i] = toValue(effective(m->args[i], m->name), gen, i, strings[i]);
                        // A window shown or closed by the script: the loop runs while one is shown.
                        if (strcmp(o->cls->name, "Window") == 0) {
                                if (strcmp(m->name, "show") == 0) {
                                        o->shown = true;
                                        o->shownAt = ++showCount;
                                }
                                if (strcmp(m->name, "close") == 0) o->shown = false;
                        }
                        const uint32_t name = b->atomOf();
                        if (result == gt::None) {
                                (void)bmds_gui_call(gui, o->id, name, args, m->count, nullptr);
                                remember(b->cls, b->name);
                        } else {
                                bmds_gui_value_t r;
                                remember(b->cls, b->name, true);
                                if (bmds_gui_call(gui, o->id, name, args, m->count, &r) == 0)
                                        returnValue(result, gen, r);
                                else
                                        returnNothing(result, gen);
                        }
                        for (const char **s : strings) free(s);
                        return;
                }

                // on<Event>(callback): kept (null: none); an event sent only when asked is asked for.
                void eventSet(asIScriptGeneric *gen) {
                        auto   *b = static_cast<Binding *>(gen->GetAuxiliary());
                        GuiRef *o = self(gen);
                        // The argument's reference: kept as the callback's, or let go.
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (o == nullptr) {
                                if (fn != nullptr) fn->Release();
                                return;
                        }
                        const uint32_t atom = b->atomOf();
                        Handler       *slot = nullptr;
                        for (uint32_t i = 0; i < o->handlerCount; ++i)
                                if (o->handlers[i].atom == atom) slot = &o->handlers[i];
                        if (slot == nullptr && fn != nullptr) {
                                auto *grown = static_cast<Handler *>(
                                        realloc(o->handlers, (o->handlerCount + 1) * sizeof(Handler)));
                                if (grown == nullptr) {
                                        fn->Release();
                                        raise("Out of memory for a callback");
                                        return;
                                }
                                o->handlers = grown;
                                slot = &o->handlers[o->handlerCount++];
                                *slot = Handler{b->event, atom, nullptr};
                        }
                        if (slot == nullptr) return; // none, and none to clear
                        if (slot->fn != nullptr) slot->fn->Release();
                        slot->fn = fn;
                        if (!b->event->byDefault) {
                                (void)bmds_gui_listen(gui, o->id, &atom, 1, fn != nullptr);
                                remember(b->cls, b->name);
                        }
                        return;
                }

                void destroyObject(asIScriptGeneric *gen) {
                        auto *o = static_cast<GuiRef *>(gen->GetObject());
                        if (!o->alive || gui == nullptr) return;
                        (void)bmds_gui_destroy(gui, o->id);
                        remember(o->cls->name, "destroy");
                        forget(o->id);
                        return;
                }

                void refAdd(asIScriptGeneric *gen) {
                        static_cast<GuiRef *>(gen->GetObject())->addRef();
                }
                void refRelease(asIScriptGeneric *gen) {
                        static_cast<GuiRef *>(gen->GetObject())->release();
                }
                // Any class as one of its parents (or GuiObject): the same object.
                void implicitCast(asIScriptGeneric *gen) {
                        auto *o = static_cast<GuiRef *>(gen->GetObject());
                        o->addRef();
                        gen->SetReturnAddress(o);
                }

                // ---- Making objects

                struct Maker {
                                const gt::ClassDef *cls = nullptr;
                                const char         *textProperty = nullptr; // set from the second argument, if any
                };

                GuiRef *make(const gt::ClassDef *cls, GuiRef *parent) {
                        if (connection() == nullptr) return nullptr;
                        if (parent != nullptr && !parent->alive) {
                                raise("The parent was destroyed");
                                return nullptr;
                        }
                        const uint32_t parentId = parent != nullptr ? parent->id : 0;
                        const uint32_t id = bmds_gui_new(gui, cls->name, parentId);
                        remember(cls->name, "new");
                        if (id == 0) {
                                raise("Can't make a %s", cls->name);
                                return nullptr;
                        }
                        auto *o = new (std::nothrow) GuiRef();
                        if (o == nullptr) {
                                raise("Out of memory for a %s", cls->name);
                                return nullptr;
                        }
                        o->id = id;
                        o->parent = parentId;
                        o->cls = cls;
                        keep(o);
                        o->addRef(); // the script's handle
                        return o;
                }

                void setText(GuiRef *o, const char *property, const String *text) {
                        bmds_gui_prop_t p;
                        p.name = bmds_gui_atom(gui, property);
                        p.value = bmds_gui_string(text->cStr());
                        (void)bmds_gui_set(gui, o->id, &p, 1);
                        remember(o->cls->name, property);
                        return;
                }

                // The parent argument: its reference let go once the object is made under it.
                GuiRef *makeUnder(asIScriptGeneric *gen, const Maker *m) {
                        auto   *parent = static_cast<GuiRef *>(gen->GetArgObject(0));
                        GuiRef *o = make(m->cls, parent);
                        if (parent != nullptr) parent->release();
                        return o;
                }
                void makePlain(asIScriptGeneric *gen) {
                        gen->SetReturnAddress(makeUnder(gen, static_cast<Maker *>(gen->GetAuxiliary())));
                }
                void makeWithText(asIScriptGeneric *gen) {
                        auto   *m = static_cast<Maker *>(gen->GetAuxiliary());
                        GuiRef *o = makeUnder(gen, m);
                        if (o != nullptr)
                                setText(o, m->textProperty, static_cast<const String *>(gen->GetArgAddress(1)));
                        gen->SetReturnAddress(o);
                }
                // Window(title, width, height).
                void makeWindow(asIScriptGeneric *gen) {
                        auto   *m = static_cast<Maker *>(gen->GetAuxiliary());
                        GuiRef *o = make(m->cls, nullptr);
                        if (o != nullptr) {
                                const auto *title = static_cast<const String *>(gen->GetArgAddress(0));
                                const int   width = int(gen->GetArgDWord(1)), height = int(gen->GetArgDWord(2));
                                if (title->length() != 0) setText(o, "title", title);
                                bmds_gui_prop_t p[2];
                                uint32_t        n = 0;
                                if (width > 0)
                                        p[n++] = bmds_gui_prop_t{bmds_gui_atom(gui, "width"), bmds_gui_int(width)};
                                if (height > 0)
                                        p[n++] = bmds_gui_prop_t{bmds_gui_atom(gui, "height"), bmds_gui_int(height)};
                                if (n != 0) (void)bmds_gui_set(gui, o->id, p, n);
                                remember("Window", "size");
                        }
                        gen->SetReturnAddress(o);
                }
                // Box(parent, direction).
                void makeBox(asIScriptGeneric *gen) {
                        GuiRef *o = makeUnder(gen, static_cast<Maker *>(gen->GetAuxiliary()));
                        if (o != nullptr) {
                                const auto     *direction = static_cast<const String *>(gen->GetArgAddress(1));
                                bmds_gui_prop_t p{bmds_gui_atom(gui, "direction"),
                                                  bmds_gui_atom_value(bmds_gui_atom(gui, direction->cStr()))};
                                (void)bmds_gui_set(gui, o->id, &p, 1);
                                remember("Box", "direction");
                        }
                        gen->SetReturnAddress(o);
                }

                void rectConstruct(asIScriptGeneric *gen) {
                        new (gen->GetObject()) ScriptRect();
                }
                void rectConstructWith(asIScriptGeneric *gen) {
                        auto *r = new (gen->GetObject()) ScriptRect();
                        r->x = int32_t(gen->GetArgDWord(0));
                        r->y = int32_t(gen->GetArgDWord(1));
                        r->width = int32_t(gen->GetArgDWord(2));
                        r->height = int32_t(gen->GetArgDWord(3));
                }

                // ---- Canvases: drawn as a Screen is (graphics.cpp's pictures), through a surface of the
                // service's (CANVAS-SURFACE): made at the first drawing, again at each new size

#ifdef ANGEL_BMDS
                // A canvas's failure, said where it can be: the running script's exception, else stderr.
                void canvasFailed(const char *what, int error) {
                        if (asGetActiveContext() != nullptr) {
                                raise("Can't %s: %s", what, strerror(error));
                                return;
                        }
                        fflush(stdout);
                        fprintf(stderr, "angel: can't %s: %s\n", what, strerror(error));
                        return;
                }
#endif

                void putPixels(uint32_t id, int x, int y, int width, int height, const uint16_t *rgb565) {
                        if (gui != nullptr)
                                (void)bmds_gui_put_pixels(gui, id, x, y, uint32_t(width), uint32_t(height), rgb565,
                                                          uint32_t(width));
                        return;
                }

                // Canvas @p o with no surface (a remote connection's, or on a PC): its picture at
                // @p width x @p height, sent by PutPixels as it is drawn.
                void sendCanvas(GuiRef *o, int width, int height) {
                        int w = 0, h = 0;
                        canvasSize(o->picture, w, h);
                        if ((w != width || h != height) && canvasResize(o->picture, width, height) != 0) return;
                        canvasSendTo(o->picture, o->id, putPixels);
                        return;
                }

                // Canvas @p o's picture shown through a new surface of @p width x @p height, else (no
                // CANVAS-SURFACE: a remote connection) sent as pixels.
                void showCanvas(GuiRef *o, int width, int height) {
#ifdef ANGEL_BMDS
                        bmds_surface_config_t config{};
                        config.size = sizeof(config);
                        config.width = uint32_t(width + 3) & ~3u; // as a screen's: the 2D-DMA's rows
                        config.height = uint32_t(height);
                        config.format = BMDS_PIXEL_BGR24; // as a screen's: a colour reads back as drawn
                        config.buffers = 2;
                        const int fd = bmds_gui_canvas_surface_open(gui, o->id, &config);
                        if (fd < 0 && errno == ENOTSUP) {
                                sendCanvas(o, width, height);
                                return;
                        }
                        if (fd < 0) {
                                // Not laid out after all (its window hidden again): drawn in memory still.
                                if (errno != EAGAIN) canvasFailed("show a canvas", errno);
                                return;
                        }
                        const int e = canvasShow(o->picture, fd, width, height);
                        if (e != 0) {
                                close(fd);
                                canvasFailed("show a canvas", e);
                        }
#else
                        sendCanvas(o, width, height); // a PC's angel: every connection is remote
#endif
                        return;
                }

                int intNamed(uint32_t id, const char *name) {
                        bmds_gui_value_t v;
                        return bmds_gui_get_named(gui, id, name, &v) == 0 ? int(v.number) : 0;
                }

                // The picture a Canvas's drawing methods draw on: made at the first, at the canvas's
                // size if it has one (its window shown), else at the size it asks for, in memory until
                // it is laid out.
                void *canvasOf(void *object) {
                        auto *o = static_cast<GuiRef *>(object);
                        if (!o->alive || gui == nullptr) {
                                raise("This Canvas was destroyed");
                                return nullptr;
                        }
                        if (o->picture != nullptr) return o->picture;
                        int        width = intNamed(o->id, "width"), height = intNamed(o->id, "height");
                        const bool laidOut = width > 0 && height > 0;
                        if (!laidOut) {
                                width = intNamed(o->id, "preferredWidth");
                                height = intNamed(o->id, "preferredHeight");
                                if (width <= 0 || height <= 0) {
                                        raise("This canvas has no size yet: show its window first, or give it a "
                                              "preferredWidth and preferredHeight");
                                        return nullptr;
                                }
                        }
                        o->picture = canvasMake(width, height);
                        if (o->picture != nullptr && laidOut) showCanvas(o, width, height);
                        return o->picture;
                }
                constexpr CanvasHooks canvasHooks = {canvasOf};

                // ---- Calling the script back

                // Calls @p fn with event @p e's values (as @p event has them), or with @p object (a
                // screen's Event), or with nothing.
                // A context to call the script in: the running one's state pushed (@p nested), or the
                // GUI's own after the script's end. @return Null if there's none.
                asIScriptContext *beginCall(bool &nested) {
                        asIScriptContext *ctx = asGetActiveContext();
                        nested = ctx != nullptr;
                        if (nested) return ctx->PushState() < 0 ? nullptr : ctx;
                        if (ownContext == nullptr) ownContext = theEngine->CreateContext();
                        return ownContext;
                }

                // The call made (@p r: its arguments' result so far): an exception ends the script.
                void endCall(asIScriptContext *ctx, bool nested, int r) {
                        if (r >= 0) r = ctx->Execute();
                        if (r == asEXECUTION_EXCEPTION) {
                                fflush(stdout);
                                reportException(ctx);
                                restoreTerminal();
                                exit(1);
                        }
                        if (nested) {
                                const asEContextState state = ctx->GetState();
                                ctx->PopState();
                                if (state == asEXECUTION_ABORTED) ctx->Abort();
                        } else {
                                ctx->Unprepare();
                        }
                        return;
                }

                void call(asIScriptFunction *fn, const gt::EventDef *event, const bmds_gui_event_t *e,
                          void *object = nullptr) {
                        bool              nested = false;
                        asIScriptContext *ctx = beginCall(nested);
                        if (ctx == nullptr) return;
                        int           r = ctx->Prepare(fn);
                        String        strings[6];
                        ScriptRect    rects[6];
                        const uint8_t count = event != nullptr && e != nullptr ? event->count : 0;
                        for (uint8_t i = 0; r >= 0 && i < count; ++i) {
                                bmds_gui_value_t v;
                                memset(&v, 0, sizeof(v));
                                if (i < e->count) v = e->values[i];
                                switch (event->values[i]) {
                                        case gt::Int:
                                        case gt::Color: r = ctx->SetArgDWord(i, uint32_t(v.number)); break;
                                        case gt::Long: r = ctx->SetArgQWord(i, uint64_t(v.number)); break;
                                        case gt::Bool: r = ctx->SetArgByte(i, v.number != 0); break;
                                        case gt::String:
                                        case gt::Bytes:
                                                strings[i].assign(v.text != nullptr ? v.text : "", v.length);
                                                r = ctx->SetArgAddress(i, &strings[i]);
                                                break;
                                        case gt::Atom: {
                                                const char *name = bmds_gui_atom_name(gui, uint32_t(v.number));
                                                strings[i].assign(name != nullptr ? name : "",
                                                                  name != nullptr ? strlen(name) : 0);
                                                r = ctx->SetArgAddress(i, &strings[i]);
                                                break;
                                        }
                                        case gt::Rect:
                                                rects[i].x = v.x;
                                                rects[i].y = v.y;
                                                rects[i].width = int32_t(v.width);
                                                rects[i].height = int32_t(v.height);
                                                r = ctx->SetArgAddress(i, &rects[i]);
                                                break;
                                        case gt::List: {
                                                CScriptArray *a = listOf(v);
                                                r = ctx->SetArgObject(i, a);
                                                if (a != nullptr) a->Release();
                                                break;
                                        }
                                        case gt::Id: r = ctx->SetArgObject(i, find(uint32_t(v.number))); break;
                                        default: break;
                                }
                        }
                        if (r >= 0 && object != nullptr) r = ctx->SetArgObject(0, object);
                        endCall(ctx, nested, r);
                        return;
                }

                void dispatch(const bmds_gui_event_t &e) {
                        GuiRef *o = find(e.source);
                        if (o == nullptr) return;
                        o->addRef(); // a callback may destroy it
                        if (strcmp(o->cls->name, "Window") == 0 && bmds_gui_event_is(gui, &e, "closed"))
                                o->shown = false;
                        // A canvas drawn on: its picture at the new size before the script hears of it.
                        if (o->picture != nullptr && e.count >= 2 && bmds_gui_event_is(gui, &e, "resized")) {
                                int width = 0, height = 0;
                                canvasSize(o->picture, width, height);
                                const int w = int(e.values[0].number), h = int(e.values[1].number);
                                if (w > 0 && h > 0 && (w != width || h != height)) showCanvas(o, w, h);
                        }
                        for (uint32_t i = 0; i < o->handlerCount; ++i) {
                                if (o->handlers[i].atom != e.name || o->handlers[i].fn == nullptr) continue;
                                asIScriptFunction *fn = o->handlers[i].fn;
                                fn->AddRef(); // the callback may replace itself
                                call(fn, o->handlers[i].event, &e);
                                fn->Release();
                                break;
                        }
                        o->release();
                        return;
                }

                // ---- app: the loop and its timers

                struct Timer {
                                int                id = 0; // 0: free
                                int64_t            due = 0;
                                uint32_t           interval = 0; // a repeating one's (0: once)
                                asIScriptFunction *fn = nullptr;
                };
                constexpr size_t MaxTimers = 64;
                Timer            timers[MaxTimers];
                int              lastTimer = 0;

                int64_t nowMs() {
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
                }

                int startTimer(uint32_t ms, asIScriptFunction *fn, bool repeat) {
                        if (fn == nullptr) {
                                raise("No callback for the timer (null)");
                                return 0;
                        }
                        for (Timer &t : timers) {
                                if (t.id != 0) continue;
                                if (++lastTimer <= 0) lastTimer = 1;
                                t.id = lastTimer;
                                t.due = nowMs() + ms;
                                t.interval = repeat ? (ms != 0 ? ms : 1) : 0;
                                t.fn = fn; // the argument's reference
                                return t.id;
                        }
                        fn->Release();
                        raise("Too many timers (%u at once)", unsigned(MaxTimers));
                        return 0;
                }

                void stopTimer(int id) {
                        for (Timer &t : timers) {
                                if (t.id != id || id == 0) continue;
                                t.id = 0;
                                if (t.fn != nullptr) t.fn->Release();
                                t.fn = nullptr;
                        }
                        return;
                }

                // The timers due run (each once, in turn); @return ms until the next (-1: none).
                int runTimers() {
                        const int64_t now = nowMs();
                        for (Timer &t : timers) {
                                if (t.id == 0 || t.due > now) continue;
                                const int          id = t.id;
                                asIScriptFunction *fn = t.fn;
                                fn->AddRef();
                                if (t.interval != 0) {
                                        t.due += t.interval;
                                        if (t.due <= now) t.due = now + t.interval; // far behind: not caught up
                                } else {
                                        stopTimer(id);
                                }
                                call(fn, nullptr, nullptr);
                                fn->Release();
                                if (quitting) break;
                        }
                        int64_t       next = -1;
                        const int64_t after = nowMs();
                        for (const Timer &t : timers)
                                if (t.id != 0 && (next < 0 || t.due - after < next))
                                        next = t.due - after > 0 ? t.due - after : 0;
                        return next < 0 ? -1 : int(next > 60000 ? 60000 : next);
                }

                // A screen's event called back (Screen.onEvent). @return False if none was waiting.
                bool serveScreen() {
                        asIScriptFunction *fn = nullptr;
                        void              *event = nullptr;
                        if (!nextScreenEvent(fn, event)) return false;
                        call(fn, nullptr, nullptr, event);
                        fn->Release();
                        screenEventRelease(event);
                        return true;
                }

                // ---- The loop's other sources (angel.h), registered by their owners' structs

                constexpr size_t MaxSources = 64;
                LoopSource      *sources[MaxSources];
                size_t           sourceCount = 0;

                bool registered(const LoopSource *source) {
                        for (size_t i = 0; i < sourceCount; ++i)
                                if (sources[i] == source) return true;
                        return false;
                }

                // The most any source holds the loop for.
                LoopHold sourcesHold() {
                        LoopHold most = LoopHold::Nothing;
                        for (size_t i = 0; i < sourceCount; ++i)
                                if (uint8_t(sources[i]->hold) > uint8_t(most)) most = sources[i]->hold;
                        return most;
                }

                // What was drawn on canvases shown, then waits up to @p wait ms (-1: for ever) for the
                // GUI's next event, serving the screens' and the other sources meanwhile. @return 1 (an
                // event in @p e), 0 (none in time, or something else served), -1 (the connection ended).
                int nextEvent(bmds_gui_event_t &e, int wait) {
                        presentCanvases();
                        if (serveScreen()) return 0;
                        const int  screens = screenEventFd();
                        const bool alone = screens < 0 && sourceCount == 0; // the connection's the only one
                        if (gui != nullptr) {
                                const int r = bmds_gui_next_event(gui, &e, alone ? wait : 0);
                                if (r != 0 || alone) return r;
                        }
                        struct pollfd fds[2 + MaxSources];
                        LoopSource   *polled[MaxSources];
                        nfds_t        n = 0;
                        if (gui != nullptr) fds[n++] = {bmds_gui_fd(gui), POLLIN, 0};
                        if (screens >= 0) fds[n++] = {screens, POLLIN, 0};
                        const nfds_t first = n;
                        const size_t count = sourceCount;
                        for (size_t i = 0; i < count; ++i) {
                                polled[i] = sources[i];
                                fds[n++] = {sources[i]->fd, POLLIN, 0};
                        }
                        if (poll(fds, n, wait) <= 0) return 0;
                        for (size_t i = 0; i < count; ++i) {
                                // One an earlier source's callback took away isn't called.
                                if (fds[first + i].revents == 0 || !registered(polled[i])) continue;
                                polled[i]->ready(polled[i]);
                                if (quitting) return 0;
                        }
                        if (gui == nullptr || fds[0].revents == 0) return 0;
                        return bmds_gui_next_event(gui, &e, 0);
                }

                // Until app.quit(), or until no window is shown once one was (or, with no windows and no
                // GUI, until the timers are done).
                void loop() {
                        if (looping) {
                                raise("app.run() is running already");
                                return;
                        }
                        looping = true;
                        quitting = false;
                        quitCalled = false;
                        bool wasShown = false;
                        for (;;) {
                                const int wait = runTimers();
                                if (quitting) break;
                                const bool screens = screensServed();
                                if (anyShown() || screens)
                                        wasShown = true;
                                else if (wasShown)
                                        break;
                                // Nothing left to wait for.
                                const bool held = screens || sourcesHold() != LoopHold::Nothing;
                                if (gui == nullptr && wait < 0 && !held) break;
                                if (gui != nullptr && wait < 0 && objectCount == 0 && !held) break;
                                bmds_gui_event_t e;
                                const int        r = nextEvent(e, wait);
                                if (r < 0) {
                                        fflush(stdout);
                                        fprintf(stderr, "angel: the GUI's connection ended\n");
                                        break;
                                }
                                if (r == 1) dispatch(e);
                                if (quitting) break;
                        }
                        looping = false;
                        quitting = false;
                        if (gui != nullptr) (void)bmds_gui_flush(gui);
                        return;
                }

                void appRun(asIScriptGeneric *) {
                        loop();
                }
                void appQuit(asIScriptGeneric *) {
                        quitting = true;
                        quitCalled = true;
                }
                void appAfter(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(startTimer(
                                gen->GetArgDWord(0), static_cast<asIScriptFunction *>(gen->GetArgObject(1)), false)));
                }
                void appEvery(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(startTimer(
                                gen->GetArgDWord(0), static_cast<asIScriptFunction *>(gen->GetArgObject(1)), true)));
                }
                void appStop(asIScriptGeneric *gen) {
                        stopTimer(int(gen->GetArgDWord(0)));
                }

                int appObject; // `app`'s address: the type has no state of its own

                // ---- The one-line dialogs: each made, opened and waited for; the script's window they
                // belong to (its active one, else the last shown) blocked meanwhile

                // Serves the GUI's events and the timers until an event of one of @p ids (a dialog's own
                // objects) comes. @return Its index in @p ids, the event in @p out (valid until the next
                // call on the connection); -1 at app.quit() or the connection's end.
                int waitFor(const uint32_t *ids, size_t count, bmds_gui_event_t &out) {
                        for (;;) {
                                const int wait = runTimers();
                                if (quitting) return -1;
                                const int r = nextEvent(out, wait);
                                if (r < 0) return -1;
                                if (r == 0) continue;
                                for (size_t i = 0; i < count; ++i)
                                        if (out.source == ids[i]) return int(i);
                                dispatch(out);
                                if (quitting) return -1;
                        }
                }

                GuiRef *ownerWindow() {
                        GuiRef        *latest = nullptr;
                        const uint32_t active = bmds_gui_atom(gui, "active");
                        for (size_t i = 0; i < objectCount; ++i) {
                                GuiRef *o = objects[i];
                                if (!o->shown || strcmp(o->cls->name, "Window") != 0) continue;
                                bmds_gui_value_t v;
                                if (bmds_gui_get(gui, o->id, &active, 1, &v) == 0 && v.number != 0) return o;
                                if (latest == nullptr || o->shownAt > latest->shownAt) latest = o;
                        }
                        return latest;
                }

                // One of a dialog's objects (not the script's: not kept), in @p parent (0: none).
                uint32_t dialogObject(const char *cls, uint32_t parent, const char *what) {
                        const uint32_t id = bmds_gui_new(gui, cls, parent);
                        remember(what, cls);
                        if (id == 0) raise("Can't make the %s's %s", what, cls);
                        return id;
                }
                void setNamed(uint32_t id, const char *name, bmds_gui_value_t v, const char *what) {
                        (void)bmds_gui_set_named(gui, id, name, v);
                        remember(what, name);
                        return;
                }
                void setAtom(uint32_t id, const char *name, const char *value, const char *what) {
                        setNamed(id, name, bmds_gui_atom_value(bmds_gui_atom(gui, value)), what);
                        return;
                }
                void callNamed(uint32_t id, const char *name, const char *what) {
                        (void)bmds_gui_call_named(gui, id, name, nullptr, 0, nullptr);
                        remember(what, name);
                        return;
                }
                // The dialog's title: @p title, else its owner's.
                void setTitle(uint32_t id, const String *title, GuiRef *owner, const char *what) {
                        if (title->length() != 0) {
                                setNamed(id, "title", bmds_gui_string(title->cStr()), what);
                                return;
                        }
                        bmds_gui_value_t v;
                        if (owner == nullptr || bmds_gui_get_named(gui, owner->id, "title", &v) != 0) return;
                        const String copy(v.text != nullptr ? v.text : "", v.length);
                        setNamed(id, "title", bmds_gui_string(copy.cStr()), what);
                        return;
                }

                // A message box of @p kind with @p buttons: the answer's name ("" if none).
                void messageBox(asIScriptGeneric *gen, const char *what, const char *kind, const char *buttons,
                                String &answer) {
                        if (connection() == nullptr) return;
                        GuiRef        *owner = ownerWindow();
                        const uint32_t id = dialogObject("MessageBox", 0, what);
                        if (id == 0) return;
                        setAtom(id, "kind", kind, what);
                        setAtom(id, "buttons", buttons, what);
                        setNamed(id, "text",
                                 bmds_gui_string(static_cast<const String *>(gen->GetArgAddress(0))->cStr()), what);
                        setTitle(id, static_cast<const String *>(gen->GetArgAddress(1)), owner, what);
                        if (owner != nullptr) setNamed(id, "owner", bmds_gui_id(owner->id), what);
                        callNamed(id, "open", what);
                        bmds_gui_event_t e;
                        int              r;
                        do r = waitFor(&id, 1, e);
                        while (r == 0 && !bmds_gui_event_is(gui, &e, "answered"));
                        if (r == 0 && e.count != 0) {
                                const char *name = bmds_gui_atom_name(gui, uint32_t(e.values[0].number));
                                if (name != nullptr) answer.assign(name, strlen(name));
                        }
                        (void)bmds_gui_destroy(gui, id);
                        remember(what, "destroy");
                        return;
                }
                void alertCall(asIScriptGeneric *gen) {
                        String answer;
                        messageBox(gen, "alert", "information", "ok", answer);
                }
                void askCall(asIScriptGeneric *gen) {
                        String answer;
                        messageBox(gen, "ask", "question", "yesNo", answer);
                        gen->SetReturnByte(strcmp(answer.cStr(), "yes") == 0);
                }

                // openFile(path): opened as a double click on the desktop opens it, by the service
                // named `open` (the panel's: devplan/guiprograms.md §14), the path absolute and a NUL
                // after it.
                void openFileCall(asIScriptGeneric *gen) {
                        const char   *what = "openFile";
                        const String &path = *static_cast<const String *>(gen->GetArgAddress(0));
                        Text          full;
                        if (path.cStr()[0] != '/') {
                                char cwd[512];
                                if (getcwd(cwd, sizeof(cwd)) == nullptr) {
                                        raise("Can't open '%s': %s", path.cStr(), strerror(errno));
                                        return;
                                }
                                full.append(cwd);
                                if (strcmp(cwd, "/") != 0) full.append("/");
                        }
                        full.append(path.data(), path.length());
                        struct stat st;
                        if (stat(full.data(), &st) != 0) {
                                raise("Can't open '%s': %s", path.cStr(), strerror(errno));
                                return;
                        }
                        if (connection() == nullptr) return;
                        const uint32_t id = dialogObject("Services", 0, what);
                        if (id == 0) return;
                        bmds_gui_value_t args[2] = {bmds_gui_string("open"),
                                                    bmds_gui_bytes(full.data(), uint32_t(full.length() + 1))};
                        bmds_gui_value_t has;
                        if (bmds_gui_call_named(gui, id, "has", args, 1, &has) != 0 || has.number == 0) {
                                raise("Nothing opens files here (start the panel)");
                        } else {
                                (void)bmds_gui_call_named(gui, id, "send", args, 2, nullptr);
                                remember(what, "send");
                        }
                        (void)bmds_gui_destroy(gui, id);
                        remember(what, "destroy");
                }

                // askText(): a window of its own, modal over the script's: the prompt, a line to edit,
                // OK and Cancel. "" if cancelled.
                void askTextCall(asIScriptGeneric *gen) {
                        String *result = new (gen->GetAddressOfReturnLocation()) String();
                        if (connection() == nullptr) return;
                        const char    *what = "askText";
                        GuiRef        *owner = ownerWindow();
                        const uint32_t win = dialogObject("Window", 0, what);
                        if (win == 0) return;
                        setTitle(win, static_cast<const String *>(gen->GetArgAddress(2)), owner, what);
                        setNamed(win, "width", bmds_gui_int(320), what);
                        setNamed(win, "margin", bmds_gui_int(10), what);
                        setNamed(win, "spacing", bmds_gui_int(8), what);
                        setNamed(win, "resizable", bmds_gui_bool(0), what);
                        setNamed(win, "buttons", bmds_gui_bool(0), what); // a dialog's: the close box alone
                        if (owner != nullptr) {
                                setNamed(win, "owner", bmds_gui_id(owner->id), what);
                                setNamed(win, "modal", bmds_gui_bool(1), what);
                        }
                        const uint32_t label = dialogObject("Label", win, what);
                        setNamed(label, "text",
                                 bmds_gui_string(static_cast<const String *>(gen->GetArgAddress(0))->cStr()), what);
                        const uint32_t edit = dialogObject("LineEdit", win, what);
                        setNamed(edit, "text",
                                 bmds_gui_string(static_cast<const String *>(gen->GetArgAddress(1))->cStr()), what);
                        const uint32_t row = dialogObject("Box", win, what);
                        setAtom(row, "direction", "across", what);
                        setNamed(row, "spacing", bmds_gui_int(6), what);
                        const uint32_t space = dialogObject("Spacer", row, what);
                        setNamed(space, "stretch", bmds_gui_int(1), what);
                        const uint32_t ok = dialogObject("Button", row, what);
                        setNamed(ok, "text", bmds_gui_string("&OK"), what);
                        setNamed(ok, "centered", bmds_gui_bool(1), what);
                        const uint32_t cancel = dialogObject("Button", row, what);
                        setNamed(cancel, "text", bmds_gui_string("&Cancel"), what);
                        setNamed(cancel, "centered", bmds_gui_bool(1), what);
                        callNamed(win, "show", what);
                        callNamed(edit, "selectAll", what);
                        callNamed(edit, "setFocus", what);
                        // The answer: Enter in the line, OK, Cancel or the close box (the window's other
                        // events, as `resized`, aren't).
                        const uint32_t   ids[] = {win, edit, ok, cancel};
                        bmds_gui_event_t e;
                        int              which;
                        do {
                                which = waitFor(ids, 4, e);
                        } while ((which == 0 && !bmds_gui_event_is(gui, &e, "closed")) ||
                                 (which == 1 && !bmds_gui_event_is(gui, &e, "accepted")) ||
                                 (which >= 2 && !bmds_gui_event_is(gui, &e, "clicked")));
                        if (which == 1 || which == 2) {
                                bmds_gui_value_t v;
                                if (bmds_gui_get_named(gui, edit, "text", &v) == 0 && v.text != nullptr)
                                        result->assign(v.text, v.length);
                        }
                        if (which != 0) callNamed(win, "close", what);
                        (void)bmds_gui_destroy(gui, win);
                        remember(what, "destroy");
                        return;
                }

                // A file dialog in @p mode: the path chosen, "" if none. It opens where the last one was.
                void fileDialog(asIScriptGeneric *gen, const char *what, const char *mode, const String *title,
                                const String *name) {
                        String *result = new (gen->GetAddressOfReturnLocation()) String();
                        if (connection() == nullptr) return;
                        GuiRef        *owner = ownerWindow();
                        const uint32_t id = dialogObject("FileDialog", 0, what);
                        if (id == 0) return;
                        setAtom(id, "mode", mode, what);
                        if (title->length() != 0) setNamed(id, "title", bmds_gui_string(title->cStr()), what);
                        if (lastFolder != nullptr) setNamed(id, "folder", bmds_gui_string(lastFolder), what);
                        if (name != nullptr && name->length() != 0)
                                setNamed(id, "fileName", bmds_gui_string(name->cStr()), what);
                        if (owner != nullptr) setNamed(id, "owner", bmds_gui_id(owner->id), what);
                        callNamed(id, "open", what);
                        bmds_gui_event_t e;
                        int              r;
                        do r = waitFor(&id, 1, e);
                        while (r == 0 && !bmds_gui_event_is(gui, &e, "finished"));
                        if (r == 0 && e.count >= 2 && e.values[0].number != 0) {
                                bmds_gui_value_t path;
                                if (bmds_gui_list_item(&e.values[1], 0, &path) == 0 && path.text != nullptr)
                                        result->assign(path.text, path.length);
                                bmds_gui_value_t folder;
                                if (bmds_gui_get_named(gui, id, "folder", &folder) == 0 && folder.text != nullptr) {
                                        char *copy = static_cast<char *>(malloc(folder.length + 1));
                                        if (copy != nullptr) {
                                                memcpy(copy, folder.text, folder.length);
                                                copy[folder.length] = '\0';
                                                free(lastFolder);
                                                lastFolder = copy;
                                        }
                                }
                        }
                        (void)bmds_gui_destroy(gui, id);
                        remember(what, "destroy");
                        return;
                }
                void chooseFileCall(asIScriptGeneric *gen) {
                        fileDialog(gen, "chooseFile", "open", static_cast<const String *>(gen->GetArgAddress(0)),
                                   nullptr);
                }
                void chooseSaveFileCall(asIScriptGeneric *gen) {
                        fileDialog(gen, "chooseSaveFile", "save", static_cast<const String *>(gen->GetArgAddress(1)),
                                   static_cast<const String *>(gen->GetArgAddress(0)));
                }

                // chooseColor(): the GUI's colour dialog, starting at @p color; that colour if cancelled.
                void chooseColorCall(asIScriptGeneric *gen) {
                        const char *what = "chooseColor";
                        uint32_t    color = gen->GetArgDWord(0) & 0xffffffu;
                        if (connection() == nullptr) {
                                gen->SetReturnDWord(color);
                                return;
                        }
                        GuiRef        *owner = ownerWindow();
                        const uint32_t id = dialogObject("ColorDialog", 0, what);
                        if (id == 0) {
                                gen->SetReturnDWord(color);
                                return;
                        }
                        setNamed(id, "color", bmds_gui_color(color), what);
                        setTitle(id, static_cast<const String *>(gen->GetArgAddress(1)), owner, what);
                        if (owner != nullptr) setNamed(id, "owner", bmds_gui_id(owner->id), what);
                        callNamed(id, "open", what);
                        bmds_gui_event_t e;
                        int              r;
                        do r = waitFor(&id, 1, e);
                        while (r == 0 && !bmds_gui_event_is(gui, &e, "finished"));
                        if (r == 0 && e.count >= 2 && e.values[0].number != 0)
                                color = uint32_t(e.values[1].number) & 0xffffffu;
                        (void)bmds_gui_destroy(gui, id);
                        remember(what, "destroy");
                        gen->SetReturnDWord(color);
                }

                // ---- Registration

                // A type's script spelling: what a getter returns, what a setter or an argument takes,
                // what a callback is given.
                const char *returnType(Tag tag, const char *member) {
                        switch (tag) {
                                case gt::Int: return "int";
                                case gt::Long: return "int64";
                                case gt::Bool: return "bool";
                                case gt::String:
                                case gt::Atom:
                                case gt::Bytes: return "string";
                                case gt::Color: return "uint";
                                case gt::List: return "array<string>@";
                                case gt::Rect: return "Rect";
                                case gt::Id: return idType(member);
                                default: return "void";
                        }
                }
                const char *argType(Tag tag, const char *member) {
                        switch (tag) {
                                case gt::String:
                                case gt::Atom:
                                case gt::Bytes: return "const string &in";
                                case gt::List: return "const array<string> &in";
                                case gt::Rect: return "const Rect &in";
                                default: return returnType(tag, member);
                        }
                }
                // A callback's parameter, and its word in the funcdef's name.
                const char *callbackType(Tag tag) {
                        switch (tag) {
                                case gt::List: return "array<string>@";
                                case gt::Id: return "GuiObject@";
                                default: return argType(tag, "");
                        }
                }
                const char *callbackWord(Tag tag) {
                        switch (tag) {
                                case gt::Int: return "Int";
                                case gt::Long: return "Long";
                                case gt::Bool: return "Bool";
                                case gt::Color: return "Color";
                                case gt::List: return "List";
                                case gt::Rect: return "Rect";
                                case gt::Id: return "Object";
                                default: return "String";
                        }
                }
                void callbackName(const gt::EventDef &e, Text &out) {
                        out.clear();
                        for (uint8_t i = 0; i < e.count; ++i) out.append(callbackWord(e.values[i]));
                        out.append("Callback");
                        return;
                }

                char *copyOf(const Text &t) {
                        char *s = strdup(t.data());
                        if (s == nullptr) check(-1, "the GUI's help");
                        return s;
                }

                Binding *bindings = nullptr; // one for each property, method and event of the schema
                Maker   *makers = nullptr;

                void registerMembers(asIScriptEngine *engine, const gt::ClassDef &target) {
                        static constexpr size_t PropCount = sizeof(gt::Props) / sizeof(gt::Props[0]);
                        static constexpr size_t MethodCount = sizeof(gt::Methods) / sizeof(gt::Methods[0]);
                        const char             *T = target.name;
                        // Names taken (kind + name), so a parent's member a child redefines isn't registered twice.
                        char       taken[256][40];
                        size_t     takenCount = 0;
                        const auto claim = [&](char kind, const char *name) {
                                char key[40];
                                snprintf(key, sizeof(key), "%c%s", kind, name);
                                for (size_t i = 0; i < takenCount; ++i)
                                        if (strcmp(taken[i], key) == 0) return false;
                                if (takenCount < 256) memcpy(taken[takenCount++], key, sizeof(key));
                                return true;
                        };
                        Text decl, help;
                        for (const gt::ClassDef *c = &target; c != nullptr; c = parentOf(c)) {
                                for (size_t i = 0; i < PropCount; ++i) {
                                        const gt::PropDef &p = gt::Props[i];
                                        if (strcmp(p.cls, c->name) != 0 || !claim('p', p.name)) continue;
                                        const char *type = returnType(p.tag, p.name);
                                        Binding *b = &bindings[i];
                                        if (p.readable) {
                                                decl.clear();
                                                decl.appendf("%s get_%s() const property", type, p.name);
                                                help.clear();
                                                help.appendf("%s's %s (the GUI's)%s.", p.cls, p.name,
                                                             p.writable ? "" : ", read only");
                                                methodAux(engine, T, decl.data(), propertyGet, b, copyOf(help));
                                        }
                                        if (p.writable) {
                                                decl.clear();
                                                decl.appendf("void set_%s(%s) property", p.name,
                                                             argType(p.tag, p.name));
                                                methodAux(engine, T, decl.data(), propertySet, b, nullptr);
                                        }
                                }
                                for (size_t i = 0; i < MethodCount; ++i) {
                                        const gt::MethodDef &m = gt::Methods[i];
                                        if (strcmp(m.cls, c->name) != 0 || !claim('m', m.name)) continue;
                                        decl.clear();
                                        decl.appendf("%s %s(", returnType(m.result, m.name), m.name);
                                        for (uint8_t a = 0; a < m.count; ++a)
                                                decl.appendf("%s%s", a != 0 ? ", " : "", argType(m.args[a], m.name));
                                        decl.append(")");
                                        help.clear();
                                        help.appendf("%s's %s() (the GUI's)%s.", m.cls, m.name,
                                                     m.result != gt::None ? ": waits for the answer" : "");
                                        methodAux(engine, T, decl.data(), methodCall, &bindings[PropCount + i],
                                                  copyOf(help));
                                }
                                for (size_t i = 0; i < sizeof(gt::Events) / sizeof(gt::Events[0]); ++i) {
                                        const gt::EventDef &e = gt::Events[i];
                                        if (strcmp(e.cls, c->name) != 0 || !claim('e', e.name)) continue;
                                        Text name;
                                        callbackName(e, name);
                                        decl.clear();
                                        decl.appendf("void on%c%s(%s@ callback)", e.name[0] - 'a' + 'A', e.name + 1,
                                                     name.data());
                                        help.clear();
                                        help.appendf("Calls back on %s's %s event (null: no more)%s.", e.cls, e.name,
                                                     e.byDefault ? "" : "; asks for it");
                                        methodAux(engine, T, decl.data(), eventSet,
                                                  &bindings[PropCount + MethodCount + i], copyOf(help));
                                }
                        }
                        return;
                }

        } // namespace

        void registerGui(asIScriptEngine *engine) {
                theEngine = engine;
                constexpr size_t PropCount = sizeof(gt::Props) / sizeof(gt::Props[0]);
                constexpr size_t MethodCount = sizeof(gt::Methods) / sizeof(gt::Methods[0]);
                constexpr size_t EventCount = sizeof(gt::Events) / sizeof(gt::Events[0]);
                constexpr size_t ClassCount = sizeof(gt::Classes) / sizeof(gt::Classes[0]);
                bindings = new (std::nothrow) Binding[PropCount + MethodCount + EventCount];
                makers = new (std::nothrow) Maker[ClassCount];
                if (bindings == nullptr || makers == nullptr) check(-1, "the GUI's bindings");
                for (size_t i = 0; i < PropCount; ++i) {
                        bindings[i].cls = gt::Props[i].cls;
                        bindings[i].name = gt::Props[i].name;
                        bindings[i].tag = effective(gt::Props[i].tag, gt::Props[i].name);
                }
                for (size_t i = 0; i < MethodCount; ++i) {
                        bindings[PropCount + i].cls = gt::Methods[i].cls;
                        bindings[PropCount + i].name = gt::Methods[i].name;
                        bindings[PropCount + i].method = &gt::Methods[i];
                }
                for (size_t i = 0; i < EventCount; ++i) {
                        Binding &b = bindings[PropCount + MethodCount + i];
                        b.cls = gt::Events[i].cls;
                        b.name = gt::Events[i].name;
                        b.event = &gt::Events[i];
                }

                // Rect: x, y, width, height.
                check(engine->RegisterObjectType("Rect", sizeof(ScriptRect),
                                                 asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS | asOBJ_APP_CLASS_ALLINTS),
                      "Rect");
                behaviour(engine, "Rect", asBEHAVE_CONSTRUCT, "void f()", rectConstruct, "An empty rectangle.");
                behaviour(engine, "Rect", asBEHAVE_CONSTRUCT, "void f(int x, int y, int width, int height)",
                          rectConstructWith, "A rectangle: its top left corner and its size.");
                check(engine->RegisterObjectProperty("Rect", "int x", asOFFSET(ScriptRect, x)), "Rect::x");
                check(engine->RegisterObjectProperty("Rect", "int y", asOFFSET(ScriptRect, y)), "Rect::y");
                check(engine->RegisterObjectProperty("Rect", "int width", asOFFSET(ScriptRect, width)), "Rect::width");
                check(engine->RegisterObjectProperty("Rect", "int height", asOFFSET(ScriptRect, height)),
                      "Rect::height");

                // The types first (members name each other), GuiObject the base of all.
                check(engine->RegisterObjectType("GuiObject", 0, asOBJ_REF), "GuiObject");
                behaviour(engine, "GuiObject", asBEHAVE_ADDREF, "void f()", refAdd, nullptr);
                behaviour(engine, "GuiObject", asBEHAVE_RELEASE, "void f()", refRelease, nullptr);
                method(engine, "GuiObject", "void destroy()", destroyObject,
                       "Destroys a GUI object, and the objects in it (a window's widgets).");
                for (const gt::ClassDef &c : gt::Classes) {
                        if (!bound(c)) continue;
                        check(engine->RegisterObjectType(c.name, 0, asOBJ_REF), c.name);
                        behaviour(engine, c.name, asBEHAVE_ADDREF, "void f()", refAdd, nullptr);
                        behaviour(engine, c.name, asBEHAVE_RELEASE, "void f()", refRelease, nullptr);
                }
                // The callbacks' funcdefs, one for each set of values.
                Text name, decl;
                check(engine->RegisterFuncdef("void Callback()"), "Callback");
                for (size_t i = 0; i < EventCount; ++i) {
                        const gt::EventDef &e = gt::Events[i];
                        if (e.count == 0) continue;
                        callbackName(e, name);
                        bool registered = false; // by an earlier event with the same values
                        for (size_t j = 0; j < i && !registered; ++j) {
                                Text other;
                                callbackName(gt::Events[j], other);
                                registered = gt::Events[j].count != 0 && strcmp(other.data(), name.data()) == 0;
                        }
                        if (registered) continue;
                        decl.clear();
                        decl.appendf("void %s(", name.data());
                        for (uint8_t v = 0; v < e.count; ++v)
                                decl.appendf("%s%s", v != 0 ? ", " : "", callbackType(e.values[v]));
                        decl.append(")");
                        check(engine->RegisterFuncdef(decl.data()), decl.data());
                }

                // app: the connection's loop and timers.
                check(engine->RegisterObjectType("App", 0, asOBJ_REF | asOBJ_NOCOUNT), "App");
                method(engine, "App", "void run()", appRun,
                       "Serves the GUI's events and the timers, calling their callbacks, until quit(), or until no "
                       "window is shown once one was.");
                method(engine, "App", "void quit()", appQuit, "Ends run() (after the callback that calls it).");
                method(engine, "App", "int after(uint ms, Callback@ callback)", appAfter,
                       "Calls back once, ms milliseconds from now, while run() runs: its timer's number.");
                method(engine, "App", "int every(uint ms, Callback@ callback)", appEvery,
                       "Calls back every ms milliseconds while run() runs: its timer's number.");
                method(engine, "App", "void stop(int timer)", appStop, "Stops a timer (after() or every()'s).");
                property(engine, "App app", &appObject,
                         "The program's GUI: its loop (run(), quit()) and timers (after(), every()).");

                // The one-line dialogs.
                def(engine, "void alert(const string &in text, const string &in title = \"\")", alertCall,
                    "Shows text in a message box (the GUI's) and waits for OK. The title is the script's "
                    "window's unless given.");
                def(engine, "bool ask(const string &in question, const string &in title = \"\")", askCall,
                    "Asks a question (the GUI's message box): true for Yes, false for No.");
                def(engine,
                    "string askText(const string &in prompt, const string &in text = \"\", const string &in title "
                    "= \"\")",
                    askTextCall, "Asks for a line of text (text to start from): what was typed, \"\" if cancelled.");
                def(engine, "string chooseFile(const string &in title = \"\")", chooseFileCall,
                    "Asks for a file to open (the GUI's file dialog): its path, \"\" if cancelled.");
                def(engine, "string chooseSaveFile(const string &in name = \"\", const string &in title = \"\")",
                    chooseSaveFileCall,
                    "Asks where to save a file (name: the one suggested): its path, \"\" if cancelled.");
                def(engine, "uint chooseColor(uint color = 0, const string &in title = \"\")", chooseColorCall,
                    "Asks for a colour (the GUI's colour dialog), starting at color: the one chosen, color if "
                    "cancelled.");
                // Files opened with their applications (the panel's `open` service).
                def(engine, "void openFile(const string &in path)", openFileCall,
                    "Opens a file as a double click on the desktop does: by its type's application (a folder in a "
                    "window, a program run).");
                return;
        }

        void registerGuiClasses(asIScriptEngine *engine, const char *const *texts, const size_t *lengths,
                                size_t count) {
                constexpr size_t ClassCount = sizeof(gt::Classes) / sizeof(gt::Classes[0]);
                // The classes the scripts name (a whole word anywhere: a comment's is harmless), with
                // their parents, and Window (an owner is one); all without scripts.
                bool used[ClassCount] = {};
                bool any = false;
                for (size_t ci = 0; ci < ClassCount; ++ci) {
                        const char  *name = gt::Classes[ci].name;
                        const size_t n = strlen(name);
                        bool         named = texts == nullptr;
                        for (size_t t = 0; t < count && !named; ++t) {
                                const char *text = texts[t];
                                for (size_t at = 0; at + n <= lengths[t] && !named; ++at) {
                                        if (memcmp(text + at, name, n) != 0) continue;
                                        const auto word = [](char ch) {
                                                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                                       (ch >= '0' && ch <= '9') || ch == '_';
                                        };
                                        named = (at == 0 || !word(text[at - 1])) &&
                                                (at + n == lengths[t] || !word(text[at + n]));
                                }
                        }
                        if (!named) continue;
                        any = true;
                        for (const gt::ClassDef *c = &gt::Classes[ci]; c != nullptr; c = parentOf(c))
                                used[c - gt::Classes] = true;
                }
                if (any)
                        for (size_t ci = 0; ci < ClassCount; ++ci)
                                if (strcmp(gt::Classes[ci].name, "Window") == 0) used[ci] = true;
                Text        decl;
                Text        help;
                static bool done[ClassCount]; // registered already (an editor's checks call again)
                for (size_t ci = 0; ci < ClassCount; ++ci) {
                        const gt::ClassDef &c = gt::Classes[ci];
                        if (!bound(c) || !used[ci] || done[ci]) continue;
                        done[ci] = true;
                        registerMembers(engine, c);
                        const char *T = c.name;
                        if (strcmp(T, "Canvas") == 0) registerCanvasDrawing(engine, T, &canvasHooks);
                        method(engine, T, "void destroy()", destroyObject,
                               "Destroys a GUI object, and the objects in it (a window's widgets).");
                        // As its parents and as GuiObject.
                        decl.clear();
                        decl.append("GuiObject@ opImplCast()");
                        methodAux(engine, T, decl.data(), implicitCast, nullptr, nullptr);
                        for (const gt::ClassDef *p = parentOf(&c); p != nullptr; p = parentOf(p)) {
                                decl.clear();
                                decl.appendf("%s@ opImplCast()", p->name);
                                methodAux(engine, T, decl.data(), implicitCast, nullptr, nullptr);
                        }
                        // Made: under a parent; with its text or title; a window with its title and size.
                        if (c.concrete) {
                                Maker &m = makers[ci];
                                m.cls = &c;
                                static constexpr const char *TextProperties[] = {"text", "title", "label"};
                                for (const char *prop : TextProperties) {
                                        if (m.textProperty != nullptr) break;
                                        for (const gt::ClassDef *k = &c; k != nullptr && m.textProperty == nullptr;
                                             k = parentOf(k))
                                                for (const gt::PropDef &p : gt::Props)
                                                        if (strcmp(p.cls, k->name) == 0 && strcmp(p.name, prop) == 0 &&
                                                            p.tag == gt::String && p.writable)
                                                                m.textProperty = prop;
                                }
                                if (strcmp(T, "Window") == 0) {
                                        behaviourAux(
                                                engine, T, asBEHAVE_FACTORY,
                                                "Window@ f(const string &in title = \"\", int width = 0, int height = "
                                                "0)",
                                                makeWindow, &m,
                                                "A window (the GUI's): its title, and its size (0: its contents'). "
                                                "Show it with show().");
                                        continue;
                                }
                                decl.clear();
                                decl.appendf("%s@ f(GuiObject@ parent = null)", T);
                                help.clear();
                                help.appendf("A %s (the GUI's) in parent (null: none).", T);
                                behaviourAux(engine, T, asBEHAVE_FACTORY, decl.data(), makePlain, &m, copyOf(help));
                                if (strcmp(T, "Box") == 0) {
                                        behaviourAux(engine, T, asBEHAVE_FACTORY,
                                                     "Box@ f(GuiObject@ parent, const string &in direction)", makeBox,
                                                     &m, "A box in parent, its widgets \"down\" or \"across\".");
                                } else if (m.textProperty != nullptr) {
                                        decl.clear();
                                        decl.appendf("%s@ f(GuiObject@ parent, const string &in %s)", T,
                                                     m.textProperty);
                                        help.clear();
                                        help.appendf("A %s (the GUI's) in parent, with its %s.", T, m.textProperty);
                                        behaviourAux(engine, T, asBEHAVE_FACTORY, decl.data(), makeWithText, &m,
                                                     copyOf(help));
                                }
                        }
                }
                return;
        }

        void finishGui(asIScriptEngine *engine) {
                theEngine = engine;
                // app.quit(): the script is done.
                if (((gui != nullptr && anyShown()) || screensServed() || sourcesHold() == LoopHold::RunAndEnd) &&
                    !quitCalled)
                        loop();
                return;
        }

        bool addLoopSource(LoopSource *source) {
                if (registered(source)) return true;
                if (sourceCount == MaxSources) {
                        raise("Too many things watched at once (%u)", unsigned(MaxSources));
                        return false;
                }
                sources[sourceCount++] = source;
                return true;
        }

        void removeLoopSource(LoopSource *source) {
                for (size_t i = 0; i < sourceCount; ++i) {
                        if (sources[i] != source) continue;
                        sources[i] = sources[--sourceCount];
                        return;
                }
                return;
        }

        void callScript(asIScriptFunction *fn, const CallArg *args, size_t count) {
                bool              nested = false;
                asIScriptContext *ctx = beginCall(nested);
                if (ctx == nullptr) return;
                String strings[4];
                int    r = count <= 4 ? ctx->Prepare(fn) : asINVALID_ARG;
                for (size_t i = 0; r >= 0 && i < count; ++i) {
                        if (args[i].text == nullptr) {
                                r = ctx->SetArgDWord(asUINT(i), uint32_t(args[i].number));
                        } else {
                                strings[i].assign(args[i].text, args[i].length);
                                r = ctx->SetArgAddress(asUINT(i), &strings[i]);
                        }
                }
                endCall(ctx, nested, r);
                return;
        }

        void releaseGui() {
                releaseProcesses();
                releaseServices();
                releaseFolders();
                releaseAudio();
                for (Timer &t : timers) stopTimer(t.id);
                for (size_t i = 0; i < iconCount; ++i) imageRelease(icons[i].image);
                free(icons);
                icons = nullptr;
                iconCount = 0;
                iconCapacity = 0;
                free(lastFolder);
                lastFolder = nullptr;
                while (objectCount != 0) forget(objects[objectCount - 1]->id);
                free(objects);
                objects = nullptr;
                objectCapacity = 0;
                if (ownContext != nullptr) ownContext->Release();
                ownContext = nullptr;
                if (gui != nullptr) bmds_gui_close(gui);
                gui = nullptr;
                return;
        }

} // namespace angel
