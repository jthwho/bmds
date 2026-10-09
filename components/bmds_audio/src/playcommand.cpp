/**
 * @file      playcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `play` command: sound files on an audio device, through /dev/dsp as a
 * program's would be (devplan/audio.md §18): WAV, MP3, Ogg Vorbis and FLAC,
 * through `Decoder` (§19).
 */

#include <cstdio>
#include <cstring>
#include <bmds/audiofile.h>
#include <bmds/command.h>
#include <bmds/decoder.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr size_t   BufferBytes = 8192;
        constexpr uint32_t RingMs = 500; // a card's pauses, covered

        // @p frames at @p rate as M:SS.T (unknown: -:--.-).
        struct Time {
                        char text[24];
        };
        Time timeOf(uint64_t frames, uint32_t rate) {
                Time t;
                if (frames == Decoder::UnknownFrames || rate == 0) {
                        std::snprintf(t.text, sizeof(t.text), "-:--.-");
                        return t;
                }
                const uint64_t tenths = frames * 10 / rate;
                std::snprintf(t.text, sizeof(t.text), "%lu:%02lu.%lu", static_cast<unsigned long>(tenths / 600),
                              static_cast<unsigned long>(tenths / 10 % 60), static_cast<unsigned long>(tenths % 10));
                return t;
        }

        // Reads exactly what is asked at an offset (Decoder::ReadAt).
        Error readAt(void *arg, uint64_t offset, WritableByteSpan buf) {
                FileHandle &file = *static_cast<FileHandle *>(arg);
                auto [at, serr] = file.stream().seek(static_cast<int64_t>(offset), Stream::SeekFrom::Start);
                if (serr.isError()) return serr;
                size_t done = 0;
                while (done < buf.size()) {
                        auto [n, err] = file.read(buf.mid(done), WaitForever);
                        if (err == Error::EndOfFile || (err.isOk() && n == 0)) return Error::EndOfFile;
                        if (err.isError()) return err;
                        done += n;
                }
                return Error::Ok;
        }

        // Plays one sound file on @p dsp: ExitSuccess, ExitFailure (told why), or ExitInterrupted.
        int playFile(CommandContext &ctx, FileHandle &dsp, AudioFile &audio, const char *path, uint8_t *buf) {
                auto [file, oerr] = Vfs::open(StringView(path), Vfs::Read, ctx.vfs());
                if (oerr.isError()) {
                        ctx.eprintf("play: %s: %s\n", path, oerr.desc());
                        return Shell::ExitFailure;
                }
                auto [size, zerr] = file->stream().seek(0, Stream::SeekFrom::End);
                auto [d, derr] = zerr.isOk() ? Decoder::open({readAt, file.get(), size}) : makeError<Decoder *>(zerr);
                if (derr.isError()) {
                        const char *why = derr == Error::Invalid        ? "not a sound file this plays"
                                          : derr == Error::NotSupported ? "a sound file in a form this doesn't play"
                                          : derr == Error::CorruptData  ? "a damaged file"
                                                                        : derr.desc();
                        ctx.eprintf("play: %s: %s\n", path, why);
                        return Shell::ExitFailure;
                }
                const Decoder::Info info = d->info(); // a copy: used after the close
                // The decoder's own format, so nothing is quantized twice. WAV names its samples' format.
                const char *format =
                        info.codec == Codec::Wav ? SampleFormats::name(info.format) : Decoder::name(info.codec);
                ctx.printf("%s: %lu Hz, %s, %s, %s\n", path, static_cast<unsigned long>(info.rateHz), format,
                           info.channels == 1 ? "mono" : "stereo", timeOf(info.frames, info.rateHz).text);
                const StringView title = d->tag("TITLE");
                const StringView artist = d->tag("ARTIST");
                if (!title.isEmpty() || !artist.isEmpty())
                        ctx.printf("  %.*s%s%.*s\n", static_cast<int>(artist.size()), artist.data(),
                                   !title.isEmpty() && !artist.isEmpty() ? " - " : "", static_cast<int>(title.size()),
                                   title.data());
                AudioFile::Config c;
                c.format = info.format;
                c.channels = info.channels;
                c.rateHz = info.rateHz;
                c.ringFrames = info.rateHz * RingMs / 1000;
                Error err = audio.configure(c); // the file before plays out first
                if (err == Error::Interrupted) {
                        Decoder::close(d);
                        return Shell::ExitInterrupted;
                }
                if (err.isError()) {
                        ctx.eprintf("play: %s: %s\n", path,
                                    err == Error::Invalid ? "a rate this doesn't play" : err.desc());
                        Decoder::close(d);
                        return Shell::ExitFailure;
                }
                for (;;) {
                        auto [n, rerr] = d->decode(WritableByteSpan(buf, BufferBytes));
                        if (n > 0) err = dsp.write(ByteSpan(buf, n), WaitForever).error();
                        if (err.isError()) break;
                        if (rerr == Error::EndOfFile) break;
                        if (rerr.isError()) {
                                err = rerr;
                                break;
                        }
                }
                const uint64_t at = d->position();
                Decoder::close(d);
                if (err == Error::Interrupted) return Shell::ExitInterrupted;
                if (err == Error::CorruptData) {
                        ctx.eprintf("play: %s: a damaged file at %s\n", path, timeOf(at, info.rateHz).text);
                        return Shell::ExitFailure;
                }
                if (err.isError()) {
                        ctx.eprintf("play: %s: %s\n", path, err.desc());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        int runPlay(CommandContext &ctx) {
                const Args args = ctx.args();
                size_t     at = 1;
                char       device[16] = "/dev/dsp";
                if (at + 1 < args.size() && std::strcmp(args[at], "-d") == 0) {
                        const char *name = args[at + 1];
                        if (std::strncmp(name, "/dev/", 5) == 0) name += 5;
                        if (std::strncmp(name, "dsp", 3) != 0 || name[3] < '0' || name[3] > '9' ||
                            std::strlen(name) > 5)
                                return ctx.usage();
                        std::snprintf(device, sizeof(device), "/dev/%s", name);
                        at += 2;
                }
                if (at == args.size()) return ctx.usage();
                auto [dsp, derr] = Vfs::open(StringView(device), Vfs::Write, ctx.vfs());
                AudioFile *audio = derr.isOk() ? dsp->stream().audioFile() : nullptr;
                if (audio == nullptr) {
                        ctx.eprintf("play: %s: %s\n", device, derr.isError() ? derr.desc() : "not an audio device");
                        return Shell::ExitFailure;
                }
                auto *buf = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, BufferBytes));
                if (buf == nullptr) {
                        ctx.eprintf("play: %s\n", Error(Error::NoMem).desc());
                        return Shell::ExitFailure;
                }
                int status = Shell::ExitSuccess;
                for (; at < args.size(); ++at) {
                        const int s = playFile(ctx, *dsp, *audio, args[at], buf);
                        if (s == Shell::ExitInterrupted) {
                                status = s;
                                break;
                        }
                        if (s != Shell::ExitSuccess) status = Shell::ExitFailure;
                }
                // Ctrl+C stops at once; otherwise the close waits until the last file has played.
                if (status == Shell::ExitInterrupted || audio->drain() == Error::Interrupted) {
                        (void)audio->drop();
                        status = Shell::ExitInterrupted;
                }
                Memory::free(buf);
                return status;
        }

} // namespace

BMDS_SHELL_COMMAND(play, runPlay, "[-d dspN] FILE...",
                   "Play sound files (WAV, MP3, Ogg Vorbis, FLAC) on /dev/dsp, or on device dspN; Ctrl+C stops")

BMDS_NAMESPACE_END
