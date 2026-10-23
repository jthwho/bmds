/**
 * @file      guiconnection.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One client's connection to the GUI service (devplan/guiprograms.md §4a, §8): its requests
 * framed by a reader thread and run on its application's loop, its objects, its answers.
 */

#include "guiconnection.h"

#include <cstring>
#include <new>
#include <bmds/guiservice.h>
#include <bmds/memory.h>
#include <bmds/system.h>
#include "guiatoms.h"
#include "guiservicepriv.h"

BMDS_NAMESPACE_BEGIN

namespace {

        namespace GP = GuiProtocol;
        using GP::ErrorCode;
        using GP::Opcode;
        using GP::Tag;
        using GP::Value;

        constexpr size_t       LoopStackBytes = 16384;
        constexpr size_t       ReaderStackBytes = 4096;
        constexpr unsigned int Priority = 5;
        constexpr size_t       MaxPending = 256u * 1024u; // unread requests the reader holds at most
        constexpr unsigned int WriteTimeoutMs = 10000;    // a client that reads nothing this long is let go
        constexpr unsigned int ReadSliceMs = 200;         // the reader looks at `_stopping` this often
        constexpr uint32_t     MaxNames = 4096;           // in one InternNames, NameOf or Get
        constexpr uint32_t     MaxImageSide = 512;        // an Image's width and height at most (1 MB)
        constexpr uint32_t     MaxProperties = 256;       // in one Create or Set
        constexpr size_t       MaxObjects = 4096;

        // The connections by slot, for local clients' system calls (canvasSurface()); its lock also
        // each connection's surface jobs. Made at boot, never freed.
        alignas(Mutex) uint8_t registryMemory[sizeof(Mutex)];
        Mutex         *registryLock = nullptr;
        GuiConnection *registry[GuiService::MaxConnections] = {};

        // The program's name as people see it: its file's, without `.elf` (notifications, `clip list`).
        StringView programName(StringView path) {
                const size_t     slash = path.lastIndexOf('/');
                const StringView name = slash == StringView::NPos ? path : path.mid(slash + 1);
                return name.endsWith(StringView(".elf")) && name.size() > 4 ? name.left(name.size() - 4) : name;
        }

        // A property and its value from a request (its text points into the request).
        struct Assignment {
                        uint32_t           atom;
                        const GuiProperty *property;
                        Value              value;
        };

        // A value of @p tag given for a property or an argument of @p want: the same tag (a boolean's 0
        // or 1 checked by the reader), or text where bytes are wanted.
        bool fits(Tag want, const Value &v) {
                return v.tag == want || (want == Tag::Bytes && v.tag == Tag::String);
        }

} // namespace

GuiConnection::GuiConnection(FileHandle::Ptr end, Stream *clientEnd, uint8_t slot, uint32_t display)
    : _end(std::move(end)), _clientEnd(clientEnd), _slot(slot), _display(display) {}

GuiConnection::~GuiConnection() {
        for (KeptIcon *k : _icons) {
                Memory::free(k->pixels);
                k->~KeptIcon();
                Memory::free(k);
        }
}

// Rows' icons by name: copied once and kept while the connection lasts (a theme's cache may drop
// its own, and a view's rows hold the pointers).
const Icon *GuiConnection::icon(StringView name, uint16_t side) {
        for (KeptIcon *k : _icons)
                if (k->side == side && k->name.view() == name) return &k->icon;
        if (_icons.size() >= MaxIcons) return nullptr;
        const Icon *found = _app->icons().find(name, side);
        if (found == nullptr || !found->isValid()) return nullptr;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(KeptIcon));
        if (mem == nullptr) return nullptr;
        auto        *k = ::new (mem) KeptIcon();
        const size_t bytes = size_t(found->width) * found->height * sizeof(uint32_t);
        k->pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, bytes));
        if (k->pixels == nullptr || k->name.assign(name).isError() || _icons.append(std::move(k)).isError()) {
                Memory::free(k->pixels);
                k->~KeptIcon();
                Memory::free(k);
                return nullptr;
        }
        std::memcpy(k->pixels, found->pixels, bytes);
        k->side = side;
        k->icon = Icon{found->width, found->height, k->pixels};
        return &k->icon;
}

Error GuiConnection::start(FileHandle::Ptr end, Stream *clientEnd, uint8_t slot, uint32_t display, StringView program) {
        if (slot >= GuiService::MaxConnections) return Error::Invalid;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(GuiConnection));
        if (mem == nullptr) return Error::NoMem;
        auto *self = ::new (mem) GuiConnection(std::move(end), clientEnd, slot, display);
        (void)self->_program.assign(program);
        Thread::Config config;
        config.name = "gui.conn";
        config.stackBytes = LoopStackBytes;
        config.priority = Priority;
        config.kernel = true;
        const Error err = self->_loop.start(config, loopMain, self);
        if (err.isError()) {
                self->~GuiConnection();
                Memory::free(self);
                return err;
        }
        self->_started.release(); // the loop may run, and end, from here
        return Error::Ok;
}

void GuiConnection::initRegistry() {
        if (registryLock == nullptr) registryLock = ::new (registryMemory) Mutex();
        return;
}

// ---- Canvas surfaces (CANVAS-SURFACE, devplan/guiprograms.md §11 3c)

Result<FileHandle::Ptr> GuiConnection::canvasSurface(Stream &clientEnd, uint32_t canvas,
                                                     const GuiOpener::SurfaceConfig &config) {
        if (registryLock == nullptr) return makeError<FileHandle::Ptr>(Error::NotSupported);
        SurfaceJob job;
        job.canvas = canvas;
        job.config = &config;
        {
                Mutex::Locker  locker(*registryLock, WaitForever);
                GuiConnection *self = nullptr;
                for (GuiConnection *c : registry)
                        if (c != nullptr && c->_clientEnd == &clientEnd) self = c;
                if (self == nullptr) return makeError<FileHandle::Ptr>(Error::NotSupported);
                // After the jobs before it; the loop posted to when the list was empty.
                SurfaceJob **tail = &self->_surfaceJobs;
                while (*tail != nullptr) tail = &(*tail)->next;
                *tail = &job;
                if (tail == &self->_surfaceJobs && self->_app->post(surfacesDue, self).isError()) {
                        self->_surfaceJobs = nullptr;
                        return makeError<FileHandle::Ptr>(Error::Busy);
                }
        }
        // The loop runs it, or refuses it as the connection ends (closeSurfaces()).
        (void)job.done.acquire(WaitForever);
        if (job.err.isError()) return makeError<FileHandle::Ptr>(job.err);
        return makeResult(std::move(job.handle));
}

// The loop's: the surfaces asked for, made in order.
void GuiConnection::surfacesDue(void *arg) {
        auto       *self = static_cast<GuiConnection *>(arg);
        SurfaceJob *jobs;
        {
                Mutex::Locker locker(*registryLock, WaitForever);
                jobs = self->_surfaceJobs;
                self->_surfaceJobs = nullptr;
        }
        while (jobs != nullptr) {
                SurfaceJob *next = jobs->next; // done released, the job is gone
                GuiObject  *o = self->find(jobs->canvas);
                if (o == nullptr) {
                        jobs->err = Error::NotExist;
                } else {
                        auto [handle, err] = guiCanvasSurface(*o, *jobs->config);
                        jobs->err = err;
                        jobs->handle = std::move(handle);
                }
                jobs->done.release();
                jobs = next;
        }
        return;
}

void GuiConnection::closeSurfaces() {
        SurfaceJob *jobs = nullptr;
        if (registryLock != nullptr) {
                Mutex::Locker locker(*registryLock, WaitForever);
                if (registry[_slot] == this) registry[_slot] = nullptr;
                jobs = _surfaceJobs;
                _surfaceJobs = nullptr;
        }
        while (jobs != nullptr) {
                SurfaceJob *next = jobs->next;
                jobs->err = Error::NotSupported; // the connection is ending
                jobs->done.release();
                jobs = next;
        }
        return;
}

// The loop's thread: the application, the reader, the requests run; then everything let go.
void GuiConnection::loopMain(void *arg) {
        auto *self = static_cast<GuiConnection *>(arg);
        (void)self->_started.acquire(WaitForever); // start() has recorded the thread
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
        if (mem != nullptr) {
                self->_app = ::new (mem) GuiApplication();
                self->_app->setApplicationName(programName(self->_program.view()));
                if (self->_app->connect(self->_display).isOk()) {
                        if (registryLock != nullptr) { // its system calls post to the app from here
                                Mutex::Locker locker(*registryLock, WaitForever);
                                registry[self->_slot] = self;
                        }
                        Thread::Config config;
                        config.name = "gui.read";
                        config.stackBytes = ReaderStackBytes;
                        config.priority = Priority;
                        config.kernel = true;
                        if (self->_reader.start(config, readerMain, self).isOk()) {
                                (void)self->_app->exec();
                                self->_stopping.store(true, std::memory_order_release);
                                self->_room.release();
                                self->_reader.join();
                        }
                }
                self->closeSurfaces(); // before the objects (and the app's posts) go
                // The objects go, windows first closing as their objects do.
                while (!self->_objects.isEmpty()) self->remove(self->_objects[self->_objects.size() - 1]);
                if (self->_notifier != nullptr) { // its notifications stay
                        self->_notifier->~Notifier();
                        Memory::free(self->_notifier);
                        self->_notifier = nullptr;
                }
                self->_app->cancelPosts(self);
                self->_app->~GuiApplication();
                Memory::free(self->_app);
        }
        self->closeSurfaces();
        const uint8_t slot = self->_slot;
        self->_end.reset(); // the client reads the end
        // It frees itself: detached last, from its own thread (detaching at once raced start()'s
        // record of it, which an assertion caught on the board).
        self->_loop.detach();
        self->~GuiConnection();
        Memory::free(self);
        GuiServicePriv::released(slot);
        return;
}

// The reader's thread: whole requests from the stream, queued for the loop.
void GuiConnection::readerMain(void *arg) {
        auto         *self = static_cast<GuiConnection *>(arg);
        List<uint8_t> partial(MemSpace::Psram);
        uint8_t       buf[1024];
        while (!self->_stopping.load(std::memory_order_acquire)) {
                auto [n, err] = self->_end->read(WritableByteSpan(buf, sizeof(buf)), ReadSliceMs);
                if (err == Error::Timeout) continue;
                if (err.isError() || n == 0) break; // the client went
                bool bad = partial.append(Span<const uint8_t>(buf, n)).isError();
                // Whole messages to the loop, in order.
                for (;;) {
                        const int64_t length =
                                GP::messageLength(ByteSpan(partial.data(), partial.size()), GP::RequestHeaderBytes);
                        if (length < 0) bad = true; // out of step: the connection ends
                        if (bad || length == 0 || partial.size() < size_t(length)) break;
                        for (;;) {
                                {
                                        Mutex::Locker locked(self->_lock, WaitForever);
                                        if (self->_pending.size() + size_t(length) <= MaxPending ||
                                            self->_pending.isEmpty()) {
                                                bad = self->_pending
                                                              .append(Span<const uint8_t>(partial.data(),
                                                                                          size_t(length)))
                                                              .isError();
                                                break;
                                        }
                                }
                                // Full: the loop takes what waits, then says so.
                                (void)self->_room.acquire(ReadSliceMs);
                                if (self->_stopping.load(std::memory_order_acquire)) return;
                        }
                        std::memmove(partial.data(), partial.data() + length, partial.size() - size_t(length));
                        partial.truncate(partial.size() - size_t(length));
                        if (!self->_posted.exchange(true, std::memory_order_acq_rel))
                                (void)self->_app->post(drainDue, self);
                }
                if (bad) break;
        }
        (void)self->_app->post(endDue, self);
        return;
}

void GuiConnection::drainDue(void *arg) {
        static_cast<GuiConnection *>(arg)->drain();
        return;
}

void GuiConnection::endDue(void *arg) {
        static_cast<GuiConnection *>(arg)->stop();
        return;
}

void GuiConnection::stop() {
        _stopping.store(true, std::memory_order_release);
        _app->quit(0);
        return;
}

// The requests waiting, run in order; then the answers written and the windows laid out.
void GuiConnection::drain() {
        _posted.store(false, std::memory_order_release);
        List<uint8_t> requests(MemSpace::Psram);
        {
                Mutex::Locker locked(_lock, WaitForever);
                requests = std::move(_pending);
                _pending = List<uint8_t>(MemSpace::Psram);
        }
        _room.release();
        size_t at = 0;
        while (at < requests.size() && !_stopping.load(std::memory_order_acquire)) {
                const ByteSpan rest(requests.data() + at, requests.size() - at);
                const int64_t  length = GP::messageLength(rest, GP::RequestHeaderBytes); // framed by the reader
                if (length <= 0) break;
                handle(rest.first(size_t(length)));
                at += size_t(length);
        }
        layOut();
        send();
        return;
}

// ---- Requests

void GuiConnection::handle(ByteSpan message) {
        GP::Reader     header(message);
        const uint16_t opcode = header.u16();
        const uint16_t flags = header.u16();
        GP::Reader     body(message.mid(GP::RequestHeaderBytes));
        if (!_helloDone) {
                if (opcode != uint16_t(Opcode::Hello)) {
                        stop(); // not a GUI client
                        return;
                }
                hello(body);
                return;
        }
        ++_sequence;
        _opcode = opcode;
        switch (Opcode(opcode)) {
                case Opcode::InternNames: internNames(body); break;
                case Opcode::NameOf: nameOf(body); break;
                case Opcode::Create: create(body); break;
                case Opcode::Destroy: destroy(body); break;
                case Opcode::Set: set(body); break;
                case Opcode::Get: get(body); break;
                case Opcode::Call: call(body, flags); break;
                case Opcode::Listen: listen(body); break;
                case Opcode::Describe: describe(body); break;
                case Opcode::Sync: sync(); break;
                case Opcode::PutImage: putImage(body); break;
                case Opcode::PutPixels: putPixels(body); break;
                default: fail(ErrorCode::BadRequest, opcode); break;
        }
        return;
}

void GuiConnection::fail(ErrorCode code, uint32_t bad) {
        GP::Writer w(_out);
        w.beginAnswer(GP::Kind::Error, uint16_t(code), _sequence);
        w.u32(_opcode);
        w.u32(bad);
        w.end();
        return;
}

void GuiConnection::hello(GP::Reader &body) {
        const ByteSpan magic = body.bytes(sizeof(GP::Magic));
        const uint16_t major = body.u16();
        (void)body.u16(); // the client's minor: ours is answered
        const uint32_t nameLength = body.u32();
        const uint32_t dataLength = body.u32();
        (void)body.bytes(nameLength); // local: no authentication
        (void)body.bytes(dataLength);
        if (!body.ok() || std::memcmp(magic.data(), GP::Magic, sizeof(GP::Magic)) != 0) {
                stop();
                return;
        }
        GP::Writer w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::Hello), 0);
        if (major != GP::Major) {
                w.u32(uint32_t(GP::HelloStatus::BadVersion));
                w.u16(GP::Major);
                w.u16(GP::Minor);
                w.end();
                send();
                stop();
                return;
        }
        w.u32(uint32_t(GP::HelloStatus::Accepted));
        w.u16(GP::Major);
        w.u16(GP::Minor);
        w.u32(idBase());
        w.u32(IdMask);
        w.u32(GP::MaxMessage);
        // The extensions (their first opcode and event code 0: classes and a system call, not
        // requests): the clipboard; a local client's canvases take surfaces.
        w.u32(_clientEnd != nullptr ? 2 : 1);
        w.string("CLIPBOARD");
        w.string("1");
        w.u32(0);
        w.u32(0);
        if (_clientEnd != nullptr) {
                w.string("CANVAS-SURFACE");
                w.string("1");
                w.u32(0);
                w.u32(0);
        }
        // The display, as named values (more are appended).
        WindowManager &wm = _app->windowManager();
        const Rect     screen = wm.screen(), work = wm.workArea();
        w.u32(2);
        GP::Value r;
        r.tag = Tag::Rect;
        w.string("screen");
        r.rect = {screen.x, screen.y, screen.width, screen.height};
        w.value(r);
        w.string("workArea");
        r.rect = {work.x, work.y, work.width, work.height};
        w.value(r);
        w.end();
        _helloDone = true;
        return;
}

void GuiConnection::internNames(GP::Reader &body) {
        const uint32_t count = body.u32();
        if (!body.ok() || count > MaxNames) {
                fail(ErrorCode::BadLength, count);
                return;
        }
        List<uint32_t> atoms(MemSpace::Psram);
        for (uint32_t i = 0; i < count; ++i) {
                const StringView name = body.string();
                if (!body.ok()) {
                        fail(ErrorCode::BadLength, i);
                        return;
                }
                const uint32_t atom = GuiAtoms::intern(name);
                if (atom == 0 || atoms.append(atom).isError()) {
                        fail(atom == 0 && !name.isEmpty() && name.size() <= GuiAtoms::MaxName ? ErrorCode::NoMem
                                                                                              : ErrorCode::BadValue,
                             i);
                        return;
                }
        }
        GP::Writer w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::InternNames), _sequence);
        w.u32(count);
        for (uint32_t a : atoms) w.u32(a);
        w.end();
        return;
}

void GuiConnection::nameOf(GP::Reader &body) {
        const uint32_t count = body.u32();
        if (!body.ok() || count > MaxNames || body.remaining() < size_t(count) * 4) {
                fail(ErrorCode::BadLength, count);
                return;
        }
        const size_t start = _out.size();
        GP::Writer   w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::NameOf), _sequence);
        w.u32(count);
        for (uint32_t i = 0; i < count; ++i) {
                const uint32_t   atom = body.u32();
                const StringView name = GuiAtoms::name(atom);
                if (name.isEmpty()) {
                        _out.truncate(start); // the answer begun goes
                        fail(ErrorCode::BadName, atom);
                        return;
                }
                w.string(name);
        }
        w.end();
        return;
}

// The properties of a Create or Set: each named, of its class, writable, of its type.
static bool readAssignments(GP::Reader &body, const GuiClass *cls, List<Assignment> &out, ErrorCode &code,
                            uint32_t &bad) {
        const uint32_t count = body.u32();
        if (!body.ok() || count > MaxProperties) {
                code = ErrorCode::BadLength;
                bad = count;
                return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
                Assignment a;
                a.atom = body.u32();
                if (!body.value(a.value)) {
                        code = ErrorCode::BadLength;
                        bad = i;
                        return false;
                }
                a.property = guiProperty(cls, a.atom);
                if (a.property == nullptr) {
                        code = ErrorCode::BadName;
                        bad = a.atom;
                        return false;
                }
                if (a.property->set == nullptr) {
                        code = ErrorCode::BadAccess;
                        bad = a.atom;
                        return false;
                }
                if (!fits(a.property->tag, a.value) ||
                    (a.value.tag == Tag::Atom && !guiAllowed(*a.property, uint32_t(a.value.number)))) {
                        code = ErrorCode::BadValue;
                        bad = a.atom;
                        return false;
                }
                if (out.append(a).isError()) {
                        code = ErrorCode::NoMem;
                        bad = 0;
                        return false;
                }
        }
        return true;
}

void GuiConnection::create(GP::Reader &body) {
        const uint32_t id = body.u32();
        const uint32_t clsAtom = body.u32();
        const uint32_t parentId = body.u32();
        if (!body.ok()) {
                fail(ErrorCode::BadLength, 0);
                return;
        }
        if (!ownsId(id) || (id & IdMask) == 0 || find(id) != nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        const GuiClass *cls = guiClass(GuiAtoms::name(clsAtom));
        if (cls == nullptr || cls->create == nullptr) {
                fail(ErrorCode::BadClass, clsAtom);
                return;
        }
        GuiObject *parent = nullptr;
        if (parentId != 0) {
                parent = find(parentId);
                if (parent == nullptr) {
                        fail(ErrorCode::BadId, parentId);
                        return;
                }
        }
        if (_objects.size() >= MaxObjects) {
                fail(ErrorCode::NoMem, id);
                return;
        }
        List<Assignment> props(MemSpace::Psram);
        ErrorCode        code = ErrorCode::BadValue;
        uint32_t         bad = 0;
        if (!readAssignments(body, cls, props, code, bad)) {
                fail(code, bad);
                return;
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(GuiObject));
        if (mem == nullptr) {
                fail(ErrorCode::NoMem, id);
                return;
        }
        auto *o = ::new (mem) GuiObject();
        o->id = id;
        o->parent = parentId;
        o->cls = cls;
        o->connection = this;
        const GuiStatus made = cls->create(*o, parent);
        if (made != 0 || add(o).isError()) {
                if (made == 0) { // made, but not recorded: let go again
                        if (o->window != nullptr) {
                                o->window->~ClientWindow();
                                Memory::free(o->window);
                        } else {
                                if (parent != nullptr && parent->release != nullptr) parent->release(*parent, *o);
                                if (o->widget != nullptr) o->widget->destroy();
                                if (o->dispose != nullptr) o->dispose(*o);
                        }
                }
                o->~GuiObject();
                Memory::free(o);
                fail(made != 0 ? ErrorCode(made) : ErrorCode::NoMem, id);
                return;
        }
        for (const Assignment &a : props) {
                const GuiStatus s = a.property->set(*o, a.value);
                if (s != 0) {
                        fail(ErrorCode(s), a.atom);
                        return;
                }
        }
        return;
}

void GuiConnection::destroy(GP::Reader &body) {
        const uint32_t id = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        remove(o);
        return;
}

void GuiConnection::set(GP::Reader &body) {
        const uint32_t id = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        List<Assignment> props(MemSpace::Psram);
        ErrorCode        code = ErrorCode::BadValue;
        uint32_t         bad = 0;
        if (!readAssignments(body, o->cls, props, code, bad)) {
                fail(code, bad);
                return;
        }
        for (const Assignment &a : props) {
                const GuiStatus s = a.property->set(*o, a.value);
                if (s != 0) {
                        fail(ErrorCode(s), a.atom);
                        return;
                }
        }
        return;
}

void GuiConnection::get(GP::Reader &body) {
        const uint32_t id = body.u32();
        const uint32_t count = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        if (count > MaxNames || body.remaining() < size_t(count) * 4) {
                fail(ErrorCode::BadLength, count);
                return;
        }
        const size_t start = _out.size();
        GP::Writer   w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::Get), _sequence);
        w.u32(count);
        for (uint32_t i = 0; i < count; ++i) {
                const uint32_t     atom = body.u32();
                const GuiProperty *p = guiProperty(o->cls, atom);
                if (p == nullptr || p->get == nullptr) {
                        _out.truncate(start);
                        fail(p == nullptr ? ErrorCode::BadName : ErrorCode::BadAccess, atom);
                        return;
                }
                w.value(p->get(*o));
        }
        w.end();
        return;
}

void GuiConnection::call(GP::Reader &body, uint16_t flags) {
        const uint32_t id = body.u32();
        const uint32_t methodAtom = body.u32();
        const uint32_t count = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        const GuiMethod *m = guiMethod(o->cls, methodAtom);
        if (m == nullptr) {
                fail(ErrorCode::BadName, methodAtom);
                return;
        }
        // The arguments, as the schema has them.
        if (count != m->count) {
                fail(ErrorCode::BadValue, methodAtom);
                return;
        }
        Value args[GuiMaxArgs];
        for (uint32_t i = 0; i < count; ++i)
                if (!body.value(args[i]) || !fits(m->args[i], args[i])) {
                        fail(body.ok() ? ErrorCode::BadValue : ErrorCode::BadLength, methodAtom);
                        return;
                }
        Value           result;
        const GuiStatus s = m->call(*o, Span<const Value>(args, count), result);
        if (s != 0) {
                fail(ErrorCode(s), methodAtom);
                return;
        }
        if ((flags & GP::WantResult) == 0) return;
        GP::Writer w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::Call), _sequence);
        w.value(result);
        w.end();
        return;
}

void GuiConnection::listen(GP::Reader &body) {
        const uint32_t id = body.u32();
        const bool     on = body.u32() != 0;
        const uint32_t count = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        if (count > MaxNames || body.remaining() < size_t(count) * 4) {
                fail(ErrorCode::BadLength, count);
                return;
        }
        for (uint32_t i = 0; i < count; ++i) {
                const uint32_t atom = body.u32();
                if (guiEvent(o->cls, atom) == nullptr) {
                        fail(ErrorCode::BadName, atom);
                        return;
                }
                List<uint32_t> &add = on ? o->listenOn : o->listenOff;
                List<uint32_t> &drop = on ? o->listenOff : o->listenOn;
                for (size_t k = 0; k < drop.size(); ++k)
                        if (drop[k] == atom) {
                                drop.removeAt(k);
                                break;
                        }
                bool have = false;
                for (uint32_t a : add) have = have || a == atom;
                if (!have && add.append(atom).isError()) {
                        fail(ErrorCode::NoMem, atom);
                        return;
                }
        }
        return;
}

// A class's table: its parent, then its own properties, methods and events (a parent's are the
// parent's: `Describe` it too).
void GuiConnection::describe(GP::Reader &body) {
        const uint32_t  atom = body.u32();
        const GuiClass *cls = body.ok() ? guiClass(GuiAtoms::name(atom)) : nullptr;
        if (cls == nullptr) {
                fail(ErrorCode::BadClass, atom);
                return;
        }
        const auto mine = [&](const char *c) {
                return std::strcmp(c, cls->name) == 0;
        };
        const auto name = [](const char *n) {
                return GuiAtoms::intern(StringView(n));
        };
        const GuiClass *parent = guiParent(cls);
        GP::Writer      w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::Describe), _sequence);
        w.u32(parent != nullptr ? name(parent->name) : 0);
        w.u32(cls->create != nullptr ? 1 : 0); // can be made
        uint32_t n = 0;
        for (const GuiProperty &p : guiProperties()) n += mine(p.cls) ? 1 : 0;
        w.u32(n);
        for (const GuiProperty &p : guiProperties()) {
                if (!mine(p.cls)) continue;
                w.u32(name(p.name));
                w.u32(uint32_t(p.tag));
                w.u32((p.get != nullptr ? 1u : 0u) | (p.set != nullptr ? 2u : 0u)); // readable, writable
        }
        n = 0;
        for (const GuiMethod &m : guiMethods()) n += mine(m.cls) ? 1 : 0;
        w.u32(n);
        for (const GuiMethod &m : guiMethods()) {
                if (!mine(m.cls)) continue;
                w.u32(name(m.name));
                w.u32(uint32_t(m.result));
                w.u32(m.count);
                for (uint8_t i = 0; i < m.count; ++i) w.u32(uint32_t(m.args[i]));
        }
        n = 0;
        for (const GuiEvent &e : guiEvents()) n += mine(e.cls) ? 1 : 0;
        w.u32(n);
        for (const GuiEvent &e : guiEvents()) {
                if (!mine(e.cls)) continue;
                w.u32(name(e.name));
                w.u32(e.byDefault ? 1 : 0);
                w.u32(e.count);
                for (uint8_t i = 0; i < e.count; ++i) w.u32(uint32_t(e.values[i]));
        }
        w.end();
        return;
}

// PutImage: an Image object's pixels, in pieces: id, width, height, format (1: ARGB8888), the first
// pixel's offset, the count, then the pixels. A piece at offset 0 sets the size (a new block if
// it differs, the old one kept by its users).
void GuiConnection::putImage(GP::Reader &body) {
        const uint32_t id = body.u32(), width = body.u32(), height = body.u32(), format = body.u32();
        const uint32_t offset = body.u32(), count = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        if (o->cls == nullptr || std::strcmp(o->cls->name, "Image") != 0) {
                fail(ErrorCode::BadMatch, id);
                return;
        }
        if (format != 1 || width == 0 || height == 0 || width > MaxImageSide || height > MaxImageSide) {
                fail(ErrorCode::BadValue, id);
                return;
        }
        if (body.remaining() < size_t(count) * 4) {
                fail(ErrorCode::BadLength, count);
                return;
        }
        GuiImage *&image = o->image;
        if (offset == 0 && (image == nullptr || image->icon.width != width || image->icon.height != height)) {
                GuiImage *made = GuiImage::make(uint16_t(width), uint16_t(height));
                if (made == nullptr) {
                        fail(ErrorCode::NoMem, id);
                        return;
                }
                GuiImage::unref(image);
                image = made;
        }
        if (image == nullptr || image->icon.width != width || image->icon.height != height ||
            uint64_t(offset) + count > uint64_t(width) * height) {
                fail(ErrorCode::BadValue, id);
                return;
        }
        const ByteSpan pixels = body.bytes(size_t(count) * 4);
        std::memcpy(image->pixels + offset, pixels.data(), pixels.size()); // little-endian, as the wire
        return;
}

// PutPixels: canvas id, x, y, width, height, encoding, the data's bytes, the data (padded).
void GuiConnection::putPixels(GP::Reader &body) {
        const uint32_t id = body.u32();
        const int32_t  x = body.i32(), y = body.i32();
        const uint32_t width = body.u32(), height = body.u32(), encoding = body.u32(), bytes = body.u32();
        GuiObject     *o = body.ok() ? find(id) : nullptr;
        if (o == nullptr) {
                fail(ErrorCode::BadId, id);
                return;
        }
        if (body.remaining() < bytes) {
                fail(ErrorCode::BadLength, bytes);
                return;
        }
        const GuiStatus s = guiPutPixels(*o, x, y, int(width), int(height), encoding, body.bytes(bytes));
        if (s != 0) fail(ErrorCode(s), id);
        return;
}

void GuiConnection::sync() {
        GP::Writer w(_out);
        w.beginAnswer(GP::Kind::Reply, uint16_t(Opcode::Sync), _sequence);
        w.end();
        return;
}

// ---- Events and answers

bool GuiConnection::listens(const GuiObject &object, const char *event) const {
        const uint32_t  atom = GuiAtoms::intern(event);
        const GuiEvent *e = guiEvent(object.cls, atom);
        if (e == nullptr) return false;
        bool on = e->byDefault;
        for (uint32_t a : object.listenOff) on = on && a != atom;
        for (uint32_t a : object.listenOn) on = on || a == atom;
        return on;
}

void GuiConnection::emit(GuiObject &object, const char *event, Span<const Value> values) {
        if (_stopping.load(std::memory_order_acquire) || !listens(object, event)) return;
        const uint32_t atom = GuiAtoms::intern(event);
        GP::Writer w(_out);
        w.beginAnswer(GP::Kind::Event, 0, _sequence);
        w.u32(object.id);
        w.u32(atom);
        w.u32(uint32_t(System::uptimeUs() / 1000));
        w.u32(uint32_t(values.size()));
        for (const Value &v : values) w.value(v);
        w.end();
        send(); // a handler's event goes at once
        return;
}

void GuiConnection::send() {
        if (_out.isEmpty()) return;
        size_t done = 0;
        while (done < _out.size()) {
                auto [n, err] = _end->write(ByteSpan(_out.data() + done, _out.size() - done), WriteTimeoutMs);
                done += n;
                if (err.isError()) {
                        // Gone, or reading nothing for this long: the connection ends.
                        _out.clear();
                        stop();
                        return;
                }
        }
        _out.clear();
        return;
}

// ---- Objects

// A menu item of @p window whose shortcut is @p event: chosen. False: none.
bool GuiConnection::shortcut(ClientWindow *window, const KeyEvent &event) {
        if (event.action == KeyEvent::Action::Release) return false;
        const uint8_t mods = uint8_t((event.hasCtrl() ? 1 : 0) | (event.hasShift() ? 2 : 0) | (event.hasAlt() ? 4 : 0));
        for (GuiObject *o : _objects) {
                if (o->owner != window || o->keyUsage == 0 || o->keyUsage != event.usage || o->keyMods != mods)
                        continue;
                if (o->chosen != nullptr && o->chosen(*o)) return true;
        }
        return false;
}

ClientWindow *GuiConnection::modalOver(uint32_t ownerId) const {
        for (GuiObject *o : _objects)
                if (o->window != nullptr && o->window->ownerId == ownerId && o->window->modal && o->window->isShown())
                        return o->window;
        return nullptr;
}

Notifier *GuiConnection::notifier() {
        if (_notifier != nullptr) return _notifier;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(Notifier));
        if (mem == nullptr) return nullptr;
        _notifier = ::new (mem) Notifier(*_app);
        _notifier->setTold(notifierTold, this);
        return _notifier;
}

void GuiConnection::notifierTold(void *arg, Notifier &, uint32_t id, Notifier::Event event, uint32_t action) {
        guiNotificationTold(*static_cast<GuiConnection *>(arg), id, event, action);
        return;
}

GuiObject *GuiConnection::find(uint32_t id) const {
        size_t low = 0, high = _objects.size();
        while (low < high) {
                const size_t mid = (low + high) / 2;
                if (_objects[mid]->id < id)
                        low = mid + 1;
                else
                        high = mid;
        }
        return low < _objects.size() && _objects[low]->id == id ? _objects[low] : nullptr;
}

Error GuiConnection::add(GuiObject *object) {
        size_t at = _objects.size();
        while (at > 0 && _objects[at - 1]->id > object->id) --at; // ids mostly come in order
        return _objects.insert(at, std::move(object));
}

void GuiConnection::remove(GuiObject *object) {
        // Its children first (they are under it in the toolkit too).
        for (size_t i = _objects.size(); i-- > 0;)
                if (i < _objects.size() && _objects[i]->parent == object->id) remove(_objects[i]);
        if (object->window != nullptr) {
                for (size_t i = 0; i < _dirty.size(); ++i)
                        if (_dirty[i] == object->window) {
                                _dirty.removeAt(i);
                                break;
                        }
                object->window->~ClientWindow();
                Memory::free(object->window);
        } else {
                // Its parent lets go of it first (a tab, a splitter's side, a menu's entry).
                GuiObject *parent = object->parent != 0 ? find(object->parent) : nullptr;
                if (parent != nullptr && parent->release != nullptr) parent->release(*parent, *object);
                if (object->widget != nullptr) object->widget->destroy();
                if (object->dispose != nullptr) object->dispose(*object); // a menu, a dialog, a timer
                touched(object->owner);
        }
        // Its images let go once nothing of its shows them.
        GuiImage::unref(object->image);
        for (GuiImage *&image : object->images) GuiImage::unref(image);
        for (size_t i = 0; i < _objects.size(); ++i)
                if (_objects[i] == object) {
                        _objects.removeAt(i);
                        break;
                }
        object->~GuiObject();
        Memory::free(object);
        return;
}

void GuiConnection::touched(ClientWindow *window) {
        if (window == nullptr) return;
        for (ClientWindow *w : _dirty)
                if (w == window) return;
        (void)_dirty.append(std::move(window));
        return;
}

// The windows changed by the requests just run: their boxes laid out again, outermost first.
void GuiConnection::layOut() {
        for (ClientWindow *w : _dirty) {
                if (!w->isShown()) continue;
                w->column()->relayout();
                for (GuiObject *o : _objects)
                        if (o->owner == w && o->relayout != nullptr) o->relayout(*o);
                w->update();
        }
        _dirty.clear();
        return;
}

BMDS_NAMESPACE_END
