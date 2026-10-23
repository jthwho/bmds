/**
 * @file      guiconnection.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/filehandle.h>
#include <bmds/guiapplication.h>
#include <bmds/guiprotocol.h>
#include <bmds/guiwindow.h>
#include <bmds/icontheme.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/thread.h>
#include <bmds/widget.h>
#include <bmds/windowplacement.h>

BMDS_NAMESPACE_BEGIN

class GuiConnection;
struct GuiObject;

/** @brief 0, or a `GuiProtocol::ErrorCode`: what a class's code answers. */
using GuiStatus = uint16_t;

/** @brief The schema's entries (`sdk/gui/classes.def`, guiclasses.cpp): what a class has. */
using GuiSetter = GuiStatus (*)(GuiObject &object, const GuiProtocol::Value &value);
using GuiGetter = GuiProtocol::Value (*)(GuiObject &object); ///< Its text stays valid until the next change.
using GuiCaller = GuiStatus (*)(GuiObject &object, Span<const GuiProtocol::Value> args, GuiProtocol::Value &result);
using GuiCreator = GuiStatus (*)(GuiObject &object, GuiObject *parent);

/** @brief The most arguments a method takes, or values an event carries. */
constexpr size_t GuiMaxArgs = 6;

/** @brief A class. */
struct GuiClass {
                const char *name;
                const char *parent; ///< "None": none.
                GuiCreator  create; ///< Null: it can't be made (a base).
};

/** @brief A property. */
struct GuiProperty {
                const char      *cls;
                const char      *name;
                GuiProtocol::Tag tag;
                GuiSetter        set; ///< Null: read-only.
                GuiGetter        get; ///< Null: write-only.
};

/** @brief A method: its result's tag and its arguments' (checked before it runs). */
struct GuiMethod {
                const char      *cls;
                const char      *name;
                GuiProtocol::Tag result;
                GuiProtocol::Tag args[GuiMaxArgs];
                uint8_t          count;
                GuiCaller        call;
};

/** @brief An event. */
struct GuiEvent {
                const char      *cls;
                const char      *name;
                bool             byDefault; ///< Sent unless the client turns it off (`Listen`).
                GuiProtocol::Tag values[GuiMaxArgs];
                uint8_t          count;
};

/** @brief An Atom property's names ("across, down"). */
struct GuiValues {
                const char *cls;
                const char *property;
                const char *names;
};

/**
 * @brief A theme icon by name, its pixels copied (the theme's cache may drop its own). Two
 *        slots: a new icon goes in the other, and the one before is freed by `dropPrevious()`
 *        once the new one is in use (the window manager may be drawing the old).
 */
class GuiIcon {
        public:
                GuiIcon() = default;
                ~GuiIcon();
                GuiIcon(const GuiIcon &) = delete;
                GuiIcon &operator=(const GuiIcon &) = delete;

                /** @brief Takes icon @p name of @p side pixels from @p theme (empty: none). @return False: no such icon. */
                bool set(IconTheme &theme, StringView name, uint16_t side);

                /** @brief Frees the icon before the current one (no longer in use). */
                void dropPrevious();

                /** @brief The icon (null: none). */
                const Icon *icon() const { return _name.isEmpty() ? nullptr : &_icons[_current]; }
                StringView  name() const { return _name.view(); }

        private:
                Icon      _icons[2];
                uint32_t *_pixels[2] = {};
                uint8_t   _current = 0;
                String    _name{MemSpace::Psram};
};

/** @brief A window made by a client: a column for its children; its close told. */
class ClientWindow final : public GuiWindow {
        public:
                ClientWindow(GuiApplication &app, const Config &config, GuiObject &object);
                ~ClientWindow() override;

                BoxLayout *column() const { return _column; }

                /** @brief Its icon: theme icon @p name's 16 × 16, copied (empty: none). @return False if none. */
                bool       setIconNamed(StringView name);
                StringView iconName() const { return _icon.name(); }

                /** @brief Its window menu's own item (empty: none). */
                void       setMenuItem(StringView label);
                StringView menuItem() const { return _menuItem.view(); }

                /** @brief Blocks its owner's widgets while it is shown and modal, or not (after a change). */
                void updateBlock();

                WindowPlacement placementWanted; ///< Set before it is shown: where (empty rectangle: centred).
                uint32_t        ownerId = 0;     ///< The window it belongs to (0: none).
                bool            modal = false;
                bool            above = false;

        protected:
                void closedByManager() override;
                void closeRequested() override;
                void resized() override;
                void activeChanged() override;

        private:
                static void menuItemChosen(void *arg); // the window manager's task
                static void menuItemPosted(void *arg); // the loop's

                GuiObject &_object;
                BoxLayout *_column = nullptr;
                GuiIcon    _icon;
                String     _menuItem{MemSpace::Psram};
                uint16_t   _toldWidth = 0; // the size last sent with `resized`
                uint16_t   _toldHeight = 0;
                bool       _blocking = false; // its owner's widgets are disabled for it
};

/** @brief An object a client made, by its id. */
struct GuiObject {
                uint32_t        id = 0;
                uint32_t        parent = 0; ///< Its parent's id, or 0.
                const GuiClass *cls = nullptr;
                GuiConnection  *connection = nullptr;
                Widget         *widget = nullptr;    ///< A widget's (a window's: null).
                ClientWindow   *window = nullptr;    ///< A window's.
                Widget         *container = nullptr; ///< Where its children go (a window's column, a box), or null.
                ClientWindow   *owner = nullptr;     ///< The window it is in (itself, for a window), or null.
                uint16_t        width = 0;           ///< A window's client area at first (0: its children's size).
                uint16_t        height = 0;
                uint8_t         blocked = 0; ///< A window's: modal windows shown over it (its widgets disabled).
                uint8_t  frameWanted = WindowFrame::Standard; ///< A window's frame as set (`WindowFrame`'s flags).
                uint16_t minWidth = 0;                        ///< A window's least client area.
                uint16_t minHeight = 0;
                String   text{MemSpace::Psram};             ///< A getter's made text (valid until the next).
                GuiIcon         icon;                              ///< A widget's icon by name (a tool button's).
                int32_t  minimum = 0;   ///< A range's (a number field's, a slider's; a splitter's first side's least).
                int32_t  maximum = 100; ///< (A splitter's second side's least.)
                int32_t  step = 1;
                int64_t  longMaximum = 0; ///< A progress bar's.
                uint32_t first = 0;       ///< A splitter's sides (ids; 0: none).
                uint32_t second = 0;
                List<uint8_t> items{MemSpace::Psram}; ///< A getter's made list (valid until the next).

                /** @name A container's (null: a plain one) */
                ///@{
                GuiStatus (*adopt)(GuiObject &self, GuiObject &child) = nullptr; ///< A child made in it.
                void (*release)(GuiObject &self, GuiObject &child) = nullptr;    ///< A child going.
                void (*relayout)(GuiObject &self) = nullptr; ///< After a round of requests that changed it.
                void (*dispose)(GuiObject &self) = nullptr;  ///< After its widget is destroyed: `data` freed.
                void   *data = nullptr;                      ///< A class's own (a view's model).
                int32_t told = -1;                           ///< A view's current row as last told.
                bool    quiet = false;                       ///< A change the client made: not told back.
                String  path{MemSpace::Psram};               ///< A tree's current path as last told.
                ///@}
                List<uint32_t>  listenOn{MemSpace::Psram};  ///< Events turned on (atoms).
                List<uint32_t>  listenOff{MemSpace::Psram}; ///< And off.
};

/**
 * @brief One client's connection (`devplan/guiprograms.md` §4a, §8): its requests read and run,
 *        its objects, its answers and events written.
 *
 * Two threads: the reader frames requests from the stream and queues them; the loop
 * (`GuiApplication::exec()`) runs them, as handlers run, and writes the answers. The connection
 * frees itself when its client goes (the stream's end) or the window system stops.
 *
 * @par Thread Safety
 * The loop's thread, but for the reader's queue (its lock).
 */
class GuiConnection {
        public:
                /** @brief Starts a connection on @p end (the service's end of the stream), as slot @p slot. */
                static Error start(FileHandle::Ptr end, uint8_t slot, uint32_t display, StringView program);

                /** @brief Sends @p event of @p object with @p values, if the client listens to it. */
                void emit(GuiObject &object, const char *event, Span<const GuiProtocol::Value> values);

                /** @brief True if the client listens to @p object's @p event. */
                bool listens(const GuiObject &object, const char *event) const;

                /** @brief A shown modal window owned by window @p ownerId, or null. */
                ClientWindow *modalOver(uint32_t ownerId) const;

                /** @brief Theme icon @p name of @p side pixels, a copy kept while the connection lasts, or null. */
                const Icon *icon(StringView name, uint16_t side);

                /** @brief Its object @p id, or null. */
                GuiObject *find(uint32_t id) const;

                /** @brief The window @p object is in changed: laid out again after this round of requests. */
                void touched(ClientWindow *window);

                GuiApplication &application() { return *_app; }

        private:
                GuiConnection(FileHandle::Ptr end, uint8_t slot, uint32_t display);
                ~GuiConnection();

                static void loopMain(void *arg);
                static void readerMain(void *arg);
                static void drainDue(void *arg);
                static void endDue(void *arg);

                void drain();
                void handle(ByteSpan message);
                void hello(GuiProtocol::Reader &body);
                void internNames(GuiProtocol::Reader &body);
                void nameOf(GuiProtocol::Reader &body);
                void create(GuiProtocol::Reader &body);
                void destroy(GuiProtocol::Reader &body);
                void set(GuiProtocol::Reader &body);
                void get(GuiProtocol::Reader &body);
                void call(GuiProtocol::Reader &body, uint16_t flags);
                void listen(GuiProtocol::Reader &body);
                void describe(GuiProtocol::Reader &body);
                void sync();
                void layOut();
                void fail(GuiProtocol::ErrorCode code, uint32_t bad);
                void send();
                void stop();

                Error      add(GuiObject *object);
                void       remove(GuiObject *object); // with its children, the toolkit's side too
                bool       ownsId(uint32_t id) const { return (id & ~IdMask) == idBase(); }
                uint32_t   idBase() const { return uint32_t(_slot + 1) << 24; }

                static constexpr uint32_t IdMask = 0x00ffffffu;

                FileHandle::Ptr      _end;
                uint8_t              _slot;
                uint32_t             _display;
                String               _program{MemSpace::Psram};
                GuiApplication      *_app = nullptr;
                Thread               _loop;
                Thread               _reader;
                Mutex                _lock;                     // _pending
                List<uint8_t>        _pending{MemSpace::Psram}; // whole requests, as read
                Semaphore            _room{1, 0};               // the loop took requests: the reader may read again
                Semaphore            _started{1, 0};            // start() is done with _loop: the loop may run
                std::atomic<bool>    _posted{false};
                std::atomic<bool>    _stopping{false};
                bool                 _helloDone = false;
                uint32_t             _sequence = 0;             // the request being run
                uint16_t             _opcode = 0;               // and its opcode
                List<GuiObject *>    _objects{MemSpace::Psram}; // sorted by id
                List<ClientWindow *> _dirty{MemSpace::Psram};
                List<uint8_t>        _out{MemSpace::Psram}; // answers not yet written

                struct KeptIcon {
                                String    name{MemSpace::Psram};
                                uint16_t  side = 0;
                                Icon      icon;
                                uint32_t *pixels = nullptr;
                };
                static constexpr size_t MaxIcons = 64;
                List<KeptIcon *>        _icons{MemSpace::Psram};
};

/** @brief The class named @p name, or null. */
const GuiClass *guiClass(StringView name);

/** @brief @p cls's parent class, or null. */
const GuiClass *guiParent(const GuiClass *cls);

/** @brief The property named @p atom of @p cls or its parents, or null. */
const GuiProperty *guiProperty(const GuiClass *cls, uint32_t atom);

/** @brief The method named @p atom of @p cls or its parents, or null. */
const GuiMethod *guiMethod(const GuiClass *cls, uint32_t atom);

/** @brief The event named @p atom of @p cls or its parents, or null. */
const GuiEvent *guiEvent(const GuiClass *cls, uint32_t atom);

/** @brief True if @p atom is one of @p property's names (or it has no list of names). */
bool guiAllowed(const GuiProperty &property, uint32_t atom);

/** @brief The schema's tables, whole (`Describe`). */
Span<const GuiProperty> guiProperties();
Span<const GuiMethod>   guiMethods();
Span<const GuiEvent>    guiEvents();

BMDS_NAMESPACE_END
