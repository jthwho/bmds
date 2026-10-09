/**
 * @file      synth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A General MIDI synth on TinySoundFont (devplan/audio.md §22): fonts, and
 * MIDI messages placed on frames.
 */

#include <atomic>
#include <cstring>
#include <new>
#include <tsf.h>
#include <bmds/memory.h>
#include <bmds/synth.h>
#include "tsfalloc.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr int    MidiChannels = 16;
        constexpr int    DrumChannel = 9;  // channel 10
        // An allocation's size and where its block starts, kept just before it.
        struct Header {
                        size_t bytes;
                        size_t offset; // from the block's start to the allocation
        };
        constexpr size_t SmallHeader = 16; // keeps max_align_t's alignment
        constexpr size_t DmaHeader = 64;   // keeps a cache line's
        static_assert(sizeof(Header) <= SmallHeader, "the header fits before the allocation");

        // Large allocations (a font's samples) are DMA-capable and cache-line aligned, so a card
        // reads straight into them: through the driver's bounce buffer, TimGM6mb's 5.7 MB of
        // samples took 2.1 s to read, against 0.6 s in large aligned reads (storage.md §3).
        constexpr size_t DmaFrom = 64 * 1024;

        // Where a large allocation should sit within its cache line: a font's sample chunk starts
        // anywhere in its file, and FatFs reads the first partial sector through its window, then
        // whole sectors into the destination past it. Those are read by DMA only if that address
        // is aligned, which is when the destination matches the file position (mod 64). The
        // reader sets it as it goes (setLargeAllocationPhase()); loads don't overlap (SoundFonts'
        // lock).
        constinit std::atomic<size_t> largePhase{0};

        Header headerOf(const void *ptr) {
                Header h;
                std::memcpy(&h, static_cast<const uint8_t *>(ptr) - sizeof(Header), sizeof(h));
                return h;
        }

        // The largest allocation tried: a font's sizes come from the file, and none bigger than the
        // PSRAM can fit (a damaged one asked for 3.4 GB while fuzzing).
        constexpr size_t MaxAllocBytes = 32u * 1024 * 1024;

        // Allocations refused: a load that fails with none was a damaged font, not one without memory.
        constinit std::atomic<uint32_t> allocFailures{0};

        // GM System On and the GS reset: the whole SysEx in one event.
        constexpr uint8_t GmSystemOn[] = {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7};

} // namespace

BMDS_NAMESPACE_END

// ---- TinySoundFont's allocator: PSRAM, sized for realloc

extern "C" {

void *bmdsSynthMalloc(size_t bytes) {
        const bool   dma = bytes >= bmds::DmaFrom;
        const size_t offset =
                dma ? bmds::DmaHeader + bmds::largePhase.load(std::memory_order_relaxed) % 64 : bmds::SmallHeader;
        auto *block = bytes > bmds::MaxAllocBytes
                              ? nullptr
                              : static_cast<uint8_t *>(bmds::Memory::alloc(
                                        dma ? bmds::MemSpace::PsramDma : bmds::MemSpace::Psram, bytes + offset));
        if (block == nullptr) {
                bmds::allocFailures.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
        }
        const bmds::Header h{bytes, offset};
        std::memcpy(block + offset - sizeof(h), &h, sizeof(h));
        return block + offset;
}

void bmdsSynthFree(void *ptr) {
        if (ptr != nullptr) bmds::Memory::free(static_cast<uint8_t *>(ptr) - bmds::headerOf(ptr).offset);
        return;
}

void *bmdsSynthRealloc(void *ptr, size_t bytes) {
        if (ptr == nullptr) return bmdsSynthMalloc(bytes);
        const size_t old = bmds::headerOf(ptr).bytes;
        void        *grown = bmdsSynthMalloc(bytes);
        if (grown == nullptr) return nullptr;
        std::memcpy(grown, ptr, old < bytes ? old : bytes);
        bmdsSynthFree(ptr);
        return grown;
}

// tsf.c's: what the font holds, and a preset's bank and number.
size_t bmdsSynthFontBytes(const tsf *f);
void   bmdsSynthPresetId(const tsf *f, int index, unsigned short *bank, unsigned short *number);

size_t bmdsSynthAllocated(const void *ptr) {
        return ptr != nullptr ? bmds::headerOf(ptr).bytes : 0;
}

} // extern "C"

BMDS_NAMESPACE_BEGIN

// ---- SoundFont

namespace {

        struct StreamArg {
                        SoundFont::Reader *reader;
                        Error              error;
        };

        int readStream(void *data, void *ptr, unsigned int size) {
                auto *s = static_cast<StreamArg *>(data);
                if (s->error.isError()) return 0;
                s->error = s->reader->read(ptr, size);
                return s->error.isOk() ? static_cast<int>(size) : 0;
        }

        int skipStream(void *data, unsigned int count) {
                auto *s = static_cast<StreamArg *>(data);
                if (s->error.isError()) return 0;
                s->error = s->reader->skip(count);
                return s->error.isOk() ? 1 : 0;
        }

} // namespace

void SoundFont::setLargeAllocationPhase(size_t position) {
        largePhase.store(position % 64, std::memory_order_relaxed);
        return;
}

Result<SoundFont *> SoundFont::load(Reader &reader) {
        StreamArg      arg{&reader, Error::Ok};
        tsf_stream     stream{&arg, readStream, skipStream};
        const uint32_t failures = allocFailures.load(std::memory_order_relaxed);
        ::tsf         *t = tsf_load(&stream);
        largePhase.store(0, std::memory_order_relaxed);
        if (t == nullptr) {
                // TinySoundFont doesn't say why: a reader's error is the reader's; an allocation refused
                // meanwhile, no memory (or a damaged size past MaxAllocBytes); else damaged.
                if (arg.error.isError() && arg.error != Error::EndOfFile) return makeError<SoundFont *>(arg.error);
                const bool noMem = allocFailures.load(std::memory_order_relaxed) != failures;
                return makeError<SoundFont *>(noMem && arg.error.isOk() ? Error::NoMem : Error::Invalid);
        }
        // TinySoundFont doesn't check every read: a font cut short inside a chunk would load.
        if (arg.error.isError() || tsf_get_presetcount(t) == 0) {
                tsf_close(t);
                return makeError<SoundFont *>(Error::Invalid);
        }
        // TinySoundFont makes a font's use count at its first copy: made now, so that opening a
        // synth allocates the same every time (and the count lives with the font).
        ::tsf *copy = tsf_copy(t);
        if (copy == nullptr) {
                tsf_close(t);
                return makeError<SoundFont *>(Error::NoMem);
        }
        tsf_close(copy);
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(SoundFont));
        if (mem == nullptr) {
                tsf_close(t);
                return makeError<SoundFont *>(Error::NoMem);
        }
        auto *font = ::new (mem) SoundFont();
        font->_tsf = t;
        font->_bytes = bmdsSynthFontBytes(t);
        return makeResult(font);
}

void SoundFont::destroy() {
        tsf_close(_tsf);
        this->~SoundFont();
        Memory::free(this);
        return;
}

size_t SoundFont::presetCount() const {
        return static_cast<size_t>(tsf_get_presetcount(_tsf));
}

SoundFont::Preset SoundFont::preset(size_t index) const {
        Preset         p;
        unsigned short bank = 0;
        unsigned short number = 0;
        bmdsSynthPresetId(_tsf, static_cast<int>(index), &bank, &number);
        p.bank = bank;
        p.number = number;
        const char *name = tsf_get_presetname(_tsf, static_cast<int>(index));
        p.name = name != nullptr ? name : "";
        return p;
}

// ---- Synth

Synth::~Synth() {
        close();
}

Error Synth::open(SoundFont &font, const Config &config) {
        if (_tsf != nullptr) return Error::InvalidState;
        Config c = config;
        if (c.maxVoices == 0) c.maxVoices = DefaultVoices;
        if (c.rateHz < 8000 || c.rateHz > 96000 || c.channels < 1 || c.channels > 2 || c.maxVoices > MaxVoices ||
            !(c.gainDb >= MinGainDb && c.gainDb <= MaxGainDb))
                return Error::Invalid;
        _events = static_cast<Event *>(Memory::alloc(MemSpace::Psram, QueueEvents * sizeof(Event)));
        if (_events == nullptr) return Error::NoMem;
        _tsf = tsf_copy(font.handle());
        if (_tsf == nullptr || !tsf_set_max_voices(_tsf, c.maxVoices)) {
                close();
                return Error::NoMem;
        }
        tsf_set_output(_tsf, c.channels == 2 ? TSF_STEREO_INTERLEAVED : TSF_MONO, static_cast<int>(c.rateHz), c.gainDb);
        _config = c;
        _head = _count = 0;
        _frames = 0;
        _stats = Stats();
        // Every channel made now (the highest first: one allocation): later, none.
        if (!initChannels()) {
                close();
                return Error::NoMem;
        }
        return Error::Ok;
}

// The GM defaults: piano everywhere, the drums on channel 10, controllers reset.
bool Synth::initChannels() {
        // The highest channel first: all sixteen made at once. Only this can fail (no memory); a
        // font without program 0 just keeps its first preset.
        if (!tsf_channel_set_pitchwheel(_tsf, MidiChannels - 1, 8192)) return false;
        for (int ch = MidiChannels - 1; ch >= 0; --ch) {
                tsf_channel_sounds_off_all(_tsf, ch);
                (void)tsf_channel_set_presetnumber(_tsf, ch, 0, ch == DrumChannel);
                (void)tsf_channel_midi_control(_tsf, ch, 121, 0); // reset all controllers
                (void)tsf_channel_set_pitchwheel(_tsf, ch, 8192);
        }
        return true;
}

void Synth::close() {
        if (_tsf != nullptr) tsf_close(_tsf);
        _tsf = nullptr;
        Memory::free(_events);
        _events = nullptr;
        _head = _count = 0;
        return;
}

Error Synth::queue(const Event &event) {
        if (_tsf == nullptr) return Error::InvalidState;
        if (event.length == 0 || event.length > MaxEventBytes) return Error::Invalid;
        if (_count == QueueEvents) {
                ++_stats.queueFull;
                return Error::Full;
        }
        // In frame order: from the newest back to the first at or before its frame.
        size_t at = _count;
        while (at > 0 && _events[(_head + at - 1) % QueueEvents].frame > event.frame) {
                _events[(_head + at) % QueueEvents] = _events[(_head + at - 1) % QueueEvents];
                --at;
        }
        _events[(_head + at) % QueueEvents] = event;
        ++_count;
        return Error::Ok;
}

void Synth::apply(const Event &e) {
        const uint8_t status = e.data[0];
        const int     ch = status & 0x0f;
        const int     d1 = e.length > 1 ? e.data[1] & 0x7f : 0;
        const int     d2 = e.length > 2 ? e.data[2] & 0x7f : 0;
        if (e.length == sizeof(GmSystemOn) && std::memcmp(e.data, GmSystemOn, sizeof(GmSystemOn)) == 0) {
                (void)initChannels();
                return;
        }
        switch (status & 0xf0) {
                case 0x80:
                        if (e.length < 3) break;
                        tsf_channel_note_off(_tsf, ch, d1);
                        return;
                case 0x90:
                        if (e.length < 3) break;
                        if (d2 == 0) {
                                tsf_channel_note_off(_tsf, ch, d1);
                                return;
                        }
                        if (static_cast<uint32_t>(tsf_active_voice_count(_tsf)) >= _config.maxVoices)
                                ++_stats.notesAtLimit;
                        (void)tsf_channel_note_on(_tsf, ch, d1, static_cast<float>(d2) / 127.0f);
                        return;
                case 0xb0:
                        if (e.length < 3) break;
                        (void)tsf_channel_midi_control(_tsf, ch, d1, d2);
                        return;
                case 0xc0:
                        if (e.length < 2) break;
                        (void)tsf_channel_set_presetnumber(_tsf, ch, d1, ch == DrumChannel);
                        return;
                case 0xe0:
                        if (e.length < 3) break;
                        (void)tsf_channel_set_pitchwheel(_tsf, ch, d1 | d2 << 7);
                        return;
                case 0xa0: // key pressure, channel pressure: not in TinySoundFont
                case 0xd0: return;
                default: break;
        }
        if (status >= 0xf8) return; // real-time
        ++_stats.ignored;
        return;
}

void Synth::render(float *out, size_t frames) {
        if (_tsf == nullptr) {
                std::memset(out, 0, frames * _config.channels * sizeof(float));
                return;
        }
        size_t done = 0;
        while (done < frames) {
                // What is due by this frame, then up to the next event's frame.
                while (_count > 0 && _events[_head].frame <= _frames + done) {
                        apply(_events[_head]);
                        _head = (_head + 1) % QueueEvents;
                        --_count;
                }
                size_t until = frames;
                if (_count > 0 && _events[_head].frame - _frames < frames)
                        until = static_cast<size_t>(_events[_head].frame - _frames);
                tsf_render_float(_tsf, out + done * _config.channels, static_cast<int>(until - done), 0);
                done = until;
        }
        _frames += frames;
        const auto voices = static_cast<uint32_t>(tsf_active_voice_count(_tsf));
        if (voices > _stats.peakVoices) _stats.peakVoices = voices;
        return;
}

void Synth::reset() {
        _head = _count = 0;
        if (_tsf != nullptr) (void)initChannels();
        return;
}

Synth::Stats Synth::stats() const {
        Stats s = _stats;
        s.frames = _frames;
        s.voices = _tsf != nullptr ? static_cast<uint32_t>(tsf_active_voice_count(_tsf)) : 0;
        s.maxVoices = _config.maxVoices;
        s.queued = static_cast<uint32_t>(_count);
        return s;
}

BMDS_NAMESPACE_END
