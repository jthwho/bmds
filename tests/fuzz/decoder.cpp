/**
 * @file      decoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the decoders (devplan/audio.md §19): open, decode in
 * each format, seek, decode again, close; nothing may fault, read out of
 * bounds, overflow or leak.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <bmds/decoder.h>
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

} // namespace

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
        if (HostMemory::liveAllocations() != before) abort(); // a leak
        return 0;
}
