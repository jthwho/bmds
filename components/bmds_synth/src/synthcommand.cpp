/**
 * @file      synthcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `synth` command (devplan/audio.md §22): a General MIDI synth as a MIDI
 * port and a real-time audio client, until Ctrl+C. Its output is a userspace
 * interface once shipped: keep the lines and their order, add only at the end.
 *
 * Events are placed in time with a fixed latency of one period: an event that
 * arrived during the last period plays at the same offset in this one, so
 * notes keep their spacing instead of jumping to period starts.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/audiodevice.h>
#include <bmds/audiofile.h>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/deadline.h>
#include <bmds/interrupt.h>
#include <bmds/midifile.h>
#include <bmds/semaphore.h>
#include <bmds/shell.h>
#include <bmds/synthfile.h>
#include <bmds/synthservice.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t DefaultPeriod = 128; // 2.7 ms at 48 kHz: room for a chord's note-ons
        constexpr size_t   ReadEvents = 32;     // MIDI events taken at a time

        struct Run {
                        AudioFile        *audio = nullptr;
                        MidiFile         *midi = nullptr;
                        FileHandle       *midiHandle = nullptr;
                        SynthFile        *synth = nullptr;
                        FileHandle       *synthHandle = nullptr;
                        std::atomic<bool> quit{false};
                        Error             error;
                        uint32_t          events = 0;
                        uint32_t          periods = 0;
                        uint32_t          late = 0;
                        int64_t           lastReturnNs = 0; // the cycle's last return: the window's end
        };

        // MIDI events that arrived since the last period, placed at their offsets in this one
        // (one period later), then the period rendered.
        void playPeriod(Run &run, const RtStream::Period &p, uint64_t synthFrame) {
                const int64_t now = Clock::monotonicNs();
                const int64_t periodNs = static_cast<int64_t>(p.frames) * 1000000000 / p.rateHz;
                const int64_t windowStart = run.lastReturnNs != 0 ? run.lastReturnNs : now - periodNs;
                run.lastReturnNs = now;
                while ((run.midi->readiness() & Stream::Readable) != 0) {
                        MidiEvent in[ReadEvents];
                        auto [bytes, err] = run.midiHandle->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(in), sizeof(in)), WaitForever);
                        if (err.isError() || bytes == 0) break;
                        for (size_t i = 0; i < bytes / sizeof(MidiEvent); ++i) {
                                const MidiEvent &m = in[i];
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
                                ++run.events;
                        }
                }
                (void)run.synthHandle->read(
                        WritableByteSpan(reinterpret_cast<uint8_t *>(p.out), p.frames * p.channels * sizeof(float)),
                        WaitForever);
                return;
        }

        void audioThread(void *arg) {
                Run             &run = *static_cast<Run *>(arg);
                RtStream::Period p;
                uint64_t         synthFrame = 0;
                while (!run.quit.load(std::memory_order_acquire)) {
                        const Error err = run.audio->cycle(&p);
                        if (err == Error::Interrupted || err == Error::BrokenPipe) continue; // rejoins
                        if (err.isError()) {
                                run.error = err;
                                break;
                        }
                        playPeriod(run, p, synthFrame);
                        synthFrame += p.frames;
                        ++run.periods;
                        run.late = p.late;
                }
                (void)run.audio->cycle(nullptr); // ends the last period
                AudioDevice::forgetCaller();     // before this thread's task goes
                return;
        }

        int usage(CommandContext &ctx) {
                return ctx.usage();
        }

        // A built-in synth's line (its name, its setting's).
        void showService(CommandContext &ctx, SynthEngine::Kind kind) {
                const SynthServices::Status s = SynthServices::status(kind);
                const char                 *name = SynthServices::portName(kind);
                if (!s.on) {
                        ctx.printf("%s: off (synth/%s)\n", name, name);
                        return;
                }
                char mode[24] = "real time";
                if (s.aheadMs != 0)
                        std::snprintf(mode, sizeof(mode), "ahead %lu ms", static_cast<unsigned long>(s.aheadMs));
                ctx.printf("%s: midi%u, %s, %s; %lu events, %lu dropped; started %lu times, %lu late periods, "
                           "raised %lu times; voices %lu at most\n",
                           name, static_cast<unsigned>(s.port), s.playing ? "playing" : "idle", mode,
                           static_cast<unsigned long>(s.events), static_cast<unsigned long>(s.dropped),
                           static_cast<unsigned long>(s.starts), static_cast<unsigned long>(s.late),
                           static_cast<unsigned long>(s.raised), static_cast<unsigned long>(s.peakVoices));
                return;
        }

        // synth status: the built-in synths' ports (devplan/audio.md §25, §26), and the system synth.
        int showStatus(CommandContext &ctx) {
                showService(ctx, SynthEngine::Kind::Wavetable);
                showService(ctx, SynthEngine::Kind::Fm);
                String system;
                (void)SysConf::get(StringView("synth/system"), system);
                ctx.printf("system: %s (synth/system)\n", system.isEmpty() ? "none" : system.cStr());
                return Shell::ExitSuccess;
        }

        int runSynth(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() == 2 && std::strcmp(args[1], "status") == 0) return showStatus(ctx);
                const char             *font = nullptr;
                const char             *name = "synth";
                SynthEngine::Kind       engine = SynthEngine::Kind::Default;
                uint32_t                period = DefaultPeriod;
                uint32_t                voices = 0;
                for (size_t i = 1; i < args.size(); ++i) {
                        const bool more = i + 1 < args.size();
                        if (std::strcmp(args[i], "-f") == 0 && more) {
                                font = args[++i];
                        } else if (std::strcmp(args[i], "-e") == 0 && more) {
                                const char *e = args[++i];
                                if (std::strcmp(e, "fm") == 0) {
                                        engine = SynthEngine::Kind::Fm;
                                } else if (std::strcmp(e, "wavetable") == 0) {
                                        engine = SynthEngine::Kind::Wavetable;
                                } else {
                                        return usage(ctx);
                                }
                        } else if (std::strcmp(args[i], "-n") == 0 && more) {
                                name = args[++i];
                        } else if (std::strcmp(args[i], "-p") == 0 && more) {
                                period = static_cast<uint32_t>(std::strtoul(args[++i], nullptr, 10));
                        } else if (std::strcmp(args[i], "-v") == 0 && more) {
                                voices = static_cast<uint32_t>(std::strtoul(args[++i], nullptr, 10));
                        } else {
                                return usage(ctx);
                        }
                }
                if (period < AudioDevice::MinClientPeriodFrames || period > AudioDevice::MaxPeriodFrames ||
                    voices > Synth::MaxVoices || std::strlen(name) >= MidiRouter::MaxName)
                        return usage(ctx);

                // The audio side first: its rate is the synth's.
                auto [dsp, derr] = Vfs::open("/dev/dsp", Vfs::Write, ctx.vfs());
                AudioFile *audio = derr.isOk() ? dsp->stream().audioFile() : nullptr;
                if (audio == nullptr) {
                        ctx.eprintf("synth: /dev/dsp: %s\n", derr.isError() ? derr.desc() : "not an audio device");
                        return Shell::ExitFailure;
                }
                AudioFile::Config ac;
                ac.format = SampleFormat::F32Le;
                ac.channels = 2;
                ac.rateHz = 0;
                ac.wakeFrames = period;
                ac.flags = AudioFile::Realtime;
                Error err = audio->configure(ac);
                if (err.isError()) {
                        ctx.eprintf("synth: real-time audio: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const AudioFile::Config got = audio->config();

                SynthFile::Config sc;
                sc.rateHz = got.rateHz;
                sc.channels = 2;
                sc.format = SampleFormat::F32Le;
                sc.maxVoices = static_cast<uint16_t>(voices);
                sc.font = font != nullptr ? StringView(font) : StringView();
                sc.engine = engine;
                const int64_t loadStart = Clock::monotonicNs();
                auto [synthHandle, serr] = SynthFile::open(sc, ctx.vfs());
                if (serr.isError()) {
                        ctx.eprintf("synth: the instruments: %s\n",
                                    serr == Error::Invalid ? "damaged, or not a SoundFont or OP2 bank" : serr.desc());
                        return Shell::ExitFailure;
                }
                SynthFile *synth = synthHandle->stream().synthFile();

                auto [midiHandle, merr] = Vfs::open("/dev/midinew", Vfs::Read | Vfs::Write, ctx.vfs());
                MidiFile *midi = merr.isOk() ? midiHandle->stream().midiFile() : nullptr;
                if (midi == nullptr) {
                        ctx.eprintf("synth: /dev/midinew: %s\n", merr.isError() ? merr.desc() : "not a MIDI file");
                        return Shell::ExitFailure;
                }
                MidiFile::Config mc;
                MidiFile::Config mgot;
                mc.flags = MidiFile::Create | MidiFile::Destination | MidiFile::Events;
                mc.eventSize = sizeof(MidiEvent);
                std::snprintf(mc.name, sizeof(mc.name), "%s", name);
                err = midi->configure(mc, mgot);
                if (err.isError()) {
                        ctx.eprintf("synth: its MIDI port: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const SynthFile::Info info = synth->info();
                ctx.printf("synth: midi%u (%s), %s: %u presets, %u KB (%u ms); %u Hz, %u frames a period, %u voices; "
                           "Ctrl+C stops\n",
                           static_cast<unsigned>(mgot.port), mgot.name, synth->fontPath().data(),
                           static_cast<unsigned>(info.presets), static_cast<unsigned>(info.fontBytes / 1024),
                           static_cast<unsigned>((Clock::monotonicNs() - loadStart) / 1000000),
                           static_cast<unsigned>(got.rateHz), static_cast<unsigned>(got.wakeFrames),
                           static_cast<unsigned>(info.config.maxVoices));

                Run run;
                run.audio = audio;
                run.midi = midi;
                run.midiHandle = midiHandle.get();
                run.synth = synth;
                run.synthHandle = synthHandle.get();
                Thread         thread;
                Thread::Config tc;
                tc.name = "synth";
                tc.stackBytes = 4096;
                tc.stackSpace = MemSpace::Internal;
                tc.priority = 2;
                err = thread.start(tc, audioThread, &run);
                if (err.isError()) {
                        ctx.eprintf("synth: no thread: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                // Until Ctrl+C.
                Semaphore never(1, 0);
                while (Interrupt::waitCurrent(never, Deadline(WaitForever)) != Error::Interrupted) {}
                run.quit.store(true, std::memory_order_release);
                thread.join();
                const SynthFile::Info end = synth->info();
                ctx.printf("synth: %u events, %u periods, %u late; voices %u at most (%u at the limit); CPU %u.%u%% at "
                           "most\n",
                           static_cast<unsigned>(run.events), static_cast<unsigned>(run.periods),
                           static_cast<unsigned>(run.late), static_cast<unsigned>(end.synth.peakVoices),
                           static_cast<unsigned>(end.synth.notesAtLimit),
                           static_cast<unsigned>(end.cpuPermilleMax / 10),
                           static_cast<unsigned>(end.cpuPermilleMax % 10));
                if (run.error.isError()) {
                        ctx.eprintf("synth: %s\n", run.error.desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitInterrupted;
        }

} // namespace

BMDS_SHELL_COMMAND(synth, runSynth, "[-e fm|wavetable] [-f FILE] [-n NAME] [-p FRAMES] [-v VOICES] | status",
                   "A General MIDI synth: a MIDI port (NAME, synth) playing on /dev/dsp until Ctrl+C, the FM engine "
                   "on an OP2 bank (synth/fm-bank) or the wavetable on a SoundFont (synth/font); by default the "
                   "system synth's, or FILE's kind; 'status': the built-in synths' ports and the system synth")

BMDS_NAMESPACE_END
