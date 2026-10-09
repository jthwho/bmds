/**
 * @file      mididecoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Standard MIDI Files as a decoder, played by a synth (devplan/audio.md §22).
 */

#include <cstring>
#include <new>
#include <tml.h>
#include <atomic>
#include <bmds/memory.h>
#include <bmds/mididecoder.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t RateHz = 48000;
        constexpr uint64_t TailFrames = 2 * RateHz;        // the last notes' release
        constexpr uint64_t MaxFileBytes = 4 * 1024 * 1024; // read whole: MIDI files are small

        // The file in memory, read by tml_load() (tml_load_memory() calls its reader through a
        // pointer of another type: undefined, and reported by the sanitizers).
        struct MemoryStream {
                        const uint8_t *data;
                        size_t         size;
                        size_t         at;
        };

        int readMemory(void *arg, void *out, unsigned int bytes) {
                auto *m = static_cast<MemoryStream *>(arg);
                if (bytes > m->size - m->at) bytes = static_cast<unsigned int>(m->size - m->at);
                if (out != nullptr) std::memcpy(out, m->data + m->at, bytes); // null: a skip
                m->at += bytes;
                return static_cast<int>(bytes);
        }

        constinit std::atomic<MidiDecoder::OpenSynth>  openSynth{nullptr};
        constinit std::atomic<MidiDecoder::CloseSynth> closeSynth{nullptr};

        class MidiFileDecoder final : public Decoder {
                public:
                        explicit MidiFileDecoder(const Source &source) : Decoder(source) {}

                        Error start(SoundFont *font, tml_message *messages, uint64_t frames) {
                                _font = font;
                                _messages = messages;
                                _next = messages;
                                _info.codec = Codec::Midi;
                                _info.format = SampleFormat::F32Le;
                                _info.channels = 2;
                                _info.rateHz = RateHz;
                                _info.frames = frames;
                                return finishOpen();
                        }

                protected:
                        ~MidiFileDecoder() override {
                                if (_font != nullptr) closeSynth.load(std::memory_order_acquire)(_synth, _font);
                                if (_messages != nullptr) tml_free(_messages);
                        }

                        Result<size_t> decodeFrames(uint8_t *out, size_t frames) override {
                                // The end is EndOfFile, never nothing without an error: a reader would spin.
                                if (_position >= _info.frames) return Result<size_t>(0, Error::EndOfFile);
                                if (frames > _info.frames - _position)
                                        frames = static_cast<size_t>(_info.frames - _position);
                                auto  *samples = reinterpret_cast<float *>(out);
                                size_t done = 0;
                                while (done < frames) {
                                        // The file's messages up to this chunk's end, as many as the queue takes.
                                        const uint64_t end = _position + frames;
                                        while (_next != nullptr && frameOf(*_next) < end) {
                                                Synth::Event e;
                                                e.frame = _base + frameOf(*_next);
                                                if (toEvent(*_next, e) && _synth.queue(e) == Error::Full) break;
                                                _next = _next->next;
                                        }
                                        // Up to the first message not queued (the queue was full), or the end.
                                        size_t n = frames - done;
                                        if (_next != nullptr && frameOf(*_next) < end) {
                                                const uint64_t until = frameOf(*_next);
                                                const uint64_t at = _position + done;
                                                n = until > at ? static_cast<size_t>(until - at) : 1;
                                                if (n > frames - done) n = frames - done;
                                        }
                                        _synth.render(samples + done * 2, n);
                                        done += n;
                                }
                                return makeResult(frames);
                        }

                        // Back to the start; the controllers and programs replayed up to @p frame, its
                        // notes not.
                        Error seekFrame(uint64_t frame) override {
                                _synth.reset();
                                _base = _synth.stats().frames - frame;
                                for (_next = _messages; _next != nullptr && frameOf(*_next) < frame;
                                     _next = _next->next) {
                                        if (_next->type == TML_NOTE_ON || _next->type == TML_NOTE_OFF) continue;
                                        Synth::Event e;
                                        e.frame = 0; // at once
                                        if (toEvent(*_next, e)) (void)_synth.queue(e);
                                }
                                return Error::Ok;
                        }

                public:
                        Synth _synth;

                private:
                        static uint64_t frameOf(const tml_message &m) {
                                return static_cast<uint64_t>(m.time) * RateHz / 1000;
                        }

                        // A message's bytes (false: not one the synth takes, as a tempo change).
                        static bool toEvent(const tml_message &m, Synth::Event &e) {
                                const auto status = static_cast<uint8_t>(m.type | (m.channel & 0x0f));
                                switch (m.type) {
                                        case TML_NOTE_OFF:
                                        case TML_NOTE_ON:
                                        case TML_KEY_PRESSURE:
                                        case TML_CONTROL_CHANGE:
                                                e.length = 3;
                                                e.data[0] = status;
                                                e.data[1] = static_cast<uint8_t>(m.key) & 0x7f;
                                                e.data[2] = static_cast<uint8_t>(m.velocity) & 0x7f;
                                                return true;
                                        case TML_PROGRAM_CHANGE:
                                        case TML_CHANNEL_PRESSURE:
                                                e.length = 2;
                                                e.data[0] = status;
                                                e.data[1] = static_cast<uint8_t>(m.program) & 0x7f;
                                                return true;
                                        case TML_PITCH_BEND:
                                                e.length = 3;
                                                e.data[0] = status;
                                                e.data[1] = static_cast<uint8_t>(m.pitch_bend & 0x7f);
                                                e.data[2] = static_cast<uint8_t>((m.pitch_bend >> 7) & 0x7f);
                                                return true;
                                        default: return false;
                                }
                        }

                        SoundFont   *_font = nullptr;
                        tml_message *_messages = nullptr;
                        tml_message *_next = nullptr;
                        uint64_t     _base = 0; // the synth's frame at this file's frame 0
        };

} // namespace

void MidiDecoder::setFonts(OpenSynth open, CloseSynth close) {
        closeSynth.store(close, std::memory_order_release);
        openSynth.store(open, std::memory_order_release);
        return;
}

Result<Decoder *> MidiDecoder::open(const Decoder::Source &source) {
        if (source.size < 14 || source.size > MaxFileBytes) return makeError<Decoder *>(Error::Invalid);
        // The whole file, parsed into timed messages.
        auto *data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, static_cast<size_t>(source.size)));
        if (data == nullptr) return makeError<Decoder *>(Error::NoMem);
        Error        err = source.read(source.arg, 0, WritableByteSpan(data, static_cast<size_t>(source.size)));
        MemoryStream memory{data, static_cast<size_t>(source.size), 0};
        tml_stream   stream{&memory, readMemory};
        tml_message *messages = err.isOk() ? tml_load(&stream) : nullptr;
        Memory::free(data);
        if (err.isError()) return makeError<Decoder *>(err);
        if (messages == nullptr) return makeError<Decoder *>(Error::Invalid);
        unsigned int lengthMs = 0;
        (void)tml_get_info(messages, nullptr, nullptr, nullptr, nullptr, &lengthMs);

        void *mem = Memory::alloc(MemSpace::Psram, sizeof(MidiFileDecoder));
        if (mem == nullptr) {
                tml_free(messages);
                return makeError<Decoder *>(Error::NoMem);
        }
        auto         *d = ::new (mem) MidiFileDecoder(source);
        Synth::Config c;
        c.rateHz = RateHz;
        c.channels = 2;
        const OpenSynth opener = openSynth.load(std::memory_order_acquire);
        SoundFont      *sf = nullptr;
        if (opener == nullptr) {
                err = Error::NotSupported;
        } else {
                auto [f, ferr] = opener(c, d->_synth);
                err = ferr;
                sf = f;
        }
        if (err.isOk())
                err = d->start(sf, messages, static_cast<uint64_t>(lengthMs) * RateHz / 1000 + TailFrames);
        else
                tml_free(messages);
        if (err.isError()) {
                Decoder::close(d); // frees the synth and the messages it holds
                return makeError<Decoder *>(err);
        }
        return makeResult(static_cast<Decoder *>(d));
}

BMDS_NAMESPACE_END
