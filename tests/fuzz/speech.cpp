/**
 * @file      speech.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for text to speech (devplan/speech.md): any text, as any
 * program may write to a speech fd, spoken by the en-US voice: markup obeyed
 * or read, a speed, pitch and volume, put in pieces of any size, NULs and
 * broken UTF-8 included; nothing may fault, read out of bounds or leak.
 *
 * The input: a byte of settings (bit 0 markup; bits 1-2 the prosody), a byte
 * giving the pieces' size, then the text.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <bmds/speaker.h>

using bmds::ByteSpan;
using bmds::Speaker;

namespace {

        struct File {
                        uint8_t *data = nullptr;
                        size_t   size = 0;
        };

        File slurp(const char *name) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/%s", BMDS_SPEECH_DIR, name);
                FILE *f = std::fopen(path, "rb");
                if (f == nullptr) {
                        std::fprintf(stderr, "speech_fuzz: %s: not found\n", path);
                        std::abort();
                }
                File file;
                std::fseek(f, 0, SEEK_END);
                file.size = static_cast<size_t>(std::ftell(f));
                std::rewind(f);
                file.data = static_cast<uint8_t *>(std::malloc(file.size));
                if (std::fread(file.data, 1, file.size, f) != file.size) std::abort();
                std::fclose(f);
                return file;
        }

        // Each run speaks at most this much: long input can't make one run take minutes.
        constexpr size_t MaxSamples = 8 * Speaker::RateHz;

        constexpr Speaker::Prosody Prosodies[4] = {{100, 100, 100}, {500, 200, 500}, {20, 50, 1}, {250, 75, 100}};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static const File analysis = slurp("en-US_ta.bin");
        static const File signal = slurp("en-US_lh0_sg.bin");
        if (size < 2) return 0;
        const bool   markup = (data[0] & 1) != 0;
        const auto   prosody = Prosodies[(data[0] >> 1) & 3];
        const size_t piece = data[1] == 0 ? 1 : data[1];
        data += 2;
        size -= 2;
        Speaker speaker;
        if (speaker.open({ByteSpan(analysis.data, analysis.size), ByteSpan(signal.data, signal.size)}, markup, prosody)
                    .isError())
                std::abort();
        int16_t block[512];
        size_t  at = 0;
        size_t  made = 0;
        bool    ended = false; // the NUL after the input put
        while (made < MaxSamples) {
                if (at < size) {
                        const size_t n = size - at < piece ? size - at : piece;
                        auto [taken, err] = speaker.put(ByteSpan(data + at, n));
                        if (err.isError()) std::abort();
                        at += taken;
                } else if (!ended) {
                        const uint8_t nul = 0;
                        auto [taken, err] = speaker.put(ByteSpan(&nul, 1));
                        if (err.isError()) std::abort();
                        ended = taken == 1;
                }
                auto [n, rerr] = speaker.render(bmds::Span<int16_t>(block, 512));
                if (rerr.isError()) break; // Pico failed and was reset: allowed, not a fault
                made += n;
                if (n == 0 && at == size && ended) break;
        }
        return 0;
}
