/**
 * @file      keptrecord.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Records kept across resets (devplan/keptstate.md §2c).
 */

#include <atomic>
#include <cstring>
#include <bmds/keptrecord.h>

BMDS_NAMESPACE_BEGIN

namespace {

        enum Field : size_t {
                Magic,
                Size,
                Sequence,
                Check
        };

        uint32_t fnv(uint32_t h, const void *data, size_t bytes) {
                const auto *b = static_cast<const uint8_t *>(data);
                for (size_t i = 0; i < bytes; ++i) {
                        h ^= b[i];
                        h *= 16777619u;
                }
                return h;
        }

} // namespace

uint32_t KeptRecord::checkOf(uint32_t sequence, const void *payload) const {
        const uint32_t header[3] = {_magic, _payloadBytes, sequence};
        return fnv(fnv(2166136261u, header, sizeof(header)), payload, _payloadBytes);
}

bool KeptRecord::loadCopy(uint32_t index, void *payload, uint32_t &sequence) const {
        const uint32_t *copy = copyAt(index);
        uint32_t        header[HeaderBytes / 4];
        std::memcpy(header, copy, sizeof(header));
        if (header[Magic] != _magic || header[Size] != _payloadBytes) return false;
        std::memcpy(payload, copy + HeaderBytes / 4, _payloadBytes);
        sequence = header[Sequence];
        return header[Check] == checkOf(sequence, payload);
}

bool KeptRecord::load(void *payload) {
        uint32_t   seq0 = 0;
        uint32_t   seq1 = 0;
        const bool ok0 = loadCopy(0, payload, seq0);
        const bool ok1 = loadCopy(1, payload, seq1); // payload now holds copy 1 if it passed
        if (ok1 && (!ok0 || int32_t(seq1 - seq0) > 0)) {
                _sequence = seq1;
                return true;
        }
        if (!ok0) return false;
        std::memcpy(payload, copyAt(0) + HeaderBytes / 4, _payloadBytes);
        _sequence = seq0;
        return true;
}

void KeptRecord::save(const void *payload) {
        const uint32_t sequence = _sequence + 1;
        uint32_t      *copy = copyAt(sequence & 1);
        copy[Magic] = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::memcpy(copy + HeaderBytes / 4, payload, _payloadBytes);
        copy[Size] = _payloadBytes;
        copy[Sequence] = sequence;
        copy[Check] = checkOf(sequence, payload);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        copy[Magic] = _magic;
        _sequence = sequence;
        return;
}

BMDS_NAMESPACE_END
