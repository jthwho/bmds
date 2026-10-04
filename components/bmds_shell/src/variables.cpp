/**
 * @file      variables.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/variables.h>

BMDS_NAMESPACE_BEGIN

size_t Variables::lowerBound(StringView name) const {
        size_t lo = 0, hi = _entries.size();
        while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                if (_entries[mid].name.view() < name) {
                        lo = mid + 1;
                } else {
                        hi = mid;
                }
        }
        return lo;
}

const Variables::Entry *Variables::find(StringView name) const {
        const size_t i = lowerBound(name);
        return i < _entries.size() && _entries[i].name == name ? &_entries[i] : nullptr;
}

const String *Variables::value(StringView name) const {
        const Entry *e = find(name);
        return e != nullptr && (e->flags & IsSet) != 0 ? &e->value : nullptr;
}

Result<Variables::Entry *> Variables::findOrCreate(StringView name) {
        const size_t i = lowerBound(name);
        if (i < _entries.size() && _entries[i].name == name) return makeResult(&_entries[i]);
        Entry e;
        Error err = e.name.assign(name);
        if (err.isOk()) err = _entries.insert(i, std::move(e));
        if (err.isError()) return makeError<Entry *>(err);
        return makeResult(&_entries[i]);
}

Error Variables::set(StringView name, StringView value) {
        const Entry *existing = find(name);
        if (existing != nullptr && (existing->flags & ReadOnly) != 0) return Error::ReadOnly;
        auto [e, err] = findOrCreate(name);
        if (err.isError()) return err;
        err = e->value.assign(value);
        if (err.isError()) return err;
        e->flags |= IsSet;
        return Error::Ok;
}

Error Variables::addFlags(StringView name, uint8_t flags) {
        auto [e, err] = findOrCreate(name);
        if (err.isError()) return err;
        e->flags |= static_cast<uint8_t>(flags & ~IsSet);
        return Error::Ok;
}

Error Variables::unset(StringView name) {
        const size_t i = lowerBound(name);
        if (i >= _entries.size() || !(_entries[i].name == name)) return Error::Ok;
        if ((_entries[i].flags & ReadOnly) != 0) return Error::ReadOnly;
        _entries.removeAt(i);
        return Error::Ok;
}

Error Variables::restore(StringView name, bool existed, StringView value, uint8_t flags) {
        if (!existed) {
                const size_t i = lowerBound(name);
                if (i < _entries.size() && _entries[i].name == name) _entries.removeAt(i);
                return Error::Ok;
        }
        auto [e, err] = findOrCreate(name);
        if (err.isError()) return err;
        err = e->value.assign(value);
        if (err.isOk()) e->flags = flags;
        return err;
}

Result<Variables> Variables::clone() const {
        Variables copy;
        Error     err = copy._entries.reserve(_entries.size());
        for (size_t i = 0; err.isOk() && i < _entries.size(); ++i) {
                Entry e;
                err = e.name.assign(_entries[i].name);
                if (err.isOk()) err = e.value.assign(_entries[i].value);
                e.flags = _entries[i].flags;
                if (err.isOk()) err = copy._entries.append(std::move(e));
        }
        return Result<Variables>(std::move(copy), err);
}

BMDS_NAMESPACE_END
