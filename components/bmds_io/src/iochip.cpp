/**
 * @file      iochip.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lookups in a chip's I/O table (devplan/io.md §3.1).
 */

#include <cstdio>
#include <bmds/iochip.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Parses a decimal number that is all of @p text.
        bool parseNumber(StringView text, unsigned &out) {
                if (text.isEmpty() || text.size() > 5) return false;
                unsigned n = 0;
                for (char c : text) {
                        if (c < '0' || c > '9') return false;
                        n = n * 10 + static_cast<unsigned>(c - '0');
                }
                out = n;
                return true;
        }

        Result<uint16_t> findSignal(Span<const char *const> names, StringView name) {
                for (size_t i = 0; i < names.size(); ++i) {
                        if (names[i] != nullptr && name.equalsIgnoreCase(StringView(names[i])))
                                return makeResult(static_cast<uint16_t>(i));
                }
                return makeError<uint16_t>(Error::NotExist);
        }

} // namespace

const IoMuxFunction *IoChip::function(size_t pad, uint8_t index) const {
        for (const IoMuxFunction &f : padFunctions(pad)) {
                if (f.index == index) return &f;
        }
        return nullptr;
}

const char *IoChip::functionName(size_t pad, uint8_t index) const {
        const IoMuxFunction *f = function(pad, index);
        return f != nullptr ? f->name : nullptr;
}

size_t IoChip::resourceCount() const {
        size_t n = pads.size();
        for (const IoKindInfo &k : kinds) n += k.count;
        return n;
}

Result<IoResource> IoChip::resource(IoKind kind, unsigned number) const {
        if (kind == IoKind::Pad) {
                if (number >= pads.size()) return makeError<IoResource>(Error::NotExist);
                return makeResult(static_cast<IoResource>(number));
        }
        size_t base = pads.size();
        for (const IoKindInfo &k : kinds) {
                if (k.kind == kind) {
                        if (number < k.first || number >= static_cast<unsigned>(k.first) + k.count)
                                return makeError<IoResource>(Error::NotExist);
                        return makeResult(static_cast<IoResource>(base + number - k.first));
                }
                base += k.count;
        }
        return makeError<IoResource>(Error::NotExist);
}

Result<IoKindInfo> IoChip::kindOf(IoResource resource, unsigned *number) const {
        if (resource < pads.size()) {
                if (number != nullptr) *number = resource;
                return makeResult(IoKindInfo{IoKind::Pad, "gpio", 0, static_cast<uint8_t>(pads.size())});
        }
        size_t base = pads.size();
        for (const IoKindInfo &k : kinds) {
                if (resource < base + k.count) {
                        if (number != nullptr) *number = static_cast<unsigned>(k.first + (resource - base));
                        return makeResult(k);
                }
                base += k.count;
        }
        return makeError<IoKindInfo>(Error::NotExist);
}

size_t IoChip::resourceName(IoResource resource, char *out, size_t size) const {
        unsigned number = 0;
        auto [kind, err] = kindOf(resource, &number);
        if (err.isError() || out == nullptr || size == 0) {
                if (out != nullptr && size > 0) out[0] = '\0';
                return 0;
        }
        const int n = kind.kind == IoKind::Pad ? std::snprintf(out, size, "GPIO%u", number)
                                               : std::snprintf(out, size, "%s%u", kind.name, number);
        if (n < 0) {
                out[0] = '\0';
                return 0;
        }
        return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

Result<IoResource> IoChip::findResource(StringView text) const {
        unsigned number = 0;
        if (text.size() > 4 && text.left(4).equalsIgnoreCase("gpio") && parseNumber(text.mid(4), number))
                return resource(IoKind::Pad, number);
        for (const IoKindInfo &k : kinds) {
                const StringView kindName(k.name);
                if (text.size() > kindName.size() && text.left(kindName.size()).equalsIgnoreCase(kindName) &&
                    parseNumber(text.mid(kindName.size()), number))
                        return resource(k.kind, number);
        }
        return makeError<IoResource>(Error::NotExist);
}

Result<uint16_t> IoChip::findInput(StringView text) const {
        return findSignal(inputSignals, text);
}

Result<uint16_t> IoChip::findOutput(StringView text) const {
        return findSignal(outputSignals, text);
}

BMDS_NAMESPACE_END
