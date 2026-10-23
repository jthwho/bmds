/**
 * @file      guiclipboard.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The toolkit's clipboard (devplan/clipboard.md C13, §4f).
 */

#include <cstring>
#include <new>
#include <sdkconfig.h>
#include <bmds/clipboardreader.h>
#include <bmds/clock.h>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t ThreadStackBytes = 6144;

        size_t index(Clipboard::Selection s) {
                return static_cast<size_t>(s);
        }

        // Runs @p data through @p path's built-in converters (a copy of this application's, pasted
        // into it: no pipe, which its own thread would wait on).
        Error convert(const ClipboardConversions &table, const ClipboardConversions::Path &path, String &data) {
                for (size_t s = 0; s < path.steps; ++s) {
                        const ClipboardConversions::Builtin b = table.converter(path.converter[s]).builtin;
                        if (b == ClipboardConversions::Builtin::None) return Error::NotSupported;
                        ClipboardConverter converter(b);
                        String             out(MemSpace::Psram);
                        Error              err = out.reserve(ClipboardConverter::maxOutput(data.size()));
                        if (err.isError()) return err;
                        const size_t n =
                                converter.put(ByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size()),
                                              reinterpret_cast<uint8_t *>(out.data()));
                        const size_t m = converter.finish(reinterpret_cast<uint8_t *>(out.data()) + n);
                        String       done(MemSpace::Psram);
                        err = done.assign(StringView(out.data(), n + m));
                        if (err.isError()) return err;
                        data = std::move(done);
                }
                return Error::Ok;
        }

} // namespace

GuiClipboard::~GuiClipboard() {
        // No owner calls into a clipboard that is gone (keep() has run, unless exec() didn't).
        Clipboard *c = clipboard();
        for (size_t s = 0; s < Clipboard::Selections && c != nullptr; ++s)
                if (_own[s].fn != nullptr || _own[s].asked != nullptr)
                        (void)c->withdraw(static_cast<Selection>(s), _own[s].serial);
        if (_started.load(std::memory_order_acquire)) {
                _quit.store(true, std::memory_order_release);
                _work.release();
                _thread.join();
        }
}

Clipboard *GuiClipboard::clipboard() const {
        auto [c, err] = Clipboard::clipboard(0);
        return err.isOk() ? c : nullptr;
}

void GuiClipboard::setName(StringView name) {
        const size_t n = name.size() < Clipboard::MaxOwnerName ? name.size() : Clipboard::MaxOwnerName;
        std::memcpy(_name, name.data(), n);
        _name[n] = '\0';
        return;
}

Error GuiClipboard::copyText(Selection selection, StringView text) {
        Clipboard *c = clipboard();
        if (c == nullptr) return Error::NotExist;
        const StringView types[] = {ClipboardType::Text};
        auto [serial, err] =
                c->copy(selection, Span<const StringView>(types),
                        ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), StringView(_name), 0);
        if (err.isError()) return err;
        _own[index(selection)] = Own{serial, nullptr, nullptr};
        return Error::Ok;
}

uint32_t GuiClipboard::serial(Selection selection) const {
        return owns(selection) ? _own[index(selection)].serial : 0;
}

Error GuiClipboard::copy(Selection selection, Span<const StringView> types, Provider fn, void *arg) {
        Clipboard *c = clipboard();
        if (c == nullptr) return Error::NotExist;
        if (fn == nullptr) return Error::Invalid;
        Clipboard::Owner owner;
        owner.requested = requested;
        owner.lost = lost;
        owner.arg = this;
        owner.name = StringView(_name);
        auto [serial, err] = c->offer(selection, types, owner);
        if (err.isError()) return err;
        _own[index(selection)] = Own{serial, fn, arg};
        return Error::Ok;
}

Error GuiClipboard::offer(Selection selection, Span<const StringView> types, Asked fn, void *arg) {
        Clipboard *c = clipboard();
        if (c == nullptr) return Error::NotExist;
        if (fn == nullptr) return Error::Invalid;
        Clipboard::Owner owner;
        owner.requested = requested;
        owner.lost = lost;
        owner.arg = this;
        owner.name = StringView(_name);
        auto [serial, err] = c->offer(selection, types, owner);
        if (err.isError()) return err;
        Own own;
        own.serial = serial;
        own.arg = arg;
        own.asked = fn;
        _own[index(selection)] = own;
        return Error::Ok;
}

GuiClipboard::Request *GuiClipboard::askedRequest(uint32_t id) {
        Mutex::Locker lock(_jobsLock, WaitForever);
        for (Request &r : _requests)
                if (r.state == State::Asked && r.id == id && id != 0) return &r;
        return nullptr;
}

Error GuiClipboard::answer(uint32_t request, StringView data) {
        Request *r = askedRequest(request); // asked: only this thread touches it until finished
        if (r == nullptr) return Error::NotFound;
        const Clipboard *c = clipboard();
        if (c != nullptr && r->data.size() + data.size() > c->limit()) return Error::NoSpace;
        return r->data.append(data);
}

Error GuiClipboard::finish(uint32_t request) {
        Request *r = askedRequest(request);
        if (r == nullptr) return Error::NotFound;
        {
                Mutex::Locker lock(_jobsLock, WaitForever);
                r->state = State::Ready;
        }
        return wake();
}

void GuiClipboard::forget(void *arg) {
        Clipboard *c = clipboard();
        for (size_t s = 0; s < Clipboard::Selections; ++s) {
                Own &own = _own[s];
                if (own.asked == nullptr || own.arg != arg) continue;
                if (c != nullptr) (void)c->withdraw(static_cast<Selection>(s), own.serial);
                own = Own();
        }
        bool ended = false;
        {
                Mutex::Locker lock(_jobsLock, WaitForever);
                for (Request &r : _requests) {
                        if (r.state != State::Asked || r.askedArg != arg) continue;
                        r.data = String(MemSpace::Psram);
                        r.state = State::Ready; // nothing written: the asker sees the end
                        ended = true;
                }
        }
        if (ended) (void)wake();
        return;
}

Error GuiClipboard::clear(Selection selection) {
        Clipboard *c = clipboard();
        if (c == nullptr) return Error::NotExist;
        Own &own = _own[index(selection)];
        if (own.serial == 0) return Error::InvalidState;
        const Error err = c->clear(selection, own.serial);
        if (err.isOk()) own = Own(); // its loss, told after, is no news
        return err;
}

bool GuiClipboard::owns(Selection selection) const {
        const Clipboard *c = clipboard();
        const uint32_t   serial = _own[index(selection)].serial;
        if (c == nullptr || serial == 0) return false;
        Clipboard::Description d;
        c->describe(selection, d);
        return d.serial == serial;
}

bool GuiClipboard::canPaste(Selection selection, StringView type) const {
        const Clipboard *c = clipboard();
        if (c == nullptr) return false;
        Clipboard::Description d;
        c->describe(selection, d);
        if (d.serial == 0) return false;
        StringView offered[Clipboard::MaxTypes];
        for (size_t i = 0; i < d.count; ++i) offered[i] = StringView(d.types[i].type);
        const ClipboardConversions *table = c->conversions();
        if (table != nullptr) return table->find(Span<const StringView>(offered, d.count), type).isOk();
        for (size_t i = 0; i < d.count; ++i)
                if (ClipboardType::equal(offered[i], type)) return true;
        return false;
}

Error GuiClipboard::paste(Selection selection, StringView type, Received fn, void *arg) {
        if (fn == nullptr) return Error::Invalid;
        Clipboard *c = clipboard();
        Paste     *slot = nullptr;
        {
                Mutex::Locker lock(_jobsLock, WaitForever);
                for (Paste &p : _pastes)
                        if (p.state == State::Free && slot == nullptr) slot = &p;
                if (slot == nullptr) return Error::Full;
                slot->state = State::Waiting;
        }
        slot->selection = selection;
        slot->fn = fn;
        slot->arg = arg;
        slot->err = Error::Ok;
        slot->data = String(MemSpace::Psram);
        auto [normal, nerr] = ClipboardType::normalize(type, Span<char>(slot->type, sizeof(slot->type)));
        (void)normal;
        // Its own copy: rendered here and converted (the thread would wait for this thread). One
        // answered later can't be: the application has what it offered.
        const Own &own = _own[index(selection)];
        if (nerr.isOk() && own.asked != nullptr && owns(selection)) nerr = Error::InvalidState;
        if (nerr.isOk() && c != nullptr && own.fn != nullptr && owns(selection)) {
                Clipboard::Description d;
                c->describe(selection, d);
                StringView offered[Clipboard::MaxTypes];
                for (size_t i = 0; i < d.count; ++i) offered[i] = StringView(d.types[i].type);
                Error                       err = Error::NotFound;
                const ClipboardConversions *table = c->conversions();
                if (table != nullptr) {
                        auto [path, perr] =
                                table->find(Span<const StringView>(offered, d.count), StringView(slot->type));
                        err = perr;
                        if (err.isOk()) err = own.fn(own.arg, offered[path.offered], slot->data);
                        if (err.isOk()) err = convert(*table, path, slot->data);
                }
                slot->err = err;
                if (err.isError()) slot->data = String(MemSpace::Psram);
                {
                        Mutex::Locker lock(_jobsLock, WaitForever);
                        slot->state = State::Ready;
                }
                if (!_posted.exchange(true, std::memory_order_acq_rel)) (void)_app.post(drain, this);
                return Error::Ok;
        }
        if (nerr.isError()) {
                slot->err = nerr == Error::InvalidState ? nerr : Error::Invalid;
                {
                        Mutex::Locker lock(_jobsLock, WaitForever);
                        slot->state = State::Ready;
                }
                if (!_posted.exchange(true, std::memory_order_acq_rel)) (void)_app.post(drain, this);
                return Error::Ok;
        }
        return wake();
}

void GuiClipboard::cancel(void *arg) {
        Mutex::Locker lock(_jobsLock, WaitForever);
        for (Paste &p : _pastes)
                if (p.state != State::Free && p.arg == arg) p.fn = nullptr; // read anyway, told no one
        return;
}

// Another program asks for a type (on its task, the clipboard's call lock held): queued for the
// loop's thread to render.
void GuiClipboard::requested(void *arg, uint32_t serial, StringView type, FileHandle::Ptr write) {
        auto    *self = static_cast<GuiClipboard *>(arg);
        Request *slot = nullptr;
        {
                Mutex::Locker lock(self->_jobsLock, WaitForever);
                for (Request &r : self->_requests)
                        if (r.state == State::Free && slot == nullptr) slot = &r;
                if (slot == nullptr) return; // too many: the asker sees the end
                slot->serial = serial;
                const size_t n = type.size() < ClipboardType::MaxType ? type.size() : ClipboardType::MaxType;
                std::memcpy(slot->type, type.data(), n);
                slot->type[n] = '\0';
                slot->write = std::move(write);
                slot->state = State::Waiting;
        }
        if (!self->_posted.exchange(true, std::memory_order_acq_rel)) (void)self->_app.post(drain, self);
        return;
}

void GuiClipboard::lost(void *arg, uint32_t serial) {
        auto *self = static_cast<GuiClipboard *>(arg);
        {
                Mutex::Locker lock(self->_jobsLock, WaitForever);
                for (uint32_t &s : self->_lostSerials) {
                        if (s != 0) continue;
                        s = serial;
                        break;
                }
        }
        if (!self->_posted.exchange(true, std::memory_order_acq_rel)) (void)self->_app.post(drain, self);
        return;
}

// On the loop's thread, the application's lock held: copies lost, types to render, pastes read.
void GuiClipboard::drain(void *arg) {
        auto *self = static_cast<GuiClipboard *>(arg);
        self->_posted.store(false, std::memory_order_release);
        uint32_t lostSerials[Clipboard::Selections] = {};
        {
                Mutex::Locker lock(self->_jobsLock, WaitForever);
                std::memcpy(lostSerials, self->_lostSerials, sizeof(lostSerials));
                std::memset(self->_lostSerials, 0, sizeof(self->_lostSerials));
        }
        for (const uint32_t serial : lostSerials) {
                if (serial == 0) continue;
                for (size_t s = 0; s < Clipboard::Selections; ++s) {
                        if (self->_own[s].serial != serial) continue;
                        self->_own[s] = Own();
                        if (self->_lost != nullptr) self->_lost(self->_lostArg, static_cast<Selection>(s));
                }
        }
        bool          written = false;
        const int64_t now = Clock::monotonicNs();
        {
                // Answers not finished in time: the asker has given up (its read timed out).
                Mutex::Locker lock(self->_jobsLock, WaitForever);
                for (Request &r : self->_requests) {
                        if (r.state != State::Asked || now < r.deadlineNs) continue;
                        r.data = String(MemSpace::Psram);
                        r.state = State::Ready;
                        written = true;
                }
        }
        for (Request &r : self->_requests) {
                {
                        Mutex::Locker lock(self->_jobsLock, WaitForever);
                        if (r.state != State::Waiting) continue;
                        r.state = State::Busy;
                }
                // A copy answered later: its owner told, the answer coming by answer() and finish().
                const Own *asked = nullptr;
                size_t     askedSelection = 0;
                for (size_t s = 0; s < Clipboard::Selections; ++s) {
                        if (self->_own[s].serial != r.serial || self->_own[s].asked == nullptr) continue;
                        asked = &self->_own[s];
                        askedSelection = s;
                }
                if (asked != nullptr) {
                        if (++self->_nextRequest == 0) self->_nextRequest = 1;
                        r.id = self->_nextRequest;
                        r.deadlineNs = now + int64_t(TimeoutMs) * 1000000;
                        r.askedArg = asked->arg;
                        r.data = String(MemSpace::Psram);
                        {
                                Mutex::Locker lock(self->_jobsLock, WaitForever);
                                r.state = State::Asked;
                        }
                        const Asked fn = asked->asked; // the call may copy again: _own changes
                        void       *fnArg = asked->arg;
                        fn(fnArg, r.id, static_cast<Selection>(askedSelection), StringView(r.type));
                        continue;
                }
                self->render(r);
                Mutex::Locker lock(self->_jobsLock, WaitForever);
                r.state = State::Ready;
                written = true;
        }
        if (written) (void)self->wake();
        for (Paste &p : self->_pastes) {
                {
                        Mutex::Locker lock(self->_jobsLock, WaitForever);
                        if (p.state != State::Ready) continue;
                }
                const Received fn = p.fn;
                void          *fnArg = p.arg;
                const Error    err = p.err;
                String         data = std::move(p.data);
                char           type[ClipboardType::BufferBytes];
                std::memcpy(type, p.type, sizeof(type));
                {
                        Mutex::Locker lock(self->_jobsLock, WaitForever);
                        p.data = String(MemSpace::Psram);
                        p.state = State::Free;
                }
                if (fn != nullptr) fn(fnArg, err, StringView(type), data.view());
        }
        return;
}

// A request's bytes, from its copy's provider (none if that copy isn't this one's now).
void GuiClipboard::render(Request &r) {
        r.data = String(MemSpace::Psram);
        for (size_t s = 0; s < Clipboard::Selections; ++s) {
                const Own &own = _own[s];
                if (own.serial != r.serial || own.fn == nullptr) continue;
                if (own.fn(own.arg, StringView(r.type), r.data).isError()) r.data = String(MemSpace::Psram);
                return;
        }
        return;
}

Error GuiClipboard::wake() {
        if (!_started.exchange(true, std::memory_order_acq_rel)) {
                Thread::Config config;
                config.name = "guiclip";
                config.stackBytes = ThreadStackBytes;
                config.priority = CONFIG_BMDS_SHELL_PRIORITY;
                const Error err = _thread.start(config, threadMain, this);
                if (err.isError()) {
                        _started.store(false, std::memory_order_release);
                        return err;
                }
        }
        _work.release();
        return Error::Ok;
}

// The thread: rendered bytes into their askers' pipes; pastes read whole.
void GuiClipboard::threadMain(void *arg) {
        auto *self = static_cast<GuiClipboard *>(arg);
        for (;;) {
                (void)self->_work.acquire(WaitForever);
                if (self->_quit.load(std::memory_order_acquire)) return;
                for (Request &r : self->_requests) {
                        {
                                Mutex::Locker lock(self->_jobsLock, WaitForever);
                                if (r.state != State::Ready) continue;
                                r.state = State::Busy;
                        }
                        if (r.write && !r.data.isEmpty())
                                (void)r.write->write(
                                        ByteSpan(reinterpret_cast<const uint8_t *>(r.data.data()), r.data.size()),
                                        TimeoutMs);
                        Mutex::Locker lock(self->_jobsLock, WaitForever);
                        r.write.reset(); // the asker's end of the bytes
                        r.data = String(MemSpace::Psram);
                        r.state = State::Free;
                }
                for (Paste &p : self->_pastes) {
                        {
                                Mutex::Locker lock(self->_jobsLock, WaitForever);
                                if (p.state != State::Waiting) continue;
                                p.state = State::Busy;
                        }
                        self->read(p);
                        {
                                Mutex::Locker lock(self->_jobsLock, WaitForever);
                                p.state = State::Ready;
                        }
                        if (!self->_posted.exchange(true, std::memory_order_acq_rel))
                                (void)self->_app.post(drain, self);
                }
        }
}

// A paste read whole (up to the clipboard's limit), on the thread.
void GuiClipboard::read(Paste &p) {
        Clipboard *c = clipboard();
        if (c == nullptr) {
                p.err = Error::NotExist;
                return;
        }
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClipboardReader));
        if (mem == nullptr) {
                p.err = Error::NoMem;
                return;
        }
        auto *reader = ::new (mem) ClipboardReader(*c);
        p.err = reader->open(p.selection, StringView(p.type));
        while (p.err.isOk()) {
                uint8_t buf[512];
                auto [n, err] = reader->read(WritableByteSpan(buf, sizeof(buf)), TimeoutMs);
                if (err == Error::EndOfFile) break;
                if (err.isError()) {
                        p.err = err;
                } else if (p.data.size() + n > c->limit()) {
                        p.err = Error::NoSpace;
                } else {
                        p.err = p.data.append(StringView(reinterpret_cast<const char *>(buf), n));
                }
        }
        if (p.err.isError()) p.data = String(MemSpace::Psram);
        reader->~ClipboardReader();
        Memory::free(reader);
        return;
}

// The application is ending (its objects still there): each copy on request it owns keeps its
// first type, stored, and is withdrawn, so it outlives the application.
void GuiClipboard::keep() {
        Clipboard *c = clipboard();
        if (c == nullptr) return;
        for (size_t s = 0; s < Clipboard::Selections; ++s) {
                Own &own = _own[s];
                if (own.asked != nullptr) { // nothing could answer it once the application is gone
                        (void)c->withdraw(static_cast<Selection>(s), own.serial);
                        own = Own();
                        continue;
                }
                if (own.fn == nullptr || !owns(static_cast<Selection>(s))) {
                        own = Own();
                        continue;
                }
                if (static_cast<Selection>(s) == Selection::Drag) {
                        (void)c->withdraw(Selection::Drag, own.serial); // a drag's: gone with the program
                        own = Own();
                        continue;
                }
                Clipboard::Description d;
                c->describe(static_cast<Selection>(s), d);
                String data(MemSpace::Psram);
                if (d.count > 0 && own.fn(own.arg, StringView(d.types[0].type), data).isOk())
                        (void)c->store(static_cast<Selection>(s), own.serial, StringView(d.types[0].type),
                                       ByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size()));
                (void)c->withdraw(static_cast<Selection>(s), own.serial);
                own = Own();
        }
        return;
}

BMDS_NAMESPACE_END
