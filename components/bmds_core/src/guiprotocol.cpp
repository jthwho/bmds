/**
 * @file      guiprotocol.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/guiprotocol.h>

#include <cstring>

BMDS_NAMESPACE_BEGIN

namespace GuiProtocol {

        // ---- Writer

        void Writer::put(const uint8_t *p, size_t n) {
                if (!_ok) return;
                for (size_t i = 0; i < n; ++i)
                        if (_out.append(p[i]).isError()) {
                                _ok = false;
                                return;
                        }
                return;
        }

        void Writer::beginRequest(Opcode opcode, uint16_t flags) {
                _start = _out.size();
                u16(uint16_t(opcode));
                u16(flags);
                u32(0); // the length, set by end()
                return;
        }

        void Writer::beginAnswer(Kind kind, uint16_t code, uint32_t sequence) {
                _start = _out.size();
                u16(uint16_t(kind));
                u16(code);
                u32(0);
                u32(sequence);
                return;
        }

        void Writer::end() {
                if (!_ok) return;
                while ((_out.size() - _start) % 4 != 0) put(reinterpret_cast<const uint8_t *>("\0"), 1);
                const uint32_t length = uint32_t(_out.size() - _start);
                for (int i = 0; i < 4; ++i) _out[_start + 4 + size_t(i)] = uint8_t(length >> (8 * i));
                return;
        }

        void Writer::u16(uint16_t v) {
                const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
                put(b, 2);
                return;
        }

        void Writer::u32(uint32_t v) {
                const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
                put(b, 4);
                return;
        }

        void Writer::i64(int64_t v) {
                u32(uint32_t(uint64_t(v)));
                u32(uint32_t(uint64_t(v) >> 32));
                return;
        }

        void Writer::bytes(ByteSpan data) {
                put(data.data(), data.size());
                static constexpr uint8_t Zero[3] = {};
                put(Zero, padded(data.size()) - data.size());
                return;
        }

        void Writer::string(StringView s) {
                u32(uint32_t(s.size()));
                bytes(ByteSpan(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
                return;
        }

        void Writer::listHeader(Tag elements, uint32_t count) {
                u32(uint32_t(Tag::List));
                u32(uint32_t(elements));
                u32(count);
                return;
        }

        void Writer::element(const Value &v) {
                switch (v.tag) {
                        case Tag::None: break;
                        case Tag::Int:
                        case Tag::Bool:
                        case Tag::Atom:
                        case Tag::Id: u32(uint32_t(v.number)); break;
                        case Tag::Long: i64(v.number); break;
                        case Tag::String:
                        case Tag::Bytes: string(v.text); break;
                        case Tag::Rect:
                                i32(v.rect.x);
                                i32(v.rect.y);
                                u32(v.rect.width);
                                u32(v.rect.height);
                                break;
                        case Tag::Color:
                                u32(uint32_t(v.number) & 0xffffffu);
                                u32(v.role);
                                break;
                        case Tag::List:
                                u32(uint32_t(v.elements));
                                u32(v.count);
                                bytes(v.items);
                                break;
                }
                return;
        }

        void Writer::value(const Value &v) {
                u32(uint32_t(v.tag));
                element(v);
                return;
        }

        // ---- Reader

        bool Reader::fits(size_t n) {
                if (!_ok || n > _data.size() - _at) {
                        _ok = false;
                        return false;
                }
                return true;
        }

        uint16_t Reader::u16() {
                if (!fits(2)) return 0;
                const uint16_t v = uint16_t(_data[_at] | _data[_at + 1] << 8);
                _at += 2;
                return v;
        }

        uint32_t Reader::u32() {
                if (!fits(4)) return 0;
                const uint32_t v = uint32_t(_data[_at]) | uint32_t(_data[_at + 1]) << 8 |
                                   uint32_t(_data[_at + 2]) << 16 | uint32_t(_data[_at + 3]) << 24;
                _at += 4;
                return v;
        }

        int64_t Reader::i64() {
                const uint64_t low = u32();
                const uint64_t high = u32();
                return int64_t(low | high << 32);
        }

        ByteSpan Reader::bytes(size_t n) {
                if (!fits(padded(n))) return ByteSpan();
                const ByteSpan r = _data.mid(_at, n);
                _at += padded(n);
                return r;
        }

        StringView Reader::string() {
                const uint32_t n = u32();
                const ByteSpan b = bytes(n);
                return StringView(reinterpret_cast<const char *>(b.data()), b.size());
        }

        void Reader::skip(size_t n) {
                if (fits(n)) _at += n;
                return;
        }

        bool Reader::element(Tag tag, Value &out) {
                out = Value();
                out.tag = tag;
                switch (tag) {
                        case Tag::None: break;
                        case Tag::Int: out.number = int32_t(u32()); break;
                        case Tag::Bool: {
                                const uint32_t b = u32();
                                if (b > 1) _ok = false;
                                out.number = b;
                                break;
                        }
                        case Tag::Atom:
                        case Tag::Id: out.number = u32(); break;
                        case Tag::Long: out.number = i64(); break;
                        case Tag::String:
                        case Tag::Bytes: out.text = string(); break;
                        case Tag::Rect:
                                out.rect.x = i32();
                                out.rect.y = i32();
                                out.rect.width = u32();
                                out.rect.height = u32();
                                break;
                        case Tag::Color:
                                out.number = u32() & 0xffffffu;
                                out.role = u32();
                                break;
                        case Tag::List: {
                                out.elements = Tag(u32());
                                out.count = u32();
                                if (out.elements == Tag::List || uint32_t(out.elements) > uint32_t(Tag::Bytes)) {
                                        _ok = false;
                                        break;
                                }
                                // The elements' extent: read through them once.
                                const size_t start = _at;
                                Value        e;
                                for (uint32_t i = 0; i < out.count && _ok; ++i) element(out.elements, e);
                                if (_ok) out.items = _data.mid(start, _at - start);
                                out.tag = Tag::List;
                                break;
                        }
                        default: _ok = false; break;
                }
                return _ok;
        }

        bool Reader::value(Value &out) {
                const uint32_t tag = u32();
                if (!_ok || tag > uint32_t(Tag::Bytes)) {
                        _ok = false;
                        return false;
                }
                return element(Tag(tag), out);
        }

        int64_t messageLength(ByteSpan buffered, size_t header) {
                if (buffered.size() < 8) return 0;
                const uint32_t length = uint32_t(buffered[4]) | uint32_t(buffered[5]) << 8 |
                                        uint32_t(buffered[6]) << 16 | uint32_t(buffered[7]) << 24;
                if (length % 4 != 0 || length < header || length > MaxMessage) return -1;
                return length;
        }

} // namespace GuiProtocol

BMDS_NAMESPACE_END
