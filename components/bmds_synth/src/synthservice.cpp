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
 * arrived in the last period are placed at their offsets in this one, a period late.
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
                                        int64_t     lastReturnNs = 0;
                                        int64_t     quietNs = 0; // since an event or a voice
                        };

                        static void controlEntry(void *arg) { static_cast<Service *>(arg)->controlMain(); }
                        static void audioEntry(void *arg);

                        void playPeriod(Run &run, const RtStream::Period &p, uint64_t synthFrame);
                        void audioMain(Run &run);
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

        void Service::playPeriod(Run &run, const RtStream::Period &p, uint64_t synthFrame) {
                const int64_t now = Clock::monotonicNs();
                const int64_t periodNs = static_cast<int64_t>(p.frames) * 1000000000 / p.rateHz;
                const int64_t windowStart = run.lastReturnNs != 0 ? run.lastReturnNs : now - periodNs;
                run.lastReturnNs = now;
                MidiEvent m;
                bool      any = false;
                while (port.ring.pop(m)) {
                        if (m.size == 0 || m.size > SynthEngine::MaxEventBytes) continue;
                        int64_t offset = (m.timeNs - windowStart) * p.rateHz / 1000000000;
                        if (offset < 0) offset = 0;
                        if (offset >= static_cast<int64_t>(p.frames)) offset = p.frames - 1;
                        SynthEngine::Event e;
                        e.frame = synthFrame + static_cast<uint64_t>(offset);
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
                const SynthFile::Info info = run.synth->info();
                if (info.synth.peakVoices > _peakVoices.load(std::memory_order_relaxed))
                        _peakVoices.store(info.synth.peakVoices, std::memory_order_relaxed);
                run.quietNs = any || info.synth.voices != 0 ? 0 : run.quietNs + periodNs;
                return;
        }

        void Service::audioEntry(void *arg) {
                auto *run = static_cast<std::pair<Service *, Run *> *>(arg);
                run->first->audioMain(*run->second);
                return;
        }

        void Service::audioMain(Run &run) {
                RtStream::Period p;
                uint64_t         synthFrame = 0;
                while (!_stop.load(std::memory_order_acquire) && run.quietNs < IdleNs) {
                        const Error err = run.audio->cycle(&p);
                        if (err == Error::Interrupted || err == Error::BrokenPipe) continue; // rejoins
                        if (err.isError()) {
                                bmdsWarn("%s: the stream: %s", _name, err.desc());
                                break;
                        }
                        playPeriod(run, p, synthFrame);
                        synthFrame += p.frames;
                        _late.store(_lateBase + p.late, std::memory_order_relaxed);
                }
                (void)run.audio->cycle(nullptr); // ends the last period
                AudioDevice::forgetCaller();     // before this thread's task goes
                return;
        }

        // A first note that waited for the font to load from the card: said, so the pause has a reason.
        void Service::tellLoaded(StringView path, int64_t ns) {
                Notification n;
                n.replaces = _notice;
                n.urgency = Notification::Urgency::Low;
                n.flags = Notification::Transient;
                std::snprintf(n.app, sizeof(n.app), "Synthesizer");
                std::snprintf(n.icon, sizeof(n.icon), "audio-x-midi");
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
                AudioFile::Config ac;
                ac.format = SampleFormat::F32Le;
                ac.channels = 2;
                ac.rateHz = 0;
                ac.wakeFrames = 0; // the device's period: slack beside a busy game, no client forcing it down
                ac.flags = AudioFile::Realtime;
                Error err = audio->configure(ac);
                if (err.isError()) {
                        bmdsWarn("%s: real-time audio: %s", _name, err.desc());
                        port.ring.clear();
                        return;
                }
                SynthFile::Config sc;
                sc.rateHz = audio->config().rateHz;
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
                tc.priority = 2;
                tc.kernel = true;
                err = thread.start(tc, audioEntry, &arg);
                if (err.isError()) {
                        bmdsWarn("%s: no audio thread: %s", _name, err.desc());
                        port.ring.clear();
                        return;
                }
                thread.join();
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
                        auto [n, err] =
                                Midi::router().addPort(MidiRouter::Kind::System, MidiRouter::Destination, _name, &port);
                        if (err.isError()) {
                                bmdsWarn("%s: no MIDI port: %s", _name, err.desc());
                                return;
                        }
                        _number.store(n);
                        Midi::applySettings(); // synth/system's route, midi/connect's
                } else if (!on && _number != MidiRouter::NoPort) {
                        Midi::router().removePort(_number.load()); // nothing more delivered
                        _number.store(MidiRouter::NoPort);
                        port.ring.clear();
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
