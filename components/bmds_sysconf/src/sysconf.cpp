/**
 * @file      sysconf.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system configuration's registry (devplan/sysconf.md).
 */

#include <cstring>
#include <new>
#include <utility>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

#include "sysconfstate.h"

BMDS_NAMESPACE_BEGIN

namespace SysConf {

        namespace {

                // A parameter: its declaration, canonical default, current and saved values.
                struct Entry {
                                const Param *param = nullptr;
                                String       defaultValue{MemSpace::Psram};
                                String       current{MemSpace::Psram};
                                String       saved{MemSpace::Psram};
                                bool         hasSaved = false;
                };

                // A program's declaration, its strings copied.
                struct Owned {
                                Param  param;
                                String key{MemSpace::Psram};
                                String defaultValue{MemSpace::Psram};
                                String help{MemSpace::Psram};
                                String choices{MemSpace::Psram};
                };

                // A saved line not applied: no such key (yet), or a value its type refuses.
                struct Orphan {
                                String key{MemSpace::Psram};
                                String value{MemSpace::Psram};
                };

                constexpr size_t MaxOrphans = 128;

                struct State {
                                Entry             *entries = nullptr; // stable: watchers keep indices
                                uint16_t          *order = nullptr;   // entries sorted by key
                                size_t             count = 0;
                                size_t             capacity = 0;
                                Owned             *owned = nullptr; // MaxRuntime
                                size_t             ownedCount = 0;
                                Orphan            *orphans = nullptr; // MaxOrphans
                                size_t             orphanCount = 0;
                                Watcher           *watchers = nullptr;
                                char               storePath[Vfs::MaxMountPath + 32] = {};
                                detail::LoadReport report;
                };

                constinit State               state;
                constinit StaticObject<Mutex> registryLock; // the state, the watchers' rings
                constinit StaticObject<Mutex> notifyLock;   // notifications running; watchers' list
                constinit StaticObject<Mutex> saveLock;     // one save (store write) at a time


                bool underPrefix(StringView key, StringView prefix) {
                        if (prefix.isEmpty()) return true;
                        return key.startsWith(prefix) && (key.size() == prefix.size() || key[prefix.size()] == '/');
                }

                StringView keyAt(size_t index) {
                        return StringView(state.entries[index].param->key);
                }

                // The position in `order` where @p key is or would go.
                size_t lowerBound(StringView key) {
                        size_t lo = 0, hi = state.count;
                        while (lo < hi) {
                                const size_t mid = (lo + hi) / 2;
                                if (keyAt(state.order[mid]) < key) {
                                        lo = mid + 1;
                                } else {
                                        hi = mid;
                                }
                        }
                        return lo;
                }

                // The entry for @p key, or -1. Lock held.
                int findLocked(StringView key) {
                        const size_t at = lowerBound(key);
                        return at < state.count && keyAt(state.order[at]) == key ? state.order[at] : -1;
                }

                void freeState() {
                        for (size_t i = 0; i < state.count; ++i) state.entries[i].~Entry();
                        for (size_t i = 0; i < state.ownedCount; ++i) state.owned[i].~Owned();
                        for (size_t i = 0; i < state.orphanCount; ++i) state.orphans[i].~Orphan();
                        Memory::free(state.entries);
                        Memory::free(state.order);
                        Memory::free(state.owned);
                        Memory::free(state.orphans);
                        state.entries = nullptr;
                        state.order = nullptr;
                        state.owned = nullptr;
                        state.orphans = nullptr;
                        state.count = state.capacity = state.ownedCount = state.orphanCount = 0;
                        return;
                }

                // Adds @p param (its default checked) to the table. Lock held.
                Error addLocked(const Param &param) {
                        if (state.count == state.capacity) return Error::Full;
                        if (!Config::isValidKey(StringView(param.key))) return Error::Invalid;
                        const size_t at = lowerBound(StringView(param.key));
                        if (at < state.count && keyAt(state.order[at]) == StringView(param.key)) return Error::Exists;
                        Entry &e = *::new (&state.entries[state.count]) Entry();
                        e.param = &param;
                        Error err = Config::canonical(param, StringView(param.defaultValue), e.defaultValue);
                        if (err.isOk()) err = e.current.assign(e.defaultValue.view());
                        if (err.isError()) {
                                e.~Entry();
                                return err;
                        }
                        std::memmove(&state.order[at + 1], &state.order[at], (state.count - at) * sizeof(uint16_t));
                        state.order[at] = static_cast<uint16_t>(state.count);
                        ++state.count;
                        return Error::Ok;
                }

                // Applies a saved @p value to entry @p index, if its type takes it. Lock held.
                bool applySavedLocked(size_t index, StringView value) {
                        Entry &e = state.entries[index];
                        String canon(MemSpace::Psram);
                        if (Config::canonical(*e.param, value, canon).isError()) return false;
                        if (e.saved.assign(canon.view()).isError() || e.current.assign(canon.view()).isError())
                                return false;
                        e.hasSaved = true;
                        return true;
                }

                void addOrphanLocked(StringView key, StringView value) {
                        if (state.orphanCount == MaxOrphans) return;
                        Orphan &o = *::new (&state.orphans[state.orphanCount]) Orphan();
                        if (o.key.assign(key).isError() || o.value.assign(value).isError()) {
                                o.~Orphan();
                                return;
                        }
                        ++state.orphanCount;
                        return;
                }

                void removeOrphanLocked(StringView key) {
                        for (size_t i = 0; i < state.orphanCount;) {
                                if (state.orphans[i].key.view() != key) {
                                        ++i;
                                        continue;
                                }
                                // The last one takes its place (Strings aren't moved as bytes).
                                const size_t last = state.orphanCount - 1;
                                if (i != last) {
                                        state.orphans[i].key = std::move(state.orphans[last].key);
                                        state.orphans[i].value = std::move(state.orphans[last].value);
                                }
                                state.orphans[last].~Orphan();
                                --state.orphanCount;
                        }
                        return;
                }

                void storeLine(void *, StringView key, StringView value) {
                        const int index = findLocked(key);
                        if (index >= 0 && applySavedLocked(size_t(index), value)) return;
                        if (index >= 0) ++state.report.rejected;
                        addOrphanLocked(key, value);
                        return;
                }

                // Reads the store. Lock held.
                void loadLocked() {
                        Vfs::Context context;
                        auto [file, err] = Vfs::open(StringView(state.storePath), Vfs::Read, context);
                        if (err == Error::NotExist) return;
                        if (err.isError()) {
                                state.report.storeError = err;
                                return;
                        }
                        auto *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxStoreBytes));
                        if (text == nullptr) return;
                        size_t got = 0;
                        while (got < MaxStoreBytes) {
                                auto [n, rerr] = file->read(
                                        WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxStoreBytes - got),
                                        WaitForever);
                                if (rerr.isError() || n == 0) break;
                                got += n;
                        }
                        state.report.malformed = Config::parseStore(StringView(text, got), storeLine, nullptr);
                        Memory::free(text);
                        return;
                }

                // The store's text: every saved value and orphan, by key. Lock held.
                Error buildStoreLocked(String &out) {
                        struct Line {
                                        StringView key;
                                        StringView value;
                        };
                        auto *lines = static_cast<Line *>(
                                Memory::alloc(MemSpace::Psram, (state.count + state.orphanCount + 1) * sizeof(Line)));
                        if (lines == nullptr) return Error::NoMem;
                        size_t n = 0;
                        for (size_t i = 0; i < state.count; ++i) {
                                const Entry &e = state.entries[state.order[i]];
                                if (e.hasSaved) lines[n++] = Line{StringView(e.param->key), e.saved.view()};
                        }
                        // The orphans, each put in its place among the sorted entries.
                        for (size_t o = 0; o < state.orphanCount; ++o) {
                                const Line line{state.orphans[o].key.view(), state.orphans[o].value.view()};
                                size_t     at = n;
                                while (at > 0 && lines[at - 1].key > line.key) {
                                        lines[at] = lines[at - 1];
                                        --at;
                                }
                                lines[at] = line;
                                ++n;
                        }
                        Error err = out.append("# The saved system settings (sysconf save): key=value, one a line.\n");
                        for (size_t i = 0; i < n && err.isOk(); ++i)
                                err = Config::appendStoreLine(out, lines[i].key, lines[i].value);
                        Memory::free(lines);
                        return err;
                }

                Error writeStore(const String &text) {
                        Vfs::Context context;
                        auto [file, err] = Vfs::open(StringView(state.storePath),
                                                     Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                        if (err.isError()) return err;
                        auto [n, werr] = file->write(
                                ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), WaitForever);
                        if (werr.isOk() && n != text.size()) werr = Error::IoError;
                        return werr; // committed (on littlefs: atomically) when the handle closes
                }

                // Sets entry @p index's current value to canonical @p value and tells the
                // watchers. Neither lock held.
                Error apply(size_t index, StringView value) {
                        Mutex::Locker    notifying(*notifyLock, WaitForever);
                        const StringView key(state.entries[index].param->key); // declarations stay
                        {
                                Mutex::Locker lock(*registryLock, WaitForever);
                                Entry        &e = state.entries[index];
                                if (e.current.view() == value) return Error::Ok;
                                const Error err = e.current.assign(value);
                                if (err.isError()) return err;
                                for (Watcher *w = state.watchers; w != nullptr; w = w->nextWatcher)
                                        if (w->matches(key)) w->push(static_cast<uint16_t>(index));
                        }
                        // The watchers' list can't change (notifyLock held), and readers may take the lock.
                        for (Watcher *w = state.watchers; w != nullptr; w = w->nextWatcher) {
                                if (!w->matches(key)) continue;
                                if (w->notify != nullptr) w->notify(w->notifyArg);
                                w->waiters().notify();
                        }
                        return Error::Ok;
                }

                // The entry for @p key and a copy of its declaration's pointer.
                int find(StringView key) {
                        if (!isReady()) return -1;
                        Mutex::Locker lock(*registryLock, WaitForever);
                        return findLocked(key);
                }

                Error setupLocked(size_t builtins) {
                        freeState();
                        state.capacity = builtins + MaxRuntime;
                        state.entries =
                                static_cast<Entry *>(Memory::alloc(MemSpace::Psram, state.capacity * sizeof(Entry)));
                        state.order = static_cast<uint16_t *>(
                                Memory::alloc(MemSpace::Psram, state.capacity * sizeof(uint16_t)));
                        state.owned = static_cast<Owned *>(Memory::alloc(MemSpace::Psram, MaxRuntime * sizeof(Owned)));
                        state.orphans =
                                static_cast<Orphan *>(Memory::alloc(MemSpace::Psram, MaxOrphans * sizeof(Orphan)));
                        if (state.entries == nullptr || state.order == nullptr || state.owned == nullptr ||
                            state.orphans == nullptr) {
                                freeState();
                                return Error::NoMem;
                        }
                        return Error::Ok;
                }

                Error start(Span<const Param> builtins, StringView storePath) {
                        if (!registryLock.isCreated()) registryLock.create();
                        if (!notifyLock.isCreated()) notifyLock.create();
                        if (!saveLock.isCreated()) saveLock.create();
                        {
                                Mutex::Locker lock(*registryLock, WaitForever);
                                const size_t  n = storePath.size() < sizeof(state.storePath) - 1
                                                          ? storePath.size()
                                                          : sizeof(state.storePath) - 1;
                                std::memcpy(state.storePath, storePath.data(), n);
                                state.storePath[n] = '\0';
                                Error err = setupLocked(builtins.size());
                                if (err.isError()) return err;
                                state.report = detail::LoadReport();
                                for (const Param &p : builtins) {
                                        err = addLocked(p);
                                        if (err.isError() && state.report.badDeclarations++ == 0)
                                                state.report.firstBad = p.key;
                                }
                                loadLocked();
                        }
                        return Error::Ok;
                }

        } // namespace

        bool isReady() {
                return registryLock.isCreated() && state.entries != nullptr;
        }

        const char *Info::stateName() const {
                if (isChanged()) return "changed";
                return hasSaved ? "saved" : "default";
        }

        Error initForHost(Span<const Param> builtins, StringView storePath) {
                return start(builtins, storePath);
        }

        detail::LoadReport detail::start(Span<const Param> builtins, StringView storePath) {
                const Error        err = SysConf::start(builtins, storePath);
                detail::LoadReport report;
                if (registryLock.isCreated()) {
                        Mutex::Locker lock(*registryLock, WaitForever);
                        report = state.report;
                        report.parameters = state.count;
                        report.orphans = state.orphanCount;
                }
                report.error = err;
                return report;
        }

        Error get(StringView key, String &out) {
                if (!isReady()) return Error::NotExist;
                Mutex::Locker lock(*registryLock, WaitForever);
                const int     index = findLocked(key);
                if (index < 0) return Error::NotExist;
                return out.assign(state.entries[index].current.view());
        }

        namespace {

                Error typed(StringView key, Config::Type type, String &value) {
                        if (!isReady()) return Error::NotExist;
                        Mutex::Locker lock(*registryLock, WaitForever);
                        const int     index = findLocked(key);
                        if (index < 0) return Error::NotExist;
                        if (state.entries[index].param->type != type) return Error::Invalid;
                        return value.assign(state.entries[index].current.view());
                }

        } // namespace

        Result<int32_t> getInt(StringView key) {
                String      value(MemSpace::Psram);
                const Error err = typed(key, Config::Type::Int, value);
                if (err.isError()) return makeError<int32_t>(err);
                return makeResult(Config::toInt(value.view()));
        }

        Result<bool> getBool(StringView key) {
                String      value(MemSpace::Psram);
                const Error err = typed(key, Config::Type::Bool, value);
                if (err.isError()) return makeError<bool>(err);
                return makeResult(Config::toBool(value.view()));
        }

        Result<uint32_t> getColor(StringView key) {
                String      value(MemSpace::Psram);
                const Error err = typed(key, Config::Type::Color, value);
                if (err.isError()) return makeError<uint32_t>(err);
                return makeResult(Config::toColor(value.view()));
        }

        Error set(StringView key, StringView value) {
                const int index = find(key);
                if (index < 0) return Error::NotExist;
                const Param &param = *state.entries[index].param; // entries are stable
                String       canon(MemSpace::Psram);
                Error        err = Config::canonical(param, value, canon);
                if (err.isOk() && param.check != nullptr) err = param.check(canon.view());
                if (err.isError()) return err;
                return apply(size_t(index), canon.view());
        }

        Error setDefault(StringView key) {
                const int index = find(key);
                if (index < 0) return Error::NotExist;
                String value(MemSpace::Psram);
                {
                        Mutex::Locker lock(*registryLock, WaitForever);
                        const Error   err = value.assign(state.entries[index].defaultValue.view());
                        if (err.isError()) return err;
                }
                return apply(size_t(index), value.view());
        }

        Error revert(StringView key) {
                const int index = find(key);
                if (index < 0) return Error::NotExist;
                String value(MemSpace::Psram);
                {
                        Mutex::Locker lock(*registryLock, WaitForever);
                        const Entry  &e = state.entries[index];
                        const Error   err = value.assign(e.hasSaved ? e.saved.view() : e.defaultValue.view());
                        if (err.isError()) return err;
                }
                return apply(size_t(index), value.view());
        }

        namespace {

                // Saves the entries @p pick chooses (or every changed one), then writes the store;
                // on a failed write, the saved values are as before.
                Error saveSome(int only) {
                        Mutex::Locker saving(*saveLock, WaitForever);
                        struct Before {
                                        uint16_t index;
                                        bool     hasSaved;
                                        String   saved{MemSpace::Psram};
                        };
                        Before *before = nullptr;
                        size_t  changed = 0;
                        String  text(MemSpace::Psram);
                        Error   err;
                        {
                                Mutex::Locker lock(*registryLock, WaitForever);
                                before = static_cast<Before *>(
                                        Memory::alloc(MemSpace::Psram, state.count * sizeof(Before) + 1));
                                if (before == nullptr) return Error::NoMem;
                                for (size_t i = 0; i < state.count && err.isOk(); ++i) {
                                        if (only >= 0 && int(i) != only) continue;
                                        Entry     &e = state.entries[i];
                                        const bool isDefault = e.current.view() == e.defaultValue.view();
                                        if (e.hasSaved ? e.saved.view() == e.current.view() && !isDefault : isDefault)
                                                continue;
                                        Before &b = *::new (&before[changed++]) Before();
                                        b.index = static_cast<uint16_t>(i);
                                        b.hasSaved = e.hasSaved;
                                        err = b.saved.assign(e.saved.view());
                                        if (err.isError()) break;
                                        // The default is saved as no line, so a new release's default applies.
                                        e.hasSaved = !isDefault;
                                        err = e.saved.assign(isDefault ? StringView() : e.current.view());
                                        removeOrphanLocked(StringView(e.param->key));
                                }
                                if (err.isOk() && changed != 0) err = buildStoreLocked(text);
                        }
                        if (err.isOk() && changed != 0) err = writeStore(text);
                        if (err.isError()) {
                                Mutex::Locker lock(*registryLock, WaitForever);
                                for (size_t i = 0; i < changed; ++i) {
                                        Entry &e = state.entries[before[i].index];
                                        e.hasSaved = before[i].hasSaved;
                                        (void)e.saved.assign(before[i].saved.view());
                                }
                        }
                        for (size_t i = 0; i < changed; ++i) before[i].~Before();
                        Memory::free(before);
                        return err;
                }

        } // namespace

        Error save(StringView key) {
                const int index = find(key);
                if (index < 0) return Error::NotExist;
                return saveSome(index);
        }

        Error saveAll() {
                if (!isReady()) return Error::InvalidState;
                return saveSome(-1);
        }

        namespace {

                Error fillInfo(size_t index, Info &out) {
                        const Entry &e = state.entries[index];
                        out.param = e.param;
                        out.hasSaved = e.hasSaved;
                        out.changed = e.current.view() != (e.hasSaved ? e.saved.view() : e.defaultValue.view());
                        Error err = out.value.assign(e.current.view());
                        if (err.isOk()) err = out.saved.assign(e.saved.view());
                        return err;
                }

        } // namespace

        Error info(StringView key, Info &out) {
                if (!isReady()) return Error::NotExist;
                Mutex::Locker lock(*registryLock, WaitForever);
                const int     index = findLocked(key);
                if (index < 0) return Error::NotExist;
                return fillInfo(size_t(index), out);
        }

        Error list(StringView prefix, ListFn fn, void *arg) {
                if (!isReady()) return Error::Ok;
                // Every key under the prefix sorts among the keys starting with its text.
                Info   info;
                size_t at = 0;
                {
                        Mutex::Locker lock(*registryLock, WaitForever);
                        at = lowerBound(prefix);
                }
                for (;; ++at) {
                        {
                                Mutex::Locker lock(*registryLock, WaitForever);
                                if (at >= state.count || !keyAt(state.order[at]).startsWith(prefix)) return Error::Ok;
                                if (!underPrefix(keyAt(state.order[at]), prefix)) continue; // "wm/palette2"
                                const Error err = fillInfo(state.order[at], info);
                                if (err.isError()) return err;
                        }
                        fn(arg, info);
                }
        }

        Error declare(const Param &param) {
                if (!isReady()) return Error::InvalidState;
                String canon(MemSpace::Psram);
                if (param.key == nullptr || !Config::isValidKey(StringView(param.key))) return Error::Invalid;
                Error err = Config::canonical(
                        param, StringView(param.defaultValue != nullptr ? param.defaultValue : ""), canon);
                if (err.isError()) return err;
                Mutex::Locker lock(*registryLock, WaitForever);
                const int     existing = findLocked(StringView(param.key));
                if (existing >= 0)
                        return state.entries[existing].param->type == param.type ? Error(Error::Ok)
                                                                                 : Error(Error::Exists);
                if (state.ownedCount == MaxRuntime) return Error::Full;
                Owned &o = *::new (&state.owned[state.ownedCount]) Owned();
                err = o.key.assign(StringView(param.key));
                if (err.isOk()) err = o.defaultValue.assign(canon.view());
                if (err.isOk()) err = o.help.assign(StringView(param.help != nullptr ? param.help : ""));
                if (err.isOk() && param.choices != nullptr) err = o.choices.assign(StringView(param.choices));
                if (err.isError()) {
                        o.~Owned();
                        return err;
                }
                o.param = param;
                o.param.key = o.key.cStr();
                o.param.defaultValue = o.defaultValue.cStr();
                o.param.help = o.help.cStr();
                o.param.choices = param.choices != nullptr ? o.choices.cStr() : nullptr;
                o.param.check = nullptr; // a program's code may go away
                err = addLocked(o.param);
                if (err.isError()) {
                        o.~Owned();
                        return err;
                }
                ++state.ownedCount;
                // Its saved value, kept for it.
                for (size_t i = 0; i < state.orphanCount; ++i) {
                        if (state.orphans[i].key.view() != StringView(param.key)) continue;
                        if (applySavedLocked(size_t(findLocked(StringView(param.key))), state.orphans[i].value.view()))
                                removeOrphanLocked(StringView(param.key));
                        break;
                }
                return Error::Ok;
        }

        // --- Watcher ----------------------------------------------------------------------

        Watcher::~Watcher() {
                stop();
        }

        Error Watcher::start(StringView prefix, Notify fn, void *arg) {
                if (_started) return Error::InvalidState;
                if (!prefix.isEmpty() && !Config::isValidKey(prefix)) return Error::Invalid;
                if (!registryLock.isCreated()) return Error::InvalidState;
                Mutex::Locker notifying(*notifyLock, WaitForever);
                Mutex::Locker lock(*registryLock, WaitForever);
                std::memcpy(_prefix, prefix.data(), prefix.size());
                _prefix[prefix.size()] = '\0';
                notify = fn;
                notifyArg = arg;
                _head = _count = 0;
                _overflowed = false;
                nextWatcher = state.watchers;
                state.watchers = this;
                _started = true;
                return Error::Ok;
        }

        void Watcher::stop() {
                if (!_started) return;
                Mutex::Locker notifying(*notifyLock, WaitForever);
                Mutex::Locker lock(*registryLock, WaitForever);
                for (Watcher **w = &state.watchers; *w != nullptr; w = &(*w)->nextWatcher) {
                        if (*w != this) continue;
                        *w = nextWatcher;
                        break;
                }
                nextWatcher = nullptr;
                _started = false;
                return;
        }

        bool Watcher::matches(StringView key) const {
                return underPrefix(key, StringView(_prefix));
        }

        void Watcher::push(uint16_t entry) {
                // A key already waiting isn't queued twice.
                for (size_t i = 0; i < _count; ++i)
                        if (_ring[(_head + i) % RingSize] == entry) return;
                if (_count == RingSize) {
                        _overflowed = true;
                        return;
                }
                _ring[(_head + _count) % RingSize] = entry;
                ++_count;
                return;
        }

        bool Watcher::next(String &key) {
                uint16_t entry = 0;
                {
                        Mutex::Locker lock(*registryLock, WaitForever);
                        if (_count == 0) return false;
                        entry = _ring[_head];
                        _head = static_cast<uint8_t>((_head + 1) % RingSize);
                        --_count;
                }
                return detail::keyOf(entry, key).isOk();
        }

        bool Watcher::overflowed() {
                Mutex::Locker lock(*registryLock, WaitForever);
                const bool    was = _overflowed;
                _overflowed = false;
                return was;
        }

        Error detail::keyOf(uint16_t entry, String &out) {
                Mutex::Locker lock(*registryLock, WaitForever);
                if (entry >= state.count) return Error::NotExist;
                return out.assign(StringView(state.entries[entry].param->key));
        }

} // namespace SysConf

BMDS_NAMESPACE_END
