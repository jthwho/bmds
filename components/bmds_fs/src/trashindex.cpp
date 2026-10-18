/**
 * @file      trashindex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "trashindex.h"

#include <bmds/list.h>
#include <bmds/psramobject.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // An entry: its names in the table's text, and its mount slot.
        struct Item {
                        uint32_t real = 0;
                        uint32_t flat = 0;
                        uint16_t realLength = 0;
                        uint16_t flatLength = 0;
                        uint8_t  slot = 0;
        };

        struct Table {
                        String         text{MemSpace::Psram}; // every name, back to back
                        List<Item>     items{MemSpace::Psram};
                        List<uint32_t> byFlat{MemSpace::Psram}; // items by flat name
                        List<uint32_t> byReal{MemSpace::Psram}; // items by slot, then real name
                        bool           built = false;
                        uint64_t       folders[Vfs::MaxMounts * 2] = {}; // each trash's files and info ids
                        size_t         folderCount = 0;
        };

        constinit PsramObject<Table> table;

        Table *tableLocked() {
                return table.isCreated() ? table.ptr() : table.create();
        }

        StringView realOf(const Table &t, const Item &item) {
                return StringView(t.text.data() + item.real, item.realLength);
        }

        StringView flatOf(const Table &t, const Item &item) {
                return StringView(t.text.data() + item.flat, item.flatLength);
        }

        // Where @p flat is, or would go, in byFlat.
        size_t flatPlace(const Table &t, StringView flat, bool &found) {
                size_t lo = 0, hi = t.byFlat.size();
                while (lo < hi) {
                        const size_t mid = (lo + hi) / 2;
                        const auto   order = flatOf(t, t.items[t.byFlat[mid]]) <=> flat;
                        if (order == 0) {
                                found = true;
                                return mid;
                        }
                        if (order < 0) {
                                lo = mid + 1;
                        } else {
                                hi = mid;
                        }
                }
                found = false;
                return lo;
        }

        // Where (@p slot, @p real) is in byReal, or byReal's size.
        size_t realPlace(const Table &t, size_t slot, StringView real) {
                size_t lo = 0, hi = t.byReal.size();
                while (lo < hi) {
                        const size_t mid = (lo + hi) / 2;
                        const Item  &item = t.items[t.byReal[mid]];
                        const auto   order = item.slot != slot ? (item.slot <=> slot) : (realOf(t, item) <=> real);
                        if (order == 0) return mid;
                        if (order < 0) {
                                lo = mid + 1;
                        } else {
                                hi = mid;
                        }
                }
                return t.byReal.size();
        }

        // Writes @p real with " (LABEL)" or " (LABEL N)" before its extension into @p out.
        Error labelled(StringView real, StringView label, unsigned n, String &out) {
                out.clear();
                const size_t     dot = real.lastIndexOf('.');
                const bool       ext = dot != StringView::NPos && dot > 0;
                const StringView base = ext ? real.left(dot) : real;
                Error            err = out.append(base);
                if (err.isOk())
                        err = n < 2 ? out.appendf(" (%.*s)", int(label.size()), label.data())
                                    : out.appendf(" (%.*s %u)", int(label.size()), label.data(), n);
                if (err.isOk() && ext) err = out.append(real.mid(dot));
                return err;
        }

        // Reads every trash's info folder and names the entries. Lock held.
        Error buildLocked(Table &t, Span<const TrashIndex::Mount> mounts) {
                t.text.clear();
                t.items.clear();
                t.byFlat.clear();
                t.byReal.clear();
                // The mounts by path: the first by path keeps a name.
                size_t order[Vfs::MaxMounts];
                size_t count = 0;
                for (size_t i = 0; i < mounts.size() && i < Vfs::MaxMounts; ++i) {
                        size_t at = count++;
                        while (at > 0 && mounts[order[at - 1]].path > mounts[i].path) {
                                order[at] = order[at - 1];
                                --at;
                        }
                        order[at] = i;
                }
                String label, flat;
                for (size_t k = 0; k < count; ++k) {
                        const size_t             slot = order[k];
                        const TrashIndex::Mount &m = mounts[slot];
                        bool                     noPath = false;
                        auto [cursor, err] = m.volume->openDirectory(TrashIndex::InfoRest, noPath);
                        if (err.isError()) continue; // no trash there (or unreadable): nothing listed
                        label.clear();
                        for (char c : m.path)
                                if (c != '/') (void)label.append(c);
                        const size_t first = t.items.size();
                        FileInfo     info;
                        for (;;) {
                                err = cursor->next(info);
                                if (err.isError()) break;
                                const StringView name = info.name.view();
                                if (info.isDirectory() || !name.endsWith(TrashIndex::InfoSuffix) ||
                                    name.size() == TrashIndex::InfoSuffix.size())
                                        continue;
                                const StringView real = name.left(name.size() - TrashIndex::InfoSuffix.size());
                                Item             item;
                                item.real = static_cast<uint32_t>(t.text.size());
                                item.realLength = static_cast<uint16_t>(real.size());
                                item.slot = static_cast<uint8_t>(slot);
                                err = t.text.append(real);
                                if (err.isOk()) err = t.items.append(item);
                                if (err.isError()) break;
                        }
                        m.volume->closeDirectory(cursor);
                        if (err.isError() && err != Error::EndOfFile) return err;
                        // Their flat names: their own, else labelled.
                        String own;
                        for (size_t i = first; i < t.items.size(); ++i) {
                                // A copy: the text moves as it grows.
                                err = own.assign(realOf(t, t.items[i]));
                                if (err.isError()) return err;
                                StringView name = own.view();
                                bool       taken = false;
                                size_t     place = flatPlace(t, name, taken);
                                for (unsigned n = 1; taken && n < 1000; ++n) {
                                        err = labelled(own.view(), label.view(), n, flat);
                                        if (err.isError()) return err;
                                        name = flat.view();
                                        place = flatPlace(t, name, taken);
                                }
                                if (taken) continue; // a thousand alike: not listed
                                t.items[i].flat = static_cast<uint32_t>(t.text.size());
                                t.items[i].flatLength = static_cast<uint16_t>(name.size());
                                err = t.text.append(name);
                                if (err.isOk()) err = t.byFlat.insert(place, static_cast<uint32_t>(i));
                                if (err.isError()) return err;
                        }
                }
                // By slot, then real name (an insertion sort: entries come mostly in order).
                for (size_t i = 0; i < t.items.size(); ++i) {
                        if (t.items[i].flatLength == 0) continue;
                        const Item &item = t.items[i];
                        size_t      at = t.byReal.size();
                        while (at > 0) {
                                const Item &prev = t.items[t.byReal[at - 1]];
                                if (prev.slot < item.slot ||
                                    (prev.slot == item.slot && realOf(t, prev) <= realOf(t, item)))
                                        break;
                                --at;
                        }
                        const Error err = t.byReal.insert(at, static_cast<uint32_t>(i));
                        if (err.isError()) return err;
                }
                t.built = true;
                return Error::Ok;
        }

        Table *readyLocked(Span<const TrashIndex::Mount> mounts) {
                Table *t = tableLocked();
                if (t == nullptr) return nullptr;
                if (!t->built && buildLocked(*t, mounts).isError()) {
                        t->built = false;
                        return nullptr;
                }
                return t;
        }

} // namespace

void TrashIndex::mountsChangedLocked(Span<const Mount> mounts) {
        Table *t = tableLocked();
        if (t == nullptr) return;
        t->built = false;
        t->folderCount = 0;
        String path;
        for (size_t i = 0; i < mounts.size() && i < Vfs::MaxMounts; ++i) {
                for (const StringView rest : {FilesRest, InfoRest}) {
                        path.clear();
                        if (mounts[i].path != "/" && path.append(mounts[i].path).isError()) continue;
                        if (path.append(rest).isError()) continue;
                        t->folders[t->folderCount++] = Fs::pathId(path.view());
                }
        }
        return;
}

bool TrashIndex::changedLocked(uint64_t folder) {
        if (!table.isCreated()) return false;
        Table &t = *table.ptr();
        for (size_t i = 0; i < t.folderCount; ++i) {
                if (t.folders[i] == folder) {
                        t.built = false;
                        return true;
                }
        }
        return false;
}

bool TrashIndex::findLocked(Span<const Mount> mounts, StringView flat, size_t &slot, StringView &real) {
        const Table *t = readyLocked(mounts);
        if (t == nullptr) return false;
        bool         found = false;
        const size_t at = flatPlace(*t, flat, found);
        if (!found) return false;
        const Item &item = t->items[t->byFlat[at]];
        slot = item.slot;
        real = realOf(*t, item);
        return true;
}

StringView TrashIndex::flatNameLocked(Span<const Mount> mounts, size_t slot, StringView real) {
        const Table *t = readyLocked(mounts);
        if (t == nullptr) return StringView();
        const size_t at = realPlace(*t, slot, real);
        return at < t->byReal.size() ? flatOf(*t, t->items[t->byReal[at]]) : StringView();
}

BMDS_NAMESPACE_END
