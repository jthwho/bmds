/**
 * @file      clipboardreader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Reading the clipboard as a type (devplan/clipboard.md §4b).
 */

#include <cstring>
#include <bmds/clipboardreader.h>

BMDS_NAMESPACE_BEGIN

Error ClipboardReader::open(Clipboard::Selection selection, StringView wanted) {
        _source.reset();
        _steps = 0;
        _ended = false;
        _outAt = _outLength = 0;
        _offered[0] = '\0';
        if (!ClipboardType::isValid(wanted)) return Error::Invalid;
        Clipboard::Description d;
        _clipboard.describe(selection, d);
        if (d.serial == 0) return Error::Empty;
        StringView offered[Clipboard::MaxTypes];
        for (size_t i = 0; i < d.count; ++i) offered[i] = StringView(d.types[i].type);
        const ClipboardConversions *table = _clipboard.conversions();
        ClipboardConversions::Path  path;
        if (table != nullptr) {
                auto [found, err] = table->find(Span<const StringView>(offered, d.count), wanted);
                if (err.isError()) return err;
                path = found;
        } else {
                // No table (no memory): only the type itself.
                size_t i = 0;
                while (i < d.count && !ClipboardType::equal(offered[i], wanted)) ++i;
                if (i == d.count) return Error::NotFound;
                path.offered = static_cast<uint8_t>(i);
        }
        for (size_t s = 0; s < path.steps; ++s) {
                const ClipboardConversions::Builtin b = table->converter(path.converter[s]).builtin;
                if (b == ClipboardConversions::Builtin::None) return Error::NotSupported;
                _converters[s] = ClipboardConverter(b);
        }
        _steps = path.steps;
        auto [handle, err] = _clipboard.request(selection, offered[path.offered], _offered);
        if (err.isError()) {
                _offered[0] = '\0';
                return err;
        }
        _source = std::move(handle);
        return Error::Ok;
}

Result<bool> ClipboardReader::fill(unsigned int timeoutMs) {
        _outAt = _outLength = 0;
        if (_ended || !_source) return makeResult(false);
        auto [n, err] = _source->read(WritableByteSpan(_in, sizeof(_in)), timeoutMs);
        size_t got = err.isOk() ? n : 0;
        if (err == Error::EndOfFile) {
                _ended = true;
        } else if (err.isError()) {
                return makeError<bool>(err);
        }
        // Through the converters: each one's output the next one's input.
        const uint8_t *data = _in;
        for (size_t s = 0; s < _steps; ++s) {
                uint8_t *out = s + 1 == _steps ? _out : _mid;
                size_t   made = _converters[s].put(ByteSpan(data, got), out);
                if (_ended) made += _converters[s].finish(out + made);
                data = out;
                got = made;
        }
        if (_steps == 0) std::memcpy(_out, _in, got);
        _outLength = got;
        return makeResult(_outLength != 0 || !_ended);
}

Result<size_t> ClipboardReader::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (!_source) return makeError<size_t>(Error::InvalidState);
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        while (_outAt == _outLength) {
                auto [more, err] = fill(timeoutMs);
                if (err.isError()) return makeError<size_t>(err);
                if (!more) return makeError<size_t>(Error::EndOfFile);
        }
        size_t n = _outLength - _outAt;
        if (n > buf.size()) n = buf.size();
        std::memcpy(buf.data(), _out + _outAt, n);
        _outAt += n;
        return makeResult(n);
}

BMDS_NAMESPACE_END
