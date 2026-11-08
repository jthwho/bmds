/**
 * @file      synthservice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The built-in synths as ports always there (devplan/audio.md §25, §26): one service, run as
 * `fm` and as `wavetable`. Each has a control thread waiting for the first event (or a setting);
 * playing, an audio thread is a real-time client as the `synth` command's is: the events that
 * arrived in the last period are placed at their offsets in this one, a period late. While an fd
 * allows it latency (BMDS_MIDI_ALLOW_LATENCY, §27), the thread renders ahead instead, into a PCM
 * stream's ring, each event placed at its time plus that latency.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>
#include <bmds/audiodevice.h>
#include <bmds/audiofile.h>
#include <bmds/clock.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midiqueue.h>
#include <bmds/notifications.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/synthfile.h>
#include <bmds/synthservice.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(synthWavetable, "synth/wavetable", Config::Type::Bool, "true",
                     "The wavetable synth as the MIDI port wavetable, always there", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(synthFm, "synth/fm", Config::Type::Bool, "true", "The FM synth as the MIDI port fm, always there",
                     0, 0, nullptr, nullptr)

        // A file's start comes at once (each channel's program and controllers, a player's resets)
        // while the synth and its stream open: 256 dropped 61 of a Freedoom song's (2026-10-27).
        constexpr size_t   RingEvents = 1024;
        constexpr int64_t  IdleNs = 5'000'000'000; // no events, no voices: closed
        constexpr unsigned RetryMs = 5'000;        // a font not there yet (the card mounting)
        constexpr unsigned PreloadTries = 6;
        constexpr int64_t  NoticeNs = 200'000'000; // an open slower than this was a load: said
        // The control thread loads a font (SynthFile, SynthEngines, SoundFonts, TinySoundFont):
        // 4 KB overflowed when a load went through the engines (found on the board, 2026-10-27).
        constexpr size_t ControlStackBytes = 6144;

        // Rendering ahead (devplan/audio.md §27): the latency allowed, the ring it buys.
        constexpr size_t  MaxAllowances = 8;        // fds allowing a synth latency at once
        constexpr int64_t MinAheadNs = 40'000'000;  // below: real time
        constexpr int64_t MaxAheadNs = 250'000'000; // more allowed: this
        // Between an event's arrival and its period's placing: a render, and the presenter (above the
        // thread, ahead) holding it off for most of a frame. 2 ms was too little beside demo1 (2026-11-07).
        constexpr int64_t  AheadMarginNs = 15'000'000;
        constexpr uint32_t MinRingFrames = 256;   // a ring's least (a power of two: its rounding)
        constexpr uint32_t MaxRingFrames = 16384; //   and most
        // The next frame's heard time, ahead: kept a period on each period and eased towards what the
        // stream's position says (its time is when the mixer took frames: a few milliseconds of
        // jitter, which placed events a period late, then counted them late); taken as measured
        // after an underrun (the ring ran dry: later by the gap), or past this.
        constexpr int64_t ReanchorNs = 20'000'000;
        constexpr int64_t EaseShift = 5; // a 32nd of the difference a period
        // Ahead, the thread's priority: below the presenter (4) while its ring holds a quarter or
        // more; under that, above the presenter and the window manager (5) until it holds half, so
        // drawing yields only when the music would break (demo1's tunnel at 60 fps, 2026-11-07).
        constexpr unsigned AudioPriority = 3;
        constexpr unsigned BehindPriority = 6;
        constexpr uint32_t DefaultPeriodFrames = 240; // the device's, if it doesn't say

        // What a stream's counter @p now adds since @p seen (a stream reopened counts from 0 again).
        uint32_t newCount(uint32_t now, uint32_t &seen) {
                const uint32_t added = now >= seen ? now - seen : now;
                seen = now;
                return added;
        }

        // The ring for latency @p latencyNs: the frames written and not heard are at most the ring,
        // a period being written and the three after the mixer (one waiting to be taken, two DMA
        // buffers), so an event sent at its time is never late.
        uint32_t aheadRingFrames(int64_t latencyNs, uint32_t rateHz, uint32_t periodFrames) {
                const int64_t periodNs = static_cast<int64_t>(periodFrames) * 1'000'000'000 / rateHz;
                const int64_t room = latencyNs - 4 * periodNs - AheadMarginNs;
                uint32_t      frames = MinRingFrames;
                while (frames * 2 <= MaxRingFrames && static_cast<int64_t>(frames) * 2 * 1'000'000'000 / rateHz <= room)
                        frames *= 2;
                return frames;
        }

        class Service;

        // The port's side: what the router delivers waits in the ring for the audio thread.
        class Port final : public MidiSink {
                public:
                        Delivery deliver(const MidiEvent &event) override {
                                if (ring.push(event)) return Delivery::TakenKick;
                                dropped.fetch_add(1, std::memory_order_relaxed);
                                return Delivery::Dropped;
                        }

                        void kick() override; // the control thread woken while not playing

                        int64_t latencyNs() const override; // the service's, measured each period

                        void allowLatency(const void *owner, int64_t ns) override; // §27
                        void cancel(const void *owner) override;                   // §27

                        Service              *service = nullptr;
                        MidiQueue             ring; // the router's lock pushes; the audio thread pops
                        std::atomic<uint32_t> dropped{0};
        };

        const Vfs::Context Root{"/", nullptr};

        bool flag(StringView key) {
                auto [on, err] = SysConf::getBool(key);
                return err.isError() || on;
        }

        // A built-in synth: its port, ring, control thread and, while playing, its stream.
        class Service {
                public:
                        Service(SynthEngine::Kind kind, const char *name, StringView enableKey)
                            : _kind(kind), _name(name), _enableKey(enableKey) {
                                port.service = this;
                        }

                        Error start(); // the boot's

                        SynthServices::Status status() const;
                        SynthEngine::Kind     kind() const { return _kind; }
                        const char           *name() const { return _name; }

                        void kicked() {
                                if (!_playing.load(std::memory_order_acquire)) _wake.release();
                                return;
                        }

                        void allow(const void *owner, int64_t ns); // under the router's lock

                        // @p owner cancelled: holding an allowance, its sound ahead is dropped (the
                        // audio thread's next period). Under the router's lock.
                        void cancelled(const void *owner) {
                                if (_aheadNs.load(std::memory_order_relaxed) == 0) return;
                                for (const Allowance &a : _allowances)
                                        if (a.owner == owner) _cancel.store(true, std::memory_order_release);
                                return;
                        }

                        // With no port (the router calls allow() no more): every allowance gone.
                        void clearAllowances() {
                                for (Allowance &a : _allowances) a = Allowance();
                                _aheadNs.store(0, std::memory_order_release);
                                return;
                        }

                        void settingsChanged() {
                                _changed.store(true);
                                _stop.store(true, std::memory_order_release); // playing: closes, then looks again
                                _wake.release();
                                return;
                        }

                        Port port;

                private:
                        struct Run {
                                        AudioFile  *audio = nullptr;
                                        SynthFile  *synth = nullptr;
                                        FileHandle *synthHandle = nullptr;
                                        uint32_t    rateHz = 0;
                                        uint64_t    synthFrame = 0;     // the frames rendered
                                        uint64_t    lastEventFrame = 0; // ahead: events kept in order
                                        int64_t     lastReturnNs = 0;
                                        int64_t     quietNs = 0; // since an event or a voice
                                        uint32_t    late = 0;    // late periods, underruns, late events
                                        uint32_t    lateSeen = 0;      // the real-time stream's count
                                        uint32_t    underrunsSeen = 0; // the PCM stream's
                                        int64_t     heardNs = 0;    // ahead: when the next frame is heard (0: unknown)
                                        bool        behind = false; // ahead: raised (BehindPriority)
                                        bool        failed = false;
                        };

                        // Allowances of latency (§27), under the router's lock.
                        struct Allowance {
                                        const void *owner = nullptr;
                                        int64_t     ns = 0;
                        };

                        static void controlEntry(void *arg) { static_cast<Service *>(arg)->controlMain(); }
                        static void audioEntry(void *arg);

                        bool running(const Run &run) const {
                                return !_stop.load(std::memory_order_acquire) && run.quietNs < IdleNs;
                        }
                        void  playPeriod(Run &run, const RtStream::Period &p);
                        Error playRealtime(Run &run);
                        Error playAhead(Run &run, int64_t latencyNs);
                        bool  placeAhead(Run &run, int64_t heardNs, int64_t latencyNs, uint32_t frames);
                        void  rendered(Run &run, bool any, int64_t periodNs);
                        void  dropAhead(Run &run);
                        void  audioMain(Run &run);
                        void play();
                        bool warm();
                        void applySetting();
                        void controlMain();
                        void tellLoaded(StringView path, int64_t ns);

                        SynthEngine::Kind     _kind;
                        const char           *_name;
                        StringView            _enableKey;
                        MidiEvent            *_storage = nullptr;          // the ring's, PSRAM
                        std::atomic<uint8_t>  _number{MidiRouter::NoPort}; // the control thread sets it
                        Semaphore             _wake{1, 0};                 // an event while idle, a setting changed
                        SysConf::Watcher      _watcher;
                        std::atomic<bool>     _changed{true};
                        std::atomic<bool>     _playing{false};
                        std::atomic<bool>     _stop{false}; // the audio thread ends (a setting)
                        Thread                _control;
                        std::atomic<uint32_t> _events{0};
                        std::atomic<uint32_t> _starts{0};
                        std::atomic<uint32_t> _late{0};
                        std::atomic<uint32_t> _peakVoices{0};
                        std::atomic<uint32_t> _raised{0};    // ahead: times behind, raised
                        std::atomic<int32_t>  _latencyUs{0}; // an event's time to its sound: the last period's
                        std::atomic<int64_t>  _aheadNs{0};   // the latency allowed (ahead); 0: real time
                        std::atomic<bool>     _cancel{false}; // ahead: an owner cancelled (cancelled())
                        std::atomic<uint32_t> _cancels{0};    // ahead: sound dropped for a cancel
                        Allowance             _allowances[MaxAllowances];

                public:
                        int64_t latencyNs() const;

                private:
                        uint32_t              _lateBase = 0;              // the stream's count when this run began
                        String                _warmFont{MemSpace::Psram}; // the font last loaded early
                        uint32_t              _notice = 0;                // the load notification's id, replaced
        };

        struct Services {
                        Service fm{SynthEngine::Kind::Fm, "fm", bmdsConf_synthFm.key};
                        Service wavetable{SynthEngine::Kind::Wavetable, "wavetable", bmdsConf_synthWavetable.key};
        };

        constinit StaticObject<Services> services;

        void Port::kick() {
                service->kicked();
                return;
        }

        int64_t Port::latencyNs() const {
                return service->latencyNs();
        }

        void Port::allowLatency(const void *owner, int64_t ns) {
                service->allow(owner, ns);
                return;
        }

        void Port::cancel(const void *owner) {
                service->cancelled(owner);
                return;
        }

        // Ahead, the latency allowed (a promise, kept by placing each event at it). Real time: until
        // a period has measured it (the synth idle), an estimate, three of the device's usual 5 ms
        // periods (devplan/sequencer.md §7).
        int64_t Service::latencyNs() const {
                const int64_t ahead = _aheadNs.load(std::memory_order_relaxed);
                if (ahead > 0) return ahead;
                const int32_t us = _latencyUs.load(std::memory_order_relaxed);
                return static_cast<int64_t>(us > 0 ? us : 15000) * 1000;
        }

        // @p owner's allowance set (0: taken back); the latency is the least allowed, real time
        // under MinAheadNs. One past MaxAllowances is not kept (that fd hears the others').
        void Service::allow(const void *owner, int64_t ns) {
                Allowance *mine = nullptr;
                Allowance *free = nullptr;
                for (Allowance &a : _allowances) {
                        if (a.owner == owner)
                                mine = &a;
                        else if (a.owner == nullptr && free == nullptr)
                                free = &a;
                }
                if (ns <= 0) {
                        if (mine != nullptr) *mine = Allowance();
                } else if (mine != nullptr) {
                        mine->ns = ns;
                } else if (free != nullptr) {
                        *free = Allowance{owner, ns};
                }
                int64_t least = 0;
                for (const Allowance &a : _allowances)
                        if (a.owner != nullptr && (least == 0 || a.ns < least)) least = a.ns;
                if (least > MaxAheadNs) least = MaxAheadNs;
                if (least < MinAheadNs) least = 0;
                _aheadNs.store(least, std::memory_order_release);
                return;
        }

        void watched(void *arg) {
                static_cast<Service *>(arg)->settingsChanged();
                return;
        }

        // Is @p name the system synth (synth/system)?
        bool isSystem(const char *name) {
                String system;
                if (SysConf::get(StringView("synth/system"), system).isError()) return std::strcmp(name, "fm") == 0;
                return system.view() == StringView(name);
        }

        // ---- Playing -------------------------------------------------------------------------------

        void Service::playPeriod(Run &run, const RtStream::Period &p) {
                const int64_t now = Clock::monotonicNs();
                const int64_t periodNs = static_cast<int64_t>(p.frames) * 1000000000 / p.rateHz;
                const int64_t windowStart = run.lastReturnNs != 0 ? run.lastReturnNs : now - periodNs;
                run.lastReturnNs = now;
                // An event at time T is placed at T − windowStart into this period, which is heard
                // from p.playNs: its latency is p.playNs − windowStart.
                if (p.playNs > windowStart)
                        _latencyUs.store(static_cast<int32_t>((p.playNs - windowStart) / 1000),
                                         std::memory_order_relaxed);
                MidiEvent m;
                bool      any = false;
                while (port.ring.pop(m)) {
                        if (m.size == 0 || m.size > SynthEngine::MaxEventBytes) continue;
                        int64_t offset = (m.timeNs - windowStart) * p.rateHz / 1000000000;
                        if (offset < 0) offset = 0;
                        if (offset >= static_cast<int64_t>(p.frames)) offset = p.frames - 1;
                        SynthEngine::Event e;
                        e.frame = run.synthFrame + static_cast<uint64_t>(offset);
                        e.length = m.size;
                        std::memcpy(e.data, m.data, m.size);
                        (void)run.synthHandle->write(ByteSpan(reinterpret_cast<const uint8_t *>(&e), sizeof(e)),
                                                     WaitForever);
                        _events.fetch_add(1, std::memory_order_relaxed);
                        any = true;
                }
                (void)run.synthHandle->read(
                        WritableByteSpan(reinterpret_cast<uint8_t *>(p.out), p.frames * p.channels * sizeof(float)),
                        WaitForever);
                rendered(run, any, periodNs);
                return;
        }

        // After a period's render: its frames counted, the voices, quiet or not.
        void Service::rendered(Run &run, bool any, int64_t periodNs) {
                const SynthFile::Info info = run.synth->info();
                if (info.synth.peakVoices > _peakVoices.load(std::memory_order_relaxed))
                        _peakVoices.store(info.synth.peakVoices, std::memory_order_relaxed);
                run.quietNs = any || info.synth.voices != 0 || !port.ring.isEmpty() ? 0 : run.quietNs + periodNs;
                _late.store(_lateBase + run.late, std::memory_order_relaxed);
                return;
        }

        void Service::audioEntry(void *arg) {
                // On the display's core (the loader's DisplayCore), away from programs that draw
                // (core 0), and pinned before its first float would pin it wherever it happened to
                // be: frame rates beside the music swung 43-60 with that chance (the user's choice,
                // 2026-11-06; docs/platforms/esp32p4.md §4c).
                (void)Thread::pinCurrent(1);
                auto *run = static_cast<std::pair<Service *, Run *> *>(arg);
                run->first->audioMain(*run->second);
                return;
        }

        // Until quiet (or a setting): real time, or ahead while latency is allowed, the stream
        // changed between periods when the allowance does (the synth stays open).
        void Service::audioMain(Run &run) {
                while (running(run)) {
                        const int64_t ahead = _aheadNs.load(std::memory_order_acquire);
                        const Error   err = ahead > 0 ? playAhead(run, ahead) : playRealtime(run);
                        if (err.isError()) {
                                bmdsWarn("%s: %s audio: %s", _name, ahead > 0 ? "ahead" : "real-time", err.desc());
                                run.failed = true;
                                break;
                        }
                }
                AudioDevice::forgetCaller(); // before this thread's task goes
                return;
        }

        Error Service::playRealtime(Run &run) {
                AudioFile::Config ac;
                ac.format = SampleFormat::F32Le;
                ac.channels = 2;
                ac.rateHz = 0;
                ac.wakeFrames = 0; // the device's period: slack beside a busy game, no client forcing it down
                ac.flags = AudioFile::Realtime;
                Error err = run.audio->configure(ac);
                if (err.isError()) return err;
                RtStream::Period p;
                run.lastReturnNs = 0;
                while (running(run) && _aheadNs.load(std::memory_order_acquire) == 0) {
                        err = run.audio->cycle(&p);
                        if (err == Error::Interrupted || err == Error::BrokenPipe) { // rejoins
                                err = Error::Ok;
                                continue;
                        }
                        if (err.isError()) break;
                        run.late += newCount(p.late, run.lateSeen);
                        playPeriod(run, p);
                        run.synthFrame += p.frames;
                }
                (void)run.audio->cycle(nullptr); // ends the last period
                return err;
        }

        // Ahead (devplan/audio.md §27): a period at a time into the ring, write() waiting for room;
        // each event at its time plus @p latencyNs.
        Error Service::playAhead(Run &run, int64_t latencyNs) {
                run.heardNs = 0;
                _cancel.store(false, std::memory_order_relaxed); // one before this run is moot
                uint32_t period = run.audio->status().periodFrames;
                if (period == 0) period = DefaultPeriodFrames;
                if (period > RtStream::MaxFrames) period = RtStream::MaxFrames;
                AudioFile::Config ac;
                ac.format = SampleFormat::F32Le;
                ac.channels = 2;
                ac.rateHz = run.rateHz;
                ac.ringFrames = aheadRingFrames(latencyNs, run.rateHz, period);
                ac.startFrames = period; // plays at once; the ring fills as the synth outruns it
                Error err = run.audio->configure(ac);
                if (err.isError()) return err;
                const size_t bytes = static_cast<size_t>(period) * 2 * sizeof(float);
                auto        *out = static_cast<float *>(Memory::alloc(MemSpace::Internal, bytes));
                if (out == nullptr) out = static_cast<float *>(Memory::alloc(MemSpace::Psram, bytes));
                if (out == nullptr) return Error::NoMem;
                const int64_t periodNs = static_cast<int64_t>(period) * 1'000'000'000 / run.rateHz;
                while (running(run) && _aheadNs.load(std::memory_order_acquire) == latencyNs) {
                        if (_cancel.exchange(false, std::memory_order_acquire)) dropAhead(run);
                        // When the frame about to be written is heard: after the mixer's next take
                        // (a period after its last, or now if it ran dry), the ring before it, then
                        // two DMA buffers.
                        const AudioFile::Status st = run.audio->status();
                        const int64_t           now = Clock::monotonicNs();
                        int64_t next = st.timestampNs != 0 && st.queuedFrames != 0 ? st.timestampNs + periodNs : now;
                        if (next < now) next = now;
                        const int64_t measured = next +
                                                 static_cast<int64_t>(st.queuedFrames) * 1'000'000'000 / run.rateHz +
                                                 2 * periodNs;
                        const uint32_t underruns = newCount(st.underruns, run.underrunsSeen);
                        const int64_t  off = measured - run.heardNs;
                        if (run.heardNs == 0 || underruns != 0 || off > ReanchorNs || off < -ReanchorNs)
                                run.heardNs = measured;
                        else
                                run.heardNs += off / (int64_t(1) << EaseShift);
                        const int64_t heard = run.heardNs;
                        run.late += underruns;
                        const bool behind =
                                run.behind ? st.queuedFrames < ac.ringFrames / 2 : st.queuedFrames < ac.ringFrames / 4;
                        if (behind != run.behind) {
                                Thread::setCurrentPriority(behind ? BehindPriority : AudioPriority);
                                run.behind = behind;
                                if (behind) _raised.fetch_add(1, std::memory_order_relaxed);
                        }
                        const bool             any = placeAhead(run, heard, latencyNs, period);
                        const WritableByteSpan span(reinterpret_cast<uint8_t *>(out), bytes);
                        (void)run.synthHandle->read(span, WaitForever);
                        run.synthFrame += period;
                        run.heardNs += periodNs;
                        const Result<size_t> r = run.audio->write(ByteSpan(span.data(), span.size()), WaitForever);
                        if (r.error().isError()) {
                                err = r.error();
                                break;
                        }
                        rendered(run, any, periodNs);
                }
                // A cancel just before the allowance went (a program stopping its music, then closing
                // its fd) is acted on still: what was made ahead isn't played out.
                if (_cancel.exchange(false, std::memory_order_acquire) && err.isOk()) dropAhead(run);
                if (run.behind) {
                        Thread::setCurrentPriority(AudioPriority);
                        run.behind = false;
                }
                Memory::free(out);
                return err;
        }

        // An owner cancelled (a stop, a seek): what was rendered and isn't heard yet is thrown away,
        // with the events waiting to be placed (every sender's: they share the sound), and the voices
        // silenced at once (all sound off on every channel), so the next sound is the program's new
        // one, not the old a latency later. The heard time is found again from the empty ring.
        void Service::dropAhead(Run &run) {
                (void)run.audio->drop();
                MidiEvent e;
                while (port.ring.pop(e)) {}
                for (uint8_t channel = 0; channel < 16; ++channel) {
                        SynthEngine::Event se;
                        se.frame = run.synthFrame > run.lastEventFrame ? run.synthFrame : run.lastEventFrame;
                        se.length = 3;
                        se.data[0] = static_cast<uint8_t>(0xb0 | channel);
                        se.data[1] = 120; // all sound off
                        se.data[2] = 0;
                        (void)run.synthHandle->write(ByteSpan(reinterpret_cast<const uint8_t *>(&se), sizeof(se)),
                                                     WaitForever);
                }
                run.heardNs = 0;
                _cancels.fetch_add(1, std::memory_order_relaxed);
                return;
        }

        // The events for the @p frames about to be rendered, the first heard at @p heardNs: each at
        // its time plus @p latencyNs; a later period's waits in the ring (they come in time order).
        bool Service::placeAhead(Run &run, int64_t heardNs, int64_t latencyNs, uint32_t frames) {
                bool any = false;
                for (const MidiEvent *m = port.ring.peek(); m != nullptr; m = port.ring.peek()) {
                        const int64_t time = m->timeNs != 0 ? m->timeNs : Clock::monotonicNs();
                        int64_t       at = (time + latencyNs - heardNs) * run.rateHz / 1'000'000'000;
                        if (at >= static_cast<int64_t>(frames)) break;
                        MidiEvent e;
                        (void)port.ring.pop(e);
                        if (e.size == 0 || e.size > SynthEngine::MaxEventBytes) continue;
                        if (at < 0) {
                                at = 0;
                                ++run.late;
                        }
                        uint64_t frame = run.synthFrame + static_cast<uint64_t>(at);
                        if (frame < run.lastEventFrame) frame = run.lastEventFrame;
                        run.lastEventFrame = frame;
                        SynthEngine::Event se;
                        se.frame = frame;
                        se.length = e.size;
                        std::memcpy(se.data, e.data, e.size);
                        (void)run.synthHandle->write(ByteSpan(reinterpret_cast<const uint8_t *>(&se), sizeof(se)),
                                                     WaitForever);
                        _events.fetch_add(1, std::memory_order_relaxed);
                        any = true;
                }
                return any;
        }

        // A first note that waited for the font to load from the card: said, so the pause has a reason.
        void Service::tellLoaded(StringView path, int64_t ns) {
                Notification n;
                n.replaces = _notice;
                n.urgency = Notification::Urgency::Low;
                n.flags = Notification::Transient;
                std::snprintf(n.app, sizeof(n.app), "Synthesizer");
                std::snprintf(n.icon, sizeof(n.icon), "audio-midi");
                std::snprintf(n.title, sizeof(n.title), "Wavetable synth");
                StringView file = path;
                for (size_t i = path.size(); i > 0; --i) {
                        if (path[i - 1] != '/') continue;
                        file = path.mid(i);
                        break;
                }
                std::snprintf(n.body, sizeof(n.body), "Loaded %.*s (%u.%u s) for its first note.",
                              static_cast<int>(file.size()), file.data(), static_cast<unsigned>(ns / 1000000000),
                              static_cast<unsigned>(ns / 100000000 % 10));
                auto [id, err] = Notifications::post(n, 0, nullptr);
                if (err.isOk()) _notice = id;
                return;
        }

        // From the first event until quiet: the stream and the synth, then closed.
        void Service::play() {
                auto [dsp, derr] = Vfs::open("/dev/dsp", Vfs::Write, Root);
                AudioFile *audio = derr.isOk() ? dsp->stream().audioFile() : nullptr;
                if (audio == nullptr) {
                        bmdsWarn("%s: /dev/dsp: %s", _name, derr.isError() ? derr.desc() : "not an audio device");
                        port.ring.clear();
                        return;
                }
                // At the device's rate; the audio thread configures the stream (real time or ahead).
                SynthFile::Config sc;
                sc.rateHz = audio->status().deviceRate;
                if (sc.rateHz == 0) {
                        bmdsWarn("%s: the audio device has no rate", _name);
                        port.ring.clear();
                        return;
                }
                sc.channels = 2;
                sc.format = SampleFormat::F32Le;
                sc.engine = _kind;
                const int64_t opening = Clock::monotonicNs();
                auto [synthHandle, serr] = SynthFile::open(sc, Root);
                if (serr.isError()) {
                        bmdsWarn("%s: the instruments: %s", _name,
                                 serr == Error::Invalid ? "damaged, or not a SoundFont or OP2 bank" : serr.desc());
                        port.ring.clear();
                        return;
                }
                SynthFile    *synth = synthHandle->stream().synthFile();
                const int64_t took = Clock::monotonicNs() - opening;
                if (_kind == SynthEngine::Kind::Wavetable && took >= NoticeNs) tellLoaded(synth->fontPath(), took);
                Run run;
                run.audio = audio;
                run.synthHandle = synthHandle.get();
                run.synth = synth;
                run.rateHz = sc.rateHz;
                _lateBase = _late.load(std::memory_order_relaxed);
                _starts.fetch_add(1, std::memory_order_relaxed);
                std::pair<Service *, Run *> arg{this, &run};
                char                        name[16];
                std::snprintf(name, sizeof(name), "%s rt", _name);
                Thread         thread;
                Thread::Config tc;
                tc.name = name;
                tc.stackBytes = 4096;
                tc.stackSpace = MemSpace::Internal; // a real-time client's (it runs while flash is busy)
                tc.priority = AudioPriority;        // real time: raised as a client, put back after
                tc.kernel = true;
                const Error err = thread.start(tc, audioEntry, &arg);
                if (err.isError()) {
                        bmdsWarn("%s: no audio thread: %s", _name, err.desc());
                        port.ring.clear();
                        return;
                }
                thread.join();
                if (run.failed) port.ring.clear(); // not played again at once
                return; // the handles close: the synth, then the stream
        }

        // ---- The control thread ----------------------------------------------------------------

        // The wavetable's font loaded and left in the cache (synth/keep-fonts), so the first note
        // doesn't wait: only while it is the system synth. False if the font isn't there yet (the
        // card not mounted): tried again later.
        bool Service::warm() {
                if (_kind != SynthEngine::Kind::Wavetable || !isSystem(_name)) return true;
                String font(MemSpace::Psram);
                if (SoundFonts::defaultFont(font).isError() || font.view() == _warmFont.view()) return true;
                SynthFile::Config sc;
                sc.engine = SynthEngine::Kind::Wavetable;
                auto [handle, err] = SynthFile::open(sc, Root);
                if (err.isError()) return err != Error::NotExist && err != Error::NotReady;
                (void)_warmFont.assign(font.view());
                return true;
        }

        // The port as its setting says.
        void Service::applySetting() {
                const bool on = flag(_enableKey);
                if (on && _number == MidiRouter::NoPort) {
                        port.ring.clear();
                        clearAllowances(); // none can come before the port is there
                        auto [n, err] =
                                Midi::router().addPort(MidiRouter::Kind::System, MidiRouter::Destination, _name, &port);
                        if (err.isError()) {
                                bmdsWarn("%s: no MIDI port: %s", _name, err.desc());
                                return;
                        }
                        _number.store(n);
                        Midi::applySettings(); // synth/system's route, midi/connect's
                } else if (!on && _number != MidiRouter::NoPort) {
                        Midi::router().removePort(_number.load()); // nothing more delivered (or allowed)
                        _number.store(MidiRouter::NoPort);
                        port.ring.clear();
                        clearAllowances();
                }
                return;
        }

        void Service::controlMain() {
                unsigned tries = 0;
                for (;;) {
                        if (_changed.exchange(false)) {
                                SysConf::Id id;
                                while (_watcher.next(id)) {}
                                (void)_watcher.overflowed();
                                applySetting();
                                Midi::applySettings(); // synth/system may name another synth now
                                tries = 0;
                        }
                        bool warmed = true;
                        if (_number != MidiRouter::NoPort && flag(StringView("synth/keep-fonts")) &&
                            tries < PreloadTries) {
                                warmed = warm();
                                tries = warmed ? PreloadTries : tries + 1;
                        }
                        if (_number != MidiRouter::NoPort && !port.ring.isEmpty()) {
                                _stop.store(false, std::memory_order_release);
                                _playing.store(true, std::memory_order_release);
                                play();
                                _playing.store(false, std::memory_order_release);
                                continue; // events that came as it stopped
                        }
                        (void)_wake.acquire(!warmed && tries < PreloadTries ? RetryMs : WaitForever);
                }
        }

        Error Service::start() {
                _storage = static_cast<MidiEvent *>(Memory::alloc(MemSpace::Psram, RingEvents * sizeof(MidiEvent)));
                if (_storage == nullptr) return Error::NoMem;
                (void)port.ring.setStorage(Span<MidiEvent>(_storage, RingEvents));
                Error err = _watcher.start(StringView("synth"), &watched, this);
                if (err.isError()) return err;
                Thread::Config tc;
                tc.name = _name;
                tc.stackBytes = ControlStackBytes;
                tc.stackSpace = MemSpace::Psram;
                tc.priority = 2;
                tc.kernel = true;
                return _control.start(tc, controlEntry, this); // for good: never joined
        }

        SynthServices::Status Service::status() const {
                SynthServices::Status s;
                s.port = _number.load();
                s.on = s.port != MidiRouter::NoPort;
                s.playing = _playing.load();
                s.system = isSystem(_name);
                s.events = _events.load();
                s.dropped = port.dropped.load();
                s.starts = _starts.load();
                s.late = _late.load();
                s.peakVoices = _peakVoices.load();
                s.aheadMs = static_cast<uint32_t>(_aheadNs.load() / 1'000'000);
                s.raised = _raised.load();
                s.cancels = _cancels.load();
                return s;
        }

        Service *serviceOf(SynthEngine::Kind kind) {
                if (!services.isCreated()) return nullptr;
                switch (kind) {
                        case SynthEngine::Kind::Fm: return &services->fm;
                        case SynthEngine::Kind::Wavetable: return &services->wavetable;
                        default: return nullptr;
                }
        }

} // namespace

Error SynthServices::init() {
        services.create();
        if (Error err = services->fm.start(); err.isError()) return err;
        return services->wavetable.start();
}

SynthServices::Status SynthServices::status(SynthEngine::Kind kind) {
        const Service *s = serviceOf(kind);
        return s != nullptr ? s->status() : Status();
}

const char *SynthServices::portName(SynthEngine::Kind kind) {
        return kind == SynthEngine::Kind::Fm ? "fm" : kind == SynthEngine::Kind::Wavetable ? "wavetable" : "";
}

BMDS_NAMESPACE_END
