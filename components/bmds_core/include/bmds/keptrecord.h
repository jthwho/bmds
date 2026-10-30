/**
 * @file      keptrecord.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A record kept where a reset leaves memory alone (`devplan/keptstate.md` §2c): its bytes
 *        in two copies, each with a header (magic, size, sequence, check), written alternately.
 *
 * `save()` writes the older copy, its magic cleared first and set last, so a reset partway leaves
 * the other. `load()` takes the newer copy that passes: the magic, the size, and an FNV-1a check
 * over the header and the bytes. Memory that held something else (power-up's garbage, another
 * firmware's objects) doesn't pass. Each copy is read and written whole, a word at a time where
 * it can: the LP RAM's accesses are slow (about 500 ns a word).
 *
 * Portable: the storage is the caller's (`RTC_NOINIT_ATTR` on the target, an array in a test).
 *
 * @par Thread Safety
 * Not thread-safe: its owner's lock.
 */
class KeptRecord {
        public:
                /** @brief A copy's header. */
                static constexpr size_t HeaderBytes = 16;

                /** @brief The 32-bit words of storage two copies of @p payloadBytes need. */
                static constexpr size_t storageWords(size_t payloadBytes) {
                        return 2 * (HeaderBytes / 4 + (payloadBytes + 3) / 4);
                }

                /**
                 * @brief A record of @p payloadBytes in @p storage (`storageWords()` words),
                 *        @p magic telling it from anything else there.
                 */
                constexpr KeptRecord(uint32_t *storage, size_t payloadBytes, uint32_t magic)
                    : _storage(storage), _payloadBytes(uint32_t(payloadBytes)), _magic(magic) {}

                /**
                 * @brief Copies the newer copy that passes into @p payload.
                 * @return True if one did; false leaves @p payload's contents unspecified.
                 */
                bool load(void *payload);

                /** @brief Writes @p payload into the older copy. */
                void save(const void *payload);

        private:
                uint32_t *copyAt(uint32_t index) const {
                        return _storage + index * (HeaderBytes / 4 + (_payloadBytes + 3) / 4);
                }
                uint32_t checkOf(uint32_t sequence, const void *payload) const;
                bool     loadCopy(uint32_t index, void *payload, uint32_t &sequence) const;

                uint32_t *_storage;
                uint32_t  _payloadBytes;
                uint32_t  _magic;
                uint32_t  _sequence = 0; // the newest copy's
};

BMDS_NAMESPACE_END
