/**
 * @file      saycommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `say` command (devplan/speech.md §4): text spoken through /dev/dsp, as
 * `play` plays, or written to a WAV file. Modelled on macOS's `say`.
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <new>
#include <bmds/audiofile.h>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/speaker.h>
#include <bmds/vfs.h>
#include <bmds/voices.h>
#include <bmds/wavfile.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr uint32_t NormalWpm = 170; // Pico's own speed (measured: en-US prose, 171)
        constexpr uint32_t MinWpm = NormalWpm * Speaker::MinSpeed / 100;
        constexpr uint32_t MaxWpm = NormalWpm * Speaker::MaxSpeed / 100;
        // Pico makes a sentence's first samples only after working through it; at the slowest
        // rate that took about 540 ms on the board (as 67 ms on the host, 8 times faster), past a
        // 0.5 s ring: so 2 s queued (64 KB), playing from 250 ms so speech starts as soon.
        constexpr uint32_t RingMs = 2000;
        constexpr uint32_t StartMs = 250;
        constexpr size_t   BlockSamples = 1024;
        constexpr size_t   TextBytes = 512;

        struct Options {
                        const char      *voice = nullptr; // null: speech/voice
                        Speaker::Prosody prosody;
                        bool             markup = false;
                        bool             raw = false;
                        bool             list = false;
                        const char      *output = nullptr; // null: the speakers
                        const char      *file = nullptr;   // null: the text, or stdin
                        size_t           textAt = 0;       // the first word of the text in the arguments
        };

        // The speaker and its buffers, in PSRAM.
        struct Work {
                        Speaker speaker;
                        int16_t samples[BlockSamples];
                        uint8_t text[TextBytes];
        };

        // Parses @p s as a whole number in [@p min, @p max].
        bool parseNumber(const char *s, uint32_t min, uint32_t max, uint32_t &out) {
                char *end = nullptr;
                errno = 0;
                const long v = std::strtol(s, &end, 10);
                if (errno != 0 || end == s || *end != 0 || v < long(min) || v > long(max)) return false;
                out = static_cast<uint32_t>(v);
                return true;
        }

        // Returns false (after a message) on a bad argument.
        bool parse(CommandContext &ctx, Options &o) {
                const Args args = ctx.args();
                size_t     at = 1;
                for (; at < args.size(); ++at) {
                        const char *a = args[at];
                        if (std::strcmp(a, "--") == 0) {
                                ++at;
                                break;
                        }
                        if (a[0] != '-' || a[1] == 0) break;
                        if (std::strcmp(a, "--markup") == 0) {
                                o.markup = true;
                                continue;
                        }
                        if (std::strcmp(a, "--raw") == 0) {
                                o.raw = true;
                                continue;
                        }
                        const bool takesValue = std::strcmp(a, "-v") == 0 || std::strcmp(a, "-r") == 0 ||
                                                std::strcmp(a, "-p") == 0 || std::strcmp(a, "-o") == 0 ||
                                                std::strcmp(a, "-f") == 0;
                        if (!takesValue || at + 1 == args.size()) {
                                ctx.usage();
                                return false;
                        }
                        const char *v = args[++at];
                        uint32_t    n = 0;
                        switch (a[1]) {
                                case 'v':
                                        o.voice = v;
                                        o.list = std::strcmp(v, "?") == 0;
                                        break;
                                case 'r':
                                        if (!parseNumber(v, MinWpm, MaxWpm, n)) {
                                                ctx.eprintf("say: -r: words a minute, %lu to %lu\n",
                                                            static_cast<unsigned long>(MinWpm),
                                                            static_cast<unsigned long>(MaxWpm));
                                                return false;
                                        }
                                        n = (n * 100 + NormalWpm / 2) / NormalWpm;
                                        o.prosody.speed = static_cast<uint16_t>(
                                                n < Speaker::MinSpeed
                                                        ? Speaker::MinSpeed
                                                        : (n > Speaker::MaxSpeed ? Speaker::MaxSpeed : n));
                                        break;
                                case 'p':
                                        if (!parseNumber(v, Speaker::MinPitch, Speaker::MaxPitch, n)) {
                                                ctx.eprintf("say: -p: percent, %u to %u\n", Speaker::MinPitch,
                                                            Speaker::MaxPitch);
                                                return false;
                                        }
                                        o.prosody.pitch = static_cast<uint16_t>(n);
                                        break;
                                case 'o': o.output = v; break;
                                case 'f': o.file = v; break;
                        }
                }
                o.textAt = at;
                if ((o.raw && o.output == nullptr) || (o.file != nullptr && at < args.size())) {
                        ctx.usage();
                        return false;
                }
                return true;
        }

        Error printVoice(void *arg, const Voices::Found &found) {
                auto &ctx = *static_cast<CommandContext *>(arg);
                // The voice's directory: its text analysis file's path but "/NAME_ta.bin".
                const StringView path = found.analysis.view();
                const size_t     tail = 1 + found.name.size() + StringView("_ta.bin").size();
                const size_t     dir = path.size() > tail ? path.size() - tail : 0;
                ctx.printf("%-8s %.*s\n", found.name.cStr(), static_cast<int>(dir), path.data());
                return ctx.isInterrupted() ? Error::Interrupted : Error::Ok;
        }

        // Where the samples go: the speakers, a file, or stdout.
        class Sink {
                public:
                        explicit Sink(CommandContext &ctx) : _ctx(ctx) {}

                        Error open(const Options &o) {
                                _raw = o.raw;
                                if (o.output == nullptr) return openSpeakers();
                                if (std::strcmp(o.output, "-") == 0) {
                                        _stdout = true;
                                } else {
                                        auto [file, err] =
                                                Vfs::open(StringView(o.output),
                                                          Vfs::Write | Vfs::Create | Vfs::Truncate, _ctx.vfs());
                                        if (err.isError()) return err;
                                        _file = std::move(file);
                                }
                                if (_raw) return Error::Ok;
                                uint8_t header[WavFile::HeaderBytes];
                                (void)WavFile::header(header, SampleFormat::S16Le, 1, Speaker::RateHz,
                                                      WavFile::UnknownLength);
                                return write(ByteSpan(header, sizeof(header)));
                        }

                        Error write(ByteSpan data) {
                                _bytes += data.size();
                                if (_stdout) return _ctx.write(1, data);
                                return _file->write(data, WaitForever).error();
                        }

                        // Played out, or the file's header given its length. @p stopped: Ctrl+C.
                        Error finish(bool stopped) {
                                if (_audio != nullptr) {
                                        if (stopped || _audio->drain() == Error::Interrupted) {
                                                (void)_audio->drop();
                                                return Error::Interrupted;
                                        }
                                        return Error::Ok;
                                }
                                if (!_file || _raw) return Error::Ok;
                                // A file that seeks gets its real sizes; one that doesn't keeps "to the end".
                                uint8_t header[WavFile::HeaderBytes];
                                (void)WavFile::header(header, SampleFormat::S16Le, 1, Speaker::RateHz,
                                                      _bytes - WavFile::HeaderBytes);
                                if (_file->stream().seek(0, Stream::SeekFrom::Start).error().isError())
                                        return Error::Ok;
                                return _file->write(ByteSpan(header, sizeof(header)), WaitForever).error();
                        }

                private:
                        Error openSpeakers() {
                                auto [dsp, err] = Vfs::open(StringView("/dev/dsp"), Vfs::Write, _ctx.vfs());
                                if (err.isError()) return err;
                                _audio = dsp->stream().audioFile();
                                if (_audio == nullptr) return Error::NotSupported;
                                _file = std::move(dsp);
                                AudioFile::Config c;
                                c.format = SampleFormat::S16Le;
                                c.channels = 1;
                                c.rateHz = Speaker::RateHz;
                                c.ringFrames = Speaker::RateHz * RingMs / 1000;
                                c.startFrames = Speaker::RateHz * StartMs / 1000;
                                return _audio->configure(c);
                        }

                        CommandContext &_ctx;
                        FileHandle::Ptr _file;
                        AudioFile      *_audio = nullptr;
                        uint64_t        _bytes = 0;
                        bool            _stdout = false;
                        bool            _raw = false;
        };

        // Where the text comes from: the arguments, a file, or stdin.
        class Source {
                public:
                        Source(CommandContext &ctx, const Options &o) : _ctx(ctx), _o(o) {}

                        Error open() {
                                const Args args = _ctx.args();
                                if (_o.textAt < args.size()) {
                                        for (size_t i = _o.textAt; i < args.size(); ++i) {
                                                Error err = i > _o.textAt ? _text.append(' ') : Error::Ok;
                                                if (err.isOk()) err = _text.append(StringView(args[i]));
                                                if (err.isError()) return err;
                                        }
                                        return Error::Ok;
                                }
                                return _ctx.openInput(StringView(_o.file != nullptr ? _o.file : "-"), _input);
                        }

                        // The next text into @p buf: its bytes, 0 at the end.
                        Result<size_t> next(WritableByteSpan buf) {
                                if (!_input.isOpen()) {
                                        const size_t n =
                                                _text.size() - _at < buf.size() ? _text.size() - _at : buf.size();
                                        std::memcpy(buf.data(), _text.data() + _at, n);
                                        _at += n;
                                        return makeResult(n);
                                }
                                auto [n, err] = _input.read(buf);
                                if (err == Error::EndOfFile) return makeResult(size_t(0));
                                return Result<size_t>(n, err);
                        }

                private:
                        CommandContext       &_ctx;
                        const Options        &_o;
                        String                _text;
                        size_t                _at = 0;
                        CommandContext::Input _input;
        };

        // Speaks the source into the sink. @return `Ok`, `Interrupted`, or the error.
        Error speak(CommandContext &ctx, Work &w, Source &source, Sink &sink) {
                size_t pending = 0; // text read and not yet put, at w.text + putAt
                size_t putAt = 0;
                bool   read = false;  // the source's end reached
                bool   ended = false; // the NUL after it put: everything is said
                for (;;) {
                        if (ctx.isInterrupted()) return Error::Interrupted;
                        if (pending == 0 && !read) {
                                auto [n, err] = source.next(WritableByteSpan(w.text, TextBytes));
                                if (err.isError()) return err;
                                read = n == 0;
                                pending = n;
                                putAt = 0;
                        }
                        if (pending != 0) {
                                auto [n, err] = w.speaker.put(ByteSpan(w.text + putAt, pending));
                                if (err.isError()) return err;
                                putAt += n;
                                pending -= n;
                        } else if (read && !ended) {
                                const uint8_t nul = 0;
                                auto [n, err] = w.speaker.put(ByteSpan(&nul, 1));
                                if (err.isError()) return err;
                                ended = n == 1;
                        }
                        auto [made, rerr] = w.speaker.render(Span<int16_t>(w.samples, BlockSamples));
                        if (rerr.isError()) return rerr;
                        if (made != 0) {
                                const Error err =
                                        sink.write(ByteSpan(reinterpret_cast<const uint8_t *>(w.samples), made * 2));
                                if (err.isError()) return err;
                        } else if (ended) {
                                return Error::Ok;
                        }
                }
        }

        int runSay(CommandContext &ctx) {
                Options o;
                if (!parse(ctx, o)) return Shell::ExitUsage;
                if (o.list) {
                        const Error err = Voices::list(printVoice, &ctx);
                        return err.isOk() ? Shell::ExitSuccess
                                          : (err == Error::Interrupted ? Shell::ExitInterrupted : Shell::ExitFailure);
                }
                auto [voice, verr] = Voices::acquire(StringView(o.voice != nullptr ? o.voice : ""));
                if (verr.isError()) {
                        String name;
                        if (o.voice == nullptr) (void)Voices::defaultVoice(name);
                        ctx.eprintf("say: %s: %s\n", o.voice != nullptr ? o.voice : name.cStr(),
                                    verr == Error::NotExist  ? "no such voice (say -v ? lists them)"
                                    : verr == Error::Invalid ? "not a voice's name"
                                                             : verr.desc());
                        return Shell::ExitFailure;
                }
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Work));
                if (mem == nullptr) {
                        Voices::release(voice);
                        ctx.eprintf("say: %s\n", Error(Error::NoMem).desc());
                        return Shell::ExitFailure;
                }
                Work  *w = ::new (mem) Work();
                Source source(ctx, o);
                Sink   sink(ctx);
                Error  err = w->speaker.open(Voices::files(voice), o.markup, o.prosody);
                if (err.isError()) {
                        ctx.eprintf("say: %.*s: %s\n", static_cast<int>(Voices::name(voice).size()),
                                    Voices::name(voice).data(),
                                    err == Error::CorruptData || err == Error::NotSupported ? "a damaged voice"
                                                                                            : err.desc());
                } else if ((err = source.open()).isError()) {
                        ctx.eprintf("say: %s: %s\n", o.file != nullptr ? o.file : "stdin",
                                    CommandContext::errorText(err));
                } else if ((err = sink.open(o)).isError()) {
                        ctx.eprintf("say: %s: %s\n", o.output != nullptr ? o.output : "/dev/dsp",
                                    CommandContext::errorText(err));
                } else {
                        err = speak(ctx, *w, source, sink);
                        const Error ferr = sink.finish(err == Error::Interrupted);
                        if (err.isOk()) err = ferr;
                        if (err.isError() && err != Error::Interrupted)
                                ctx.eprintf("say: %s\n", CommandContext::errorText(err));
                }
                w->~Work();
                Memory::free(mem);
                Voices::release(voice);
                if (err == Error::Interrupted) return Shell::ExitInterrupted;
                return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
        }

} // namespace

BMDS_SHELL_COMMAND(say, runSay, "[-v VOICE] [-r WPM] [-p PITCH] [--markup] [-o FILE [--raw]] [-f FILE | TEXT...]",
                   "Speak text (TEXT, a file or stdin) on /dev/dsp, or into a WAV file (-o); -v ? lists the voices")

BMDS_NAMESPACE_END
