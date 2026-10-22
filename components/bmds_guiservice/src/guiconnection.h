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
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/thread.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class GuiConnection;
struct GuiObject;

/** @brief 0, or a `GuiProtocol::ErrorCode`: what a class's code answers. */
using GuiStatus = uint16_t;

/** @brief A property of a class (`devplan/guiprograms.md` §4b). */
struct GuiProperty {
                const char      *name;
                GuiProtocol::Tag tag;
                GuiStatus (*set)(GuiObject &object, const GuiProtocol::Value &value); ///< Null: read-only.
                GuiProtocol::Value (*get)(
                        GuiObject &object); ///< Null: write-only. Its text stays valid until the next change.
};

/** @brief A method: its arguments read from @p args (@p count of them), its result into @p result. */
struct GuiMethod {
                const char *name;
                GuiStatus (*call)(GuiObject &object, GuiProtocol::Reader &args, uint32_t count,
                                  GuiProtocol::Value &result);
};

/** @brief An event a class sends. */
struct GuiEvent {
                const char *name;
                bool        byDefault; ///< Sent unless the client turns it off (`Listen`).
};

/** @brief A class: its parent, its names, and how an object of it is made. */
struct GuiClass {
                const char             *name;
                const GuiClass         *parent;
                Span<const GuiProperty> properties;
                Span<const GuiMethod>   methods;
                Span<const GuiEvent>    events;
                /** @brief Makes the toolkit's side under @p parent (null: none). Null: the class can't be made (a base). */
                GuiStatus (*create)(GuiObject &object, GuiObject *parent);
};

/** @brief A window made by a client: a column for its children; its close told. */
class ClientWindow final : public GuiWindow {
        public:
                ClientWindow(GuiApplication &app, const Config &config, GuiObject &object);
                ~ClientWindow() override;

                BoxLayout *column() const { return _column; }

        protected:
                void closedByManager() override;

        private:
                GuiObject &_object;
                BoxLayout *_column = nullptr;
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
                bool            across = false; ///< A box's direction (read from `Create` before it is made).
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
                void sync();
                void layOut();
                void fail(GuiProtocol::ErrorCode code, uint32_t bad);
                void send();
                void stop();

                GuiObject *find(uint32_t id) const;
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
};

/** @brief The classes (guiclasses.cpp): by name. */
const GuiClass *guiClass(StringView name);

/** @brief The property named @p atom of @p cls or its parents, or null. */
const GuiProperty *guiProperty(const GuiClass *cls, uint32_t atom);

/** @brief The method named @p atom of @p cls or its parents, or null. */
const GuiMethod *guiMethod(const GuiClass *cls, uint32_t atom);

/** @brief The event named @p atom of @p cls or its parents, or null. */
const GuiEvent *guiEvent(const GuiClass *cls, uint32_t atom);

BMDS_NAMESPACE_END
