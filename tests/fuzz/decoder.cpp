/**
 * @file      decoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the decoders (devplan/audio.md §19): open, decode in
 * each format, seek, decode again, close; nothing may fault, read out of
 * bounds, overflow or leak. MIDI files too (§22), played by the synth on the
 * test font (tests/host/synth/sine.sf2).
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <bmds/decoder.h>
#include <bmds/mididecoder.h>
#include <bmds/soundfont.h>
#include <bmds/synth.h>
#include "hostmemory.h"

using bmds::Decoder;
using bmds::Error;

namespace {

        struct Input {
                        const uint8_t *data;
                        size_t         size;
        };

        Error readInput(void *arg, uint64_t offset, bmds::WritableByteSpan buf) {
                const auto *in = static_cast<const Input *>(arg);
                if (offset > in->size || in->size - offset < buf.size()) return Error::EndOfFile;
                std::memcpy(buf.data(), in->data + offset, buf.size());
                return Error::Ok;
        }

        // Decodes about @p most frames: a damaged length can't run on for long, and the seeds
        // (make-seeds.sh) are a quarter of a second.
        void decodeSome(Decoder &d, size_t most) {
                static uint8_t buf[4096 + 7];
                for (size_t done = 0; done < most;) {
                        // An odd size now and then: whole frames still, the rest unused.
                        auto [n, err] = d.decode(bmds::WritableByteSpan(buf, (done & 1) ? sizeof(buf) : 4096));
                        if (err.isError()) return;
                        if (n == 0) abort(); // nothing and no error: a reader would spin
                        done += n / d.frameBytes();
                }
        }

        // The test font, read once, for MIDI files.
        class FileReader final : public bmds::SoundFont::Reader {
                public:
                        explicit FileReader(FILE *f) : _f(f) {}
                        Error read(void *out, size_t bytes) override {
                                return std::fread(out, 1, bytes, _f) == bytes ? Error::Ok : Error::EndOfFile;
                        }
                        Error skip(size_t bytes) override {
                                return std::fseek(_f, static_cast<long>(bytes), SEEK_CUR) == 0 ? Error::Ok
                                                                                               : Error::EndOfFile;
                        }

                private:
                        FILE *_f;
        };

        bmds::SoundFont *font = nullptr;

} // namespace

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
        FILE *f = std::fopen(BMDS_SYNTH_DIR "/sine.sf2", "rb");
        if (f == nullptr) abort();
        FileReader reader(f);
        auto [loaded, err] = bmds::SoundFont::load(reader);
        std::fclose(f);
        if (err.isError()) abort();
        font = loaded;
        bmds::MidiDecoder::setFonts(
                [](const bmds::Synth::Config &c, bmds::Synth &s) -> bmds::Result<bmds::SoundFont *> {
                        const Error e = s.open(*font, c);
                        return e.isOk() ? bmds::makeResult(font) : bmds::makeError<bmds::SoundFont *>(e);
                },
                [](bmds::Synth &s, bmds::SoundFont *) { s.close(); });
        if (Decoder::addFormat("MThd", bmds::MidiDecoder::open).isError()) abort();
        return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        const size_t before = HostMemory::liveAllocations();
        Input        in{data, size};
        auto [d, err] = Decoder::open({readInput, &in, size});
        if (err.isOk()) {
                const Decoder::Info info = d->info();
                if (info.channels < 1 || info.channels > Decoder::MaxChannels || info.rateHz == 0) abort();
                decodeSome(*d, 12000);
                for (bmds::SampleFormat f : {bmds::SampleFormat::S16Le, bmds::SampleFormat::F32Le}) {
                        if (d->setFormat(f).isError()) continue;
                        const uint64_t to = info.frames != Decoder::UnknownFrames ? info.frames / 3 : size % 100000;
                        if (d->seek(to).isOk() && d->position() != to) abort();
                        decodeSome(*d, 3000);
                }
                for (size_t i = 0; i < d->tagCount(); ++i) (void)d->tag(i);
                Decoder::close(d);
        }
        if (HostMemory::liveAllocations() != before && getenv("BMDS_NO_LEAK_ABORT") == nullptr) abort(); // a leak
        return 0;
}
