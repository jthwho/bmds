/**
 * @file      wavetable.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The built-in synth as a port always there (devplan/audio.md §25). A control
 * thread waits for the first event (or a setting); playing, an audio thread is
 * a real-time client as the `synth` command's is: the events that arrived in
 * the last period are placed at their offsets in this one, a period late.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <bmds/audiodevice.h>
#include <bmds/audiofile.h>
#include <bmds/clock.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midiqueue.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/synthfile.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>
#include <bmds/wavetable.h>

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(synthWavetable, "synth/wavetable", Config::Type::Bool, "true",
                     "The built-in synth as the MIDI port wavetable, always there (midi/connect takes system to it)", 0,
                     0, nullptr, nullptr)

        constexpr size_t   RingEvents = 256;
        constexpr uint32_t PeriodFrames = 128;     // as the synth command's: room for a chord's note-ons
        constexpr int64_t  IdleNs = 5'000'000'000; // no events, no voices: closed
        constexpr unsigned RetryMs = 5'000;        // a font not there yet (the card mounting)
        constexpr unsigned PreloadTries = 6;

        // The port's side: what the router delivers waits in the ring for the audio thread.
        class Port final : public MidiSink {
                public:
                        Delivery deliver(const MidiEvent &event) override {
                                if (ring.push(event)) return Delivery::TakenKick;
                                dropped.fetch_add(1, std::memory_order_relaxed);
                                return Delivery::Dropped;
                        }

                        void kick() override; // the control thread woken while not playing

                        MidiQueue             ring; // the router's lock pushes; the audio thread pops
                        std::atomic<uint32_t> dropped{0};
        };

        struct State {
                        Port                  port;
                        MidiEvent            *storage = nullptr;          // the ring's, PSRAM
                        std::atomic<uint8_t>  number{MidiRouter::NoPort}; // the control thread sets it
                        Semaphore             wake{1, 0};                 // an event while idle, a setting changed
                        SysConf::Watcher      watcher;
                        std::atomic<bool>     changed{true};
                        std::atomic<bool>     playing{false};
                        std::atomic<bool>     stop{false}; // the audio thread ends (a setting)
                        Thread                control;
                        std::atomic<uint32_t> events{0};
                        std::atomic<uint32_t> starts{0};
                        std::atomic<uint32_t> late{0};
                        std::atomic<uint32_t> peakVoices{0};
                        uint32_t              lateBase = 0;              // the stream's count when this run began
                        String                warmFont{MemSpace::Psram}; // the font last loaded early
        };

        constinit StaticObject<State> state;

        void Port::kick() {
                if (!state->playing.load(std::memory_order_acquire)) state->wake.release();
                return;
        }

        const Vfs::Context Root{"/", nullptr};

        bool enabled() {
                auto [on, err] = SysConf::getBool(bmdsConf_synthWavetable.key);
                return err.isError() || on;
        }

        bool keepFonts() {
                auto [on, err] = SysConf::getBool(StringView("synth/keep-fonts"));
                return err.isError() || on;
        }

        // ---- Playing ---------------------------------------------------------------------------

        struct Run {
                        AudioFile  *audio = nullptr;
                        SynthFile  *synth = nullptr;
                        FileHandle *synthHandle = nullptr;
                        int64_t     lastReturnNs = 0;
                        int64_t     quietNs = 0; // since an event or a voice
        };

        // The events that arrived since the last period, at their offsets in this one; the period.
        void playPeriod(Run &run, const RtStream::Period &p, uint64_t synthFrame) {
                const int64_t now = Clock::monotonicNs();
                const int64_t periodNs = static_cast<int64_t>(p.frames) * 1000000000 / p.rateHz;
                const int64_t windowStart = run.lastReturnNs != 0 ? run.lastReturnNs : now - periodNs;
                run.lastReturnNs = now;
                MidiEvent m;
                bool      any = false;
                while (state->port.ring.pop(m)) {
                        if (m.size == 0 || m.size > Synth::MaxEventBytes) continue;
                        int64_t offset = (m.timeNs - windowStart) * p.rateHz / 1000000000;
                        if (offset < 0) offset = 0;
                        if (offset >= static_cast<int64_t>(p.frames)) offset = p.frames - 1;
                        Synth::Event e;
                        e.frame = synthFrame + static_cast<uint64_t>(offset);
                        e.length = m.size;
                        std::memcpy(e.data, m.data, m.size);
                        (void)run.synthHandle->write(ByteSpan(reinterpret_cast<const uint8_t *>(&e), sizeof(e)),
                                                     WaitForever);
                        state->events.fetch_add(1, std::memory_order_relaxed);
                        any = true;
                }
                (void)run.synthHandle->read(
                        WritableByteSpan(reinterpret_cast<uint8_t *>(p.out), p.frames * p.channels * sizeof(float)),
                        WaitForever);
                const SynthFile::Info info = run.synth->info();
                if (info.synth.peakVoices > state->peakVoices.load(std::memory_order_relaxed))
                        state->peakVoices.store(info.synth.peakVoices, std::memory_order_relaxed);
                run.quietNs = any || info.synth.voices != 0 ? 0 : run.quietNs + periodNs;
                return;
        }

        void audioThread(void *arg) {
                Run             &run = *static_cast<Run *>(arg);
                RtStream::Period p;
                uint64_t         synthFrame = 0;
                while (!state->stop.load(std::memory_order_acquire) && run.quietNs < IdleNs) {
                        const Error err = run.audio->cycle(&p);
                        if (err == Error::Interrupted || err == Error::BrokenPipe) continue; // rejoins
                        if (err.isError()) {
                                bmdsWarn("wavetable: the stream: %s", err.desc());
                                break;
                        }
                        playPeriod(run, p, synthFrame);
                        synthFrame += p.frames;
                        state->late.store(state->lateBase + p.late, std::memory_order_relaxed);
                }
                (void)run.audio->cycle(nullptr); // ends the last period
                AudioDevice::forgetCaller();     // before this thread's task goes
                return;
        }

        // From the first event until quiet: the stream and the synth, then closed.
        void play() {
                auto [dsp, derr] = Vfs::open("/dev/dsp", Vfs::Write, Root);
                AudioFile *audio = derr.isOk() ? dsp->stream().audioFile() : nullptr;
                if (audio == nullptr) {
                        bmdsWarn("wavetable: /dev/dsp: %s", derr.isError() ? derr.desc() : "not an audio device");
                        state->port.ring.clear();
                        return;
                }
                AudioFile::Config ac;
                ac.format = SampleFormat::F32Le;
                ac.channels = 2;
                ac.rateHz = 0;
                ac.wakeFrames = PeriodFrames;
                ac.flags = AudioFile::Realtime;
                Error err = audio->configure(ac);
                if (err.isError()) {
                        bmdsWarn("wavetable: real-time audio: %s", err.desc());
                        state->port.ring.clear();
                        return;
                }
                SynthFile::Config sc;
                sc.rateHz = audio->config().rateHz;
                sc.channels = 2;
                sc.format = SampleFormat::F32Le;
                auto [synthHandle, serr] = SynthFile::open(sc, Root);
                if (serr.isError()) {
                        bmdsWarn("wavetable: the font: %s",
                                 serr == Error::Invalid ? "damaged, or not a SoundFont" : serr.desc());
                        state->port.ring.clear();
                        return;
                }
                Run run;
                run.audio = audio;
                run.synthHandle = synthHandle.get();
                run.synth = synthHandle->stream().synthFile();
                state->lateBase = state->late.load(std::memory_order_relaxed);
                state->starts.fetch_add(1, std::memory_order_relaxed);
                Thread         thread;
                Thread::Config tc;
                tc.name = "wavetable rt";
                tc.stackBytes = 4096;
                tc.stackSpace = MemSpace::Internal; // a real-time client's (it runs while flash is busy)
                tc.priority = 2;
                tc.kernel = true;
                err = thread.start(tc, audioThread, &run);
                if (err.isError()) {
                        bmdsWarn("wavetable: no audio thread: %s", err.desc());
                        state->port.ring.clear();
                        return;
                }
                thread.join();
                return; // the handles close: the synth, then the stream
        }

        // ---- The control thread ----------------------------------------------------------------

        // The font loaded and left in the cache (synth/keep-fonts), so the first note doesn't wait.
        // False if it isn't there yet (the card not mounted): tried again later.
        bool warm() {
                String font(MemSpace::Psram);
                if (SoundFonts::defaultFont(font).isError() || font.view() == state->warmFont.view()) return true;
                SynthFile::Config sc;
                auto [handle, err] = SynthFile::open(sc, Root);
                if (err.isError()) return err != Error::NotExist && err != Error::NotReady;
                (void)state->warmFont.assign(font.view());
                return true;
        }

        // The port as synth/wavetable says.
        void applySetting() {
                const bool on = enabled();
                if (on && state->number == MidiRouter::NoPort) {
                        state->port.ring.clear();
                        auto [n, err] = Midi::router().addPort(MidiRouter::Kind::System, MidiRouter::Destination,
                                                               "wavetable", &state->port);
                        if (err.isError()) {
                                bmdsWarn("wavetable: no MIDI port: %s", err.desc());
                                return;
                        }
                        state->number.store(n);
                        Midi::applySettings(); // midi/connect: system>wavetable
                } else if (!on && state->number != MidiRouter::NoPort) {
                        Midi::router().removePort(state->number.load()); // nothing more delivered
                        state->number.store(MidiRouter::NoPort);
                        state->port.ring.clear();
                }
                return;
        }

        void controlMain(void *) {
                unsigned tries = 0;
                for (;;) {
                        if (state->changed.exchange(false)) {
                                SysConf::Id id;
                                while (state->watcher.next(id)) {}
                                (void)state->watcher.overflowed();
                                applySetting();
                                tries = 0;
                        }
                        bool warmed = true;
                        if (state->number != MidiRouter::NoPort && keepFonts() && tries < PreloadTries) {
                                warmed = warm();
                                tries = warmed ? PreloadTries : tries + 1;
                        }
                        if (state->number != MidiRouter::NoPort && !state->port.ring.isEmpty()) {
                                state->stop.store(false, std::memory_order_release);
                                state->playing.store(true, std::memory_order_release);
                                play();
                                state->playing.store(false, std::memory_order_release);
                                continue; // events that came as it stopped
                        }
                        (void)state->wake.acquire(!warmed && tries < PreloadTries ? RetryMs : WaitForever);
                }
        }

        void settingsChanged(void *) {
                state->changed.store(true);
                state->stop.store(true, std::memory_order_release); // playing: closes, then looks again
                state->wake.release();
                return;
        }

} // namespace

Error Wavetable::init() {
        state.create();
        state->storage = static_cast<MidiEvent *>(Memory::alloc(MemSpace::Psram, RingEvents * sizeof(MidiEvent)));
        if (state->storage == nullptr) return Error::NoMem;
        (void)state->port.ring.setStorage(Span<MidiEvent>(state->storage, RingEvents));
        Error err = state->watcher.start(StringView("synth"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        Thread::Config tc;
        tc.name = "wavetable";
        tc.stackBytes = 4096;
        tc.stackSpace = MemSpace::Psram;
        tc.priority = 2;
        tc.kernel = true;
        return state->control.start(tc, controlMain, nullptr); // for good: never joined
}

Wavetable::Status Wavetable::status() {
        Status s;
        if (!state.isCreated()) return s;
        s.port = state->number.load();
        s.on = s.port != MidiRouter::NoPort;
        s.playing = state->playing.load();
        s.events = state->events.load();
        s.dropped = state->port.dropped.load();
        s.starts = state->starts.load();
        s.late = state->late.load();
        s.peakVoices = state->peakVoices.load();
        return s;
}

BMDS_NAMESPACE_END
