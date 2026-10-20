/**
 * @file      smf.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for libbmdsgame's MIDI file walker (sdk/game/smf.c,
 * devplan/audio.md §25), which reads the music files games give it: every
 * event's bytes inside the file or its own buffer, times never going back,
 * walked twice (a rewind) alike.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "../../sdk/game/game_internal.h"
}

namespace {

        // The events' times and a sum of their bytes; checks as it goes.
        uint64_t walk(bmds_game_smf_t &smf, size_t &count) {
                bmds_game_smf_event_t e;
                uint64_t              sum = 0;
                int64_t               last = 0;
                count = 0;
                while (bmds_game_smf_next(&smf, &e) == 1 && count < 100000) {
                        if (e.ns < last) std::abort(); // time goes forward
                        last = e.ns;
                        if (e.kind == BMDS_GAME_SMF_MESSAGE) {
                                if (e.length < 2 || e.length > 3 || (e.data[0] & 0x80) == 0) std::abort();
                                for (uint8_t i = 0; i < e.length; ++i) sum = sum * 31 + e.data[i];
                        } else {
                                if (e.count == 0 || e.count > sizeof(e.sysex)) std::abort();
                                for (uint32_t i = 0; i < e.count; ++i) sum = sum * 31 + e.bytes[i];
                        }
                        sum = sum * 31 + uint64_t(e.ns);
                        ++count;
                }
                return sum;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static bmds_game_smf_t smf;
        if (bmds_game_smf_open(&smf, data, size) != 0) return 0;
        size_t         n1 = 0, n2 = 0;
        const uint64_t a = walk(smf, n1);
        bmds_game_smf_rewind(&smf);
        const uint64_t b = walk(smf, n2);
        if (n1 != n2 || a != b) std::abort();
        return 0;
}
