/**
 * @file      audiocommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `audio` command: devices, their status, controls and registers, test
 * tones, system sounds and a sweep through a PCM stream (devplan/audio.md
 * §16, §17). Its output formats are a userspace interface once shipped: keep
 * the lines and their order, add only at the end.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <bmds/audiodevice.h>
#include <bmds/board.h>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr uint32_t DefaultToneMs = 2000;
        constexpr int32_t  DefaultToneCentiDb = -1200;
        constexpr uint32_t DefaultSweepRate = 44100; // resampled to the usual 48 kHz
        constexpr uint32_t DefaultSweepMs = 10000;
        constexpr float    SweepLevel = 0.25f; // -12 dBFS
        constexpr float    SweepFromHz = 20.0f;
        constexpr size_t   SweepBlockFrames = 256;

        // `dspN` names a device.
        bool isDeviceName(const char *arg) {
                return std::strncmp(arg, "dsp", 3) == 0 && arg[3] >= '0' && arg[3] <= '9';
        }

        // The device named at args[at] (moving `at` past it), or dsp0.
        AudioDevice *deviceArg(CommandContext &ctx, Args args, size_t &at) {
                size_t number = 0;
                if (at < args.size() && isDeviceName(args[at])) {
                        number = static_cast<size_t>(std::strtoul(args[at] + 3, nullptr, 10));
                        ++at;
                }
                auto [d, err] = AudioDevice::device(number);
                if (err.isError()) {
                        ctx.eprintf("audio: no device dsp%u on this board\n", static_cast<unsigned>(number));
                        return nullptr;
                }
                return d;
        }

        // A whole number: decimal, or hex with 0x.
        bool parseNumber(const char *text, uint32_t &out) {
                if (text == nullptr || *text == '\0' || *text == '-') return false;
                char         *end = nullptr;
                unsigned long v = std::strtoul(text, &end, 0);
                if (*end != '\0' || v > 0xffffffffu) return false;
                out = static_cast<uint32_t>(v);
                return true;
        }

        // Seconds with up to three decimals, as milliseconds.
        bool parseSeconds(const char *text, uint32_t &ms) {
                uint32_t    whole = 0, frac = 0, digits = 0;
                const char *p = text;
                if (*p == '\0') return false;
                for (; *p >= '0' && *p <= '9'; ++p) {
                        whole = whole * 10 + static_cast<uint32_t>(*p - '0');
                        if (whole > 3600) return false;
                }
                if (*p == '.') {
                        for (++p; *p >= '0' && *p <= '9' && digits < 3; ++p, ++digits)
                                frac = frac * 10 + static_cast<uint32_t>(*p - '0');
                }
                if (*p != '\0' || (p == text)) return false;
                for (; digits < 3; ++digits) frac *= 10;
                ms = whole * 1000 + frac;
                return ms > 0;
        }

        void printFault(CommandContext &ctx, AudioDevice &d) {
                const AudioCodec::Fault &f = d.codec().fault();
                if (f.error.isOk()) return;
                if (f.reg < 0) {
                        ctx.printf("Fault:    %s: %s\n", f.what, f.error.desc());
                } else if (f.error == Error::DeviceError) {
                        ctx.printf("Fault:    %s: register 0x%02x wrote 0x%02x, read back 0x%02x\n", f.what,
                                   static_cast<unsigned>(f.reg), f.wrote, f.read);
                } else {
                        ctx.printf("Fault:    %s: register 0x%02x: %s\n", f.what, static_cast<unsigned>(f.reg),
                                   f.error.desc());
                }
                return;
        }

        int listDevices(CommandContext &ctx) {
                bool any = false;
                for (size_t n = 0; n < AudioDevice::MaxDevices; ++n) {
                        auto [d, err] = AudioDevice::device(n);
                        if (err.isError()) continue;
                        const AudioDevice::Status s = d->status();
                        ctx.printf("dsp%u  %s  %s", static_cast<unsigned>(n), d->codec().name(),
                                   AudioDevice::stateName(s.state));
                        if (s.state == AudioDevice::State::Running)
                                ctx.printf("  %lu Hz", static_cast<unsigned long>(s.rateHz));
                        ctx.printf("\n");
                        any = true;
                }
                if (!any) ctx.printf("audio: this board has no audio devices\n");
                return Shell::ExitSuccess;
        }

        int showStatus(CommandContext &ctx, AudioDevice &d) {
                const BoardAudio &a = board().audio;
                char              id[48] = "";
                {
                        Mutex::Locker locker(d.lock(), WaitForever);
                        if (!d.codec().isRunning()) (void)d.codec().attach();
                        d.codec().identity(id, sizeof(id));
                }
                const AudioDevice::Status s = d.status();
                ctx.printf("Device:   dsp%u, %s on i2c%u at 0x%02x, %s\n", static_cast<unsigned>(d.number()),
                           d.codec().name(), static_cast<unsigned>(a.i2c), a.i2cAddr, id);
                ctx.printf("State:    %s", AudioDevice::stateName(s.state));
                if (s.state == AudioDevice::State::Running)
                        ctx.printf(" at %lu Hz%s", static_cast<unsigned long>(s.rateHz),
                                   s.toneActive ? ", tone playing" : "");
                if (s.state == AudioDevice::State::Faulted) ctx.printf(": %s", d.lastError().desc());
                ctx.printf("\n");
                printFault(ctx, d);
                if (s.rateHz == 0) return Shell::ExitSuccess;
                ctx.printf("MCLK:     %lu Hz asked, %lu Hz set; APLL %lu Hz\n",
                           static_cast<unsigned long>(s.mclkAskedHz), static_cast<unsigned long>(s.mclkHz),
                           static_cast<unsigned long>(s.apllHz));
                if (s.measuredMilliHz != 0) {
                        const int64_t nominal = static_cast<int64_t>(s.rateHz) * 1000;
                        const int64_t tenthsPpm =
                                (static_cast<int64_t>(s.measuredMilliHz) - nominal) * 10'000'000 / nominal;
                        const int64_t a10 = tenthsPpm < 0 ? -tenthsPpm : tenthsPpm;
                        ctx.printf("Measured: %lu.%03lu Hz over %lu.%lu s (%s%lld.%lld ppm)\n",
                                   static_cast<unsigned long>(s.measuredMilliHz / 1000),
                                   static_cast<unsigned long>(s.measuredMilliHz % 1000),
                                   static_cast<unsigned long>(s.measuredMs / 1000),
                                   static_cast<unsigned long>(s.measuredMs % 1000 / 100), tenthsPpm < 0 ? "-" : "+",
                                   static_cast<long long>(a10 / 10), static_cast<long long>(a10 % 10));
                } else {
                        ctx.printf("Measured: not yet (needs a second of play)\n");
                }
                ctx.printf("Periods:  %lu, %lu late; fill %lu us, at most %lu us\n",
                           static_cast<unsigned long>(s.periods), static_cast<unsigned long>(s.late),
                           static_cast<unsigned long>(s.fillUsLast), static_cast<unsigned long>(s.fillUsMax));
                const uint32_t periodUs =
                        static_cast<uint32_t>(static_cast<uint64_t>(s.periodFrames) * 1'000'000 / s.rateHz);
                ctx.printf("Period:   %lu frames (%lu.%03lu ms), %u buffers\n",
                           static_cast<unsigned long>(s.periodFrames), static_cast<unsigned long>(periodUs / 1000),
                           static_cast<unsigned long>(periodUs % 1000), static_cast<unsigned>(AudioDevice::DmaBuffers));
                if (s.mixedPeriods != 0) {
                        // The mix's average time a period, against the period: the share of a core.
                        const uint64_t avg = s.mixedUs / s.mixedPeriods;
                        const uint64_t tenthsPct = periodUs == 0 ? 0 : s.mixedUs * 1000 / s.mixedPeriods / periodUs;
                        ctx.printf("Load:     %lu us a period mixing, on average (%lu.%lu%% of a core), over %lu "
                                   "periods\n",
                                   static_cast<unsigned long>(avg), static_cast<unsigned long>(tenthsPct / 10),
                                   static_cast<unsigned long>(tenthsPct % 10),
                                   static_cast<unsigned long>(s.mixedPeriods));
                }
                ctx.printf("Volume:   %ld dBFS%s%s\n", static_cast<long>(s.volumeDb), s.muted ? ", muted" : "",
                           s.held ? ", held running" : "");
                ctx.printf("Limiter:  %lu frames reduced\n", static_cast<unsigned long>(s.limited));
                // 16 lines of 112 bytes: not on the shell's stack.
                auto *lines = static_cast<AudioDevice::StreamLine *>(
                        Memory::alloc(MemSpace::Psram, AudioDevice::MaxStreams * sizeof(AudioDevice::StreamLine)));
                if (lines == nullptr) {
                        ctx.eprintf("audio: %s\n", Error(Error::NoMem).desc());
                        return Shell::ExitFailure;
                }
                const size_t n = d.streamLines(lines, AudioDevice::MaxStreams);
                ctx.printf("Streams:  %u\n", static_cast<unsigned>(n));
                for (size_t i = 0; i < n; ++i) ctx.printf("  %s\n", lines[i].text);
                Memory::free(lines);
                return Shell::ExitSuccess;
        }

        // Starts the device and holds it running (no idle stop) until `audio stop`.
        int runStart(CommandContext &ctx, AudioDevice &d, Args args, size_t at) {
                uint32_t rate = 0; // the setting's
                if (at < args.size() && (!parseNumber(args[at++], rate) || rate == 0)) return ctx.usage();
                if (at != args.size()) return ctx.usage();
                const Error err = d.hold(rate);
                if (err.isError()) {
                        ctx.eprintf("audio: dsp%u: start: %s\n", static_cast<unsigned>(d.number()), err.desc());
                        printFault(ctx, d);
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int runTone(CommandContext &ctx, AudioDevice &d, Args args, size_t at) {
                uint32_t        frequency = 0;
                uint32_t        ms = DefaultToneMs;
                int32_t         level = DefaultToneCentiDb;
                SineTone::Slots slots = SineTone::Both;
                if (at >= args.size() || !parseNumber(args[at++], frequency)) return ctx.usage();
                if (at < args.size() && !parseSeconds(args[at++], ms)) return ctx.usage();
                if (at < args.size()) {
                        const char *w = args[at++];
                        if (std::strcmp(w, "left") == 0) {
                                slots = SineTone::Left;
                        } else if (std::strcmp(w, "right") == 0) {
                                slots = SineTone::Right;
                        } else if (std::strcmp(w, "both") != 0) {
                                return ctx.usage();
                        }
                }
                if (at < args.size()) {
                        constexpr AudioControl Level = {"level", "", AudioControl::Type::Decibel, -9600, 0, 1, ""};
                        auto [v, err] = Level.parse(args[at++]);
                        if (err.isError()) {
                                ctx.eprintf("audio: level: -96 to 0 dBFS\n");
                                return Shell::ExitUsage;
                        }
                        level = v;
                }
                if (at != args.size()) return ctx.usage();
                auto [id, err] = d.playTone(frequency, ms, level, slots);
                if (err == Error::Invalid) {
                        ctx.eprintf("audio: tone: the frequency must be below half the rate\n");
                        return Shell::ExitUsage;
                }
                if (err.isError()) {
                        ctx.eprintf("audio: dsp%u: %s\n", static_cast<unsigned>(d.number()), err.desc());
                        printFault(ctx, d);
                        return Shell::ExitFailure;
                }
                // Until it has played, so `audio tone A; audio tone B` plays both.
                if (d.waitTone(id, &ctx.shell().interrupt()) == Error::Interrupted) {
                        d.cancelTone(id);
                        return Shell::ExitInterrupted;
                }
                return Shell::ExitSuccess;
        }

        int runSound(CommandContext &ctx, AudioDevice &d, Args args, size_t at) {
                if (at + 1 != args.size()) return ctx.usage();
                for (size_t i = 0; i < SystemSoundCount; ++i) {
                        const auto sound = static_cast<SystemSound>(i);
                        if (std::strcmp(args[at], systemSoundName(sound)) != 0) continue;
                        const Error err = d.playSound(sound);
                        if (err.isError()) {
                                ctx.eprintf("audio: dsp%u: %s\n", static_cast<unsigned>(d.number()), err.desc());
                                printFault(ctx, d);
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                ctx.eprintf("audio: sound: bell, click, error or notify\n");
                return Shell::ExitUsage;
        }

        // The sweep's stream and a block of its samples (PSRAM: a command's stack is small).
        struct Sweep {
                        PcmStream stream{"sweep"};
                        float     stereo[SweepBlockFrames * 2];
                        uint8_t   block[SweepBlockFrames * 8];
        };

        // A logarithmic sweep through a PCM stream: conversion and resampling, by ear.
        int runSweep(CommandContext &ctx, AudioDevice &d, Args args, size_t at) {
                PcmStream::Format format;
                format.rateHz = DefaultSweepRate;
                format.channels = 2;
                uint32_t ms = DefaultSweepMs;
                if (at < args.size() &&
                    (!parseNumber(args[at++], format.rateHz) || format.rateHz < PcmStream::MinRateHz ||
                     format.rateHz > PcmStream::MaxRateHz)) {
                        ctx.eprintf("audio: sweep: a rate from %lu to %lu Hz\n",
                                    static_cast<unsigned long>(PcmStream::MinRateHz),
                                    static_cast<unsigned long>(PcmStream::MaxRateHz));
                        return Shell::ExitUsage;
                }
                if (at < args.size()) {
                        auto [f, err] = SampleFormats::parse(args[at++]);
                        if (err.isError()) {
                                ctx.eprintf("audio: sweep: formats: u8 s8 s16le s16be s24le s24-3le s32le f32le mu-law "
                                            "a-law\n");
                                return Shell::ExitUsage;
                        }
                        format.format = f;
                }
                if (at < args.size() && !parseSeconds(args[at++], ms)) return ctx.usage();
                if (at != args.size()) return ctx.usage();
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Sweep));
                if (mem == nullptr) {
                        ctx.eprintf("audio: sweep: out of memory\n");
                        return Shell::ExitFailure;
                }
                auto        *sweep = new (mem) Sweep;
                PcmStream   &stream = sweep->stream;
                const size_t ring = format.rateHz / 5; // 200 ms, playing from 100 ms
                Error        err = stream.open(format, ring, ring / 2, MemSpace::Psram);
                if (err.isOk()) err = d.attach(stream);
                int status = Shell::ExitSuccess;
                if (err.isError()) {
                        ctx.eprintf("audio: dsp%u: sweep: %s\n", static_cast<unsigned>(d.number()), err.desc());
                        printFault(ctx, d);
                        status = Shell::ExitFailure;
                }
                // Float only (double is done in software here): the frequency grows by a constant
                // factor a frame, the phase by the frequency.
                const uint64_t  total = static_cast<uint64_t>(ms) * format.rateHz / 1000;
                const float     toHz = 0.45f * static_cast<float>(format.rateHz);
                const float     growth = std::exp(std::log(toHz / SweepFromHz) / static_cast<float>(total));
                constexpr float TwoPi = 6.28318530717958647692f;
                float           step = TwoPi * SweepFromHz / static_cast<float>(format.rateHz);
                float           phase = 0;
                float          *stereo = sweep->stereo;
                Interrupt      *interrupt = &ctx.shell().interrupt();
                for (uint64_t n = 0; status == Shell::ExitSuccess && n < total;) {
                        const size_t frames =
                                total - n < SweepBlockFrames ? static_cast<size_t>(total - n) : SweepBlockFrames;
                        for (size_t i = 0; i < frames; ++i, ++n) {
                                const float v = SweepLevel * std::sin(phase);
                                stereo[2 * i] = v;
                                stereo[2 * i + 1] = v;
                                phase += step;
                                if (phase > TwoPi) phase -= TwoPi;
                                step *= growth;
                        }
                        SampleFormats::fromFloat(format.format, format.channels, stereo, frames, sweep->block);
                        auto [taken, werr] =
                                stream.write(ByteSpan(sweep->block, frames * stream.frameBytes()), false, interrupt);
                        if (werr == Error::Interrupted) {
                                status = Shell::ExitInterrupted;
                        } else if (werr.isError() || taken != frames * stream.frameBytes()) {
                                ctx.eprintf("audio: sweep: the stream ended: %s\n", werr.desc());
                                status = Shell::ExitFailure;
                        }
                }
                if (status == Shell::ExitSuccess && stream.drain(interrupt, WaitForever) == Error::Interrupted)
                        status = Shell::ExitInterrupted;
                if (status != Shell::ExitFailure || err.isOk()) {
                        ctx.printf("sweep: %lu frames of %s at %lu Hz played, %lu underruns\n",
                                   static_cast<unsigned long>(stream.playedFrames()),
                                   SampleFormats::name(format.format), static_cast<unsigned long>(format.rateHz),
                                   static_cast<unsigned long>(stream.underruns()));
                }
                d.detach(stream);
                sweep->~Sweep();
                Memory::free(mem);
                return status;
        }

        void printControl(CommandContext &ctx, const AudioControl &c, int32_t value) {
                char text[AudioControl::MaxText];
                c.format(value, text, sizeof(text));
                char range[80];
                switch (c.type) {
                        case AudioControl::Type::Bool: std::snprintf(range, sizeof(range), "on|off"); break;
                        case AudioControl::Type::Enum: std::snprintf(range, sizeof(range), "%s", c.choices); break;
                        case AudioControl::Type::Decibel: {
                                char lo[AudioControl::MaxText], hi[AudioControl::MaxText], st[AudioControl::MaxText];
                                c.format(c.min, lo, sizeof(lo));
                                c.format(c.max, hi, sizeof(hi));
                                c.format(c.step, st, sizeof(st));
                                std::snprintf(range, sizeof(range), "%.*s..%s, %s steps",
                                              static_cast<int>(std::strlen(lo) - 3), lo, hi, st);
                                break;
                        }
                        case AudioControl::Type::Int:
                                std::snprintf(range, sizeof(range), "%ld..%ld", static_cast<long>(c.min),
                                              static_cast<long>(c.max));
                                break;
                }
                ctx.printf("%-12s %-10s %-26s %s\n", c.name, text, range, c.help);
                return;
        }

        int runCtl(CommandContext &ctx, AudioDevice &d, Args args, size_t at) {
                AudioCodec              &codec = d.codec();
                Span<const AudioControl> controls = codec.controls();
                if (at == args.size()) {
                        Mutex::Locker locker(d.lock(), WaitForever);
                        for (size_t i = 0; i < controls.size(); ++i)
                                printControl(ctx, controls[i], codec.controlValue(i));
                        return Shell::ExitSuccess;
                }
                const char *name = args[at++];
                size_t      index = controls.size();
                for (size_t i = 0; i < controls.size(); ++i)
                        if (std::strcmp(controls[i].name, name) == 0) index = i;
                if (index == controls.size()) {
                        ctx.eprintf("audio: dsp%u has no control '%s' ('audio ctl' lists them)\n",
                                    static_cast<unsigned>(d.number()), name);
                        return Shell::ExitUsage;
                }
                const AudioControl &c = controls[index];
                Mutex::Locker       locker(d.lock(), WaitForever);
                if (at == args.size()) {
                        printControl(ctx, c, codec.controlValue(index));
                        return Shell::ExitSuccess;
                }
                auto [value, perr] = c.parse(args[at++]);
                if (perr.isError() || at != args.size()) {
                        ctx.eprintf("audio: %s: not a value of it\n", c.name);
                        printControl(ctx, c, codec.controlValue(index));
                        return Shell::ExitUsage;
                }
                const Error err = codec.setControl(index, value);
                if (err.isError()) {
                        ctx.eprintf("audio: %s: %s\n", c.name, err.desc());
                        printFault(ctx, d);
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int showRegisters(CommandContext &ctx, AudioDevice &d) {
                Mutex::Locker locker(d.lock(), WaitForever);
                AudioCodec   &codec = d.codec();
                if (!codec.isRunning()) {
                        const Error err = codec.attach();
                        if (err.isError()) {
                                ctx.eprintf("audio: dsp%u: %s\n", static_cast<unsigned>(d.number()), err.desc());
                                return Shell::ExitFailure;
                        }
                }
                Span<const uint8_t> regs = codec.registerList();
                bool                differs = false;
                for (size_t i = 0; i < regs.size(); ++i) {
                        const uint8_t reg = regs[i];
                        if (i == 0 || reg % 8 == 0 || reg != regs[i - 1] + 1)
                                ctx.printf("%s0x%02x:", i == 0 ? "" : "\n", reg);
                        auto [v, err] = codec.readRegister(reg);
                        const int16_t expected = codec.expectedRegister(reg);
                        if (err.isError()) {
                                ctx.printf("  --");
                        } else {
                                const bool mismatch = expected >= 0 && expected != v;
                                differs |= mismatch;
                                ctx.printf(" %c%02x", mismatch ? '!' : ' ', v);
                        }
                }
                ctx.printf("\n");
                if (differs) ctx.printf("!: differs from what was written ('audio reg' shows both)\n");
                return Shell::ExitSuccess;
        }

        int runRegister(CommandContext &ctx, AudioDevice &d, Args args, size_t at) {
                uint32_t reg = 0, value = 0;
                if (at >= args.size() || !parseNumber(args[at++], reg) || reg > 0xff) return ctx.usage();
                const bool write = at < args.size();
                if (write && (!parseNumber(args[at++], value) || value > 0xff)) return ctx.usage();
                if (at != args.size()) return ctx.usage();
                Mutex::Locker locker(d.lock(), WaitForever);
                AudioCodec   &codec = d.codec();
                if (!codec.isRunning()) {
                        const Error err = codec.attach();
                        if (err.isError()) {
                                ctx.eprintf("audio: dsp%u: %s\n", static_cast<unsigned>(d.number()), err.desc());
                                return Shell::ExitFailure;
                        }
                }
                const auto r = static_cast<uint8_t>(reg);
                if (write) {
                        const Error err = codec.writeRegister(r, static_cast<uint8_t>(value));
                        if (err.isError()) {
                                ctx.eprintf("audio: 0x%02x: %s\n", r, err.desc());
                                printFault(ctx, d);
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                auto [v, err] = codec.readRegister(r);
                if (err.isError()) {
                        ctx.eprintf("audio: 0x%02x: %s\n", r, err.desc());
                        return Shell::ExitFailure;
                }
                const int16_t expected = codec.expectedRegister(r);
                if (expected < 0) {
                        ctx.printf("0x%02x: 0x%02x\n", r, v);
                } else {
                        ctx.printf("0x%02x: 0x%02x (written 0x%02x)\n", r, v, static_cast<unsigned>(expected));
                }
                return Shell::ExitSuccess;
        }

        int runAudio(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return listDevices(ctx);
                const char *what = args[1];
                size_t      at = 2;
                if (std::strcmp(what, "status") == 0 || std::strcmp(what, "start") == 0 ||
                    std::strcmp(what, "stop") == 0 || std::strcmp(what, "tone") == 0 || std::strcmp(what, "ctl") == 0 ||
                    std::strcmp(what, "regs") == 0 || std::strcmp(what, "reg") == 0 ||
                    std::strcmp(what, "sound") == 0 || std::strcmp(what, "sweep") == 0) {
                        AudioDevice *d = deviceArg(ctx, args, at);
                        if (d == nullptr) return Shell::ExitFailure;
                        if (std::strcmp(what, "status") == 0)
                                return at == args.size() ? showStatus(ctx, *d) : ctx.usage();
                        if (std::strcmp(what, "start") == 0) return runStart(ctx, *d, args, at);
                        if (std::strcmp(what, "stop") == 0) {
                                if (at != args.size()) return ctx.usage();
                                const Error err = d->stop();
                                if (err.isError()) {
                                        ctx.eprintf("audio: dsp%u: stop: %s\n", static_cast<unsigned>(d->number()),
                                                    err.desc());
                                        printFault(ctx, *d);
                                        return Shell::ExitFailure;
                                }
                                return Shell::ExitSuccess;
                        }
                        if (std::strcmp(what, "tone") == 0) return runTone(ctx, *d, args, at);
                        if (std::strcmp(what, "sound") == 0) return runSound(ctx, *d, args, at);
                        if (std::strcmp(what, "sweep") == 0) return runSweep(ctx, *d, args, at);
                        if (std::strcmp(what, "ctl") == 0) return runCtl(ctx, *d, args, at);
                        if (std::strcmp(what, "regs") == 0)
                                return at == args.size() ? showRegisters(ctx, *d) : ctx.usage();
                        return runRegister(ctx, *d, args, at);
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(audio, runAudio,
                   "[status|start [RATE]|stop|tone FREQ [SECONDS] [left|right|both] [dBFS]|ctl [NAME [VALUE]]|"
                   "regs|reg REG [VALUE]|sound bell|click|error|notify|sweep [RATE] [FORMAT] [SECONDS]] "
                   "(each may name a device first: dsp0)",
                   "Audio devices: status, start and stop, test tones and sounds, a sweep, the codec's controls and "
                   "registers")

BMDS_NAMESPACE_END
