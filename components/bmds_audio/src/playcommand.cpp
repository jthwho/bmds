/**
 * @file      playcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `play` command: sound files on an audio device, through /dev/dsp as a
 * program's would be (devplan/audio.md §18). WAV now; MP3, Ogg and FLAC in
 * step 4.
 */

#include <cstdio>
#include <cstring>
#include <bmds/audiofile.h>
#include <bmds/command.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>
#include <bmds/wavfile.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr size_t   BufferBytes = 8192;
        constexpr uint32_t RingMs = 500; // a card's pauses, covered

        // Reads exactly what is asked at an offset (WavFile::ReadAt).
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

        // Plays one WAV file on @p dsp: ExitSuccess, ExitFailure (told why), or ExitInterrupted.
        int playFile(CommandContext &ctx, FileHandle &dsp, AudioFile &audio, const char *path, uint8_t *buf) {
                auto [file, oerr] = Vfs::open(StringView(path), Vfs::Read, ctx.vfs());
                if (oerr.isError()) {
                        ctx.eprintf("play: %s: %s\n", path, oerr.desc());
                        return Shell::ExitFailure;
                }
                auto [size, zerr] = file->stream().seek(0, Stream::SeekFrom::End);
                auto [info, werr] =
                        zerr.isOk() ? WavFile::parse(readAt, file.get(), size) : makeError<WavFile::Info>(zerr);
                if (werr.isError()) {
                        const char *why = werr == Error::Invalid        ? "not a WAV file"
                                          : werr == Error::NotSupported ? "a WAV file in a form this doesn't play"
                                                                        : werr.desc();
                        ctx.eprintf("play: %s: %s\n", path, why);
                        return Shell::ExitFailure;
                }
                const size_t   frameBytes = SampleFormats::bytesPerSample(info.format) * info.channels;
                const uint64_t frames = info.dataBytes / frameBytes;
                const uint64_t tenths = info.rateHz == 0 ? 0 : frames * 10 / info.rateHz;
                ctx.printf("%s: %lu Hz, %s, %s, %lu:%02lu.%lu\n", path, static_cast<unsigned long>(info.rateHz),
                           SampleFormats::name(info.format), info.channels == 1 ? "mono" : "stereo",
                           static_cast<unsigned long>(tenths / 600), static_cast<unsigned long>(tenths / 10 % 60),
                           static_cast<unsigned long>(tenths % 10));
                AudioFile::Config c;
                c.format = info.format;
                c.channels = info.channels;
                c.rateHz = info.rateHz;
                c.ringFrames = info.rateHz * RingMs / 1000;
                Error err = audio.configure(c); // the file before plays out first
                if (err == Error::Interrupted) return Shell::ExitInterrupted;
                if (err.isError()) {
                        ctx.eprintf("play: %s: %s\n", path,
                                    err == Error::Invalid ? "a rate this doesn't play" : err.desc());
                        return Shell::ExitFailure;
                }
                err = file->stream().seek(static_cast<int64_t>(info.dataOffset), Stream::SeekFrom::Start).error();
                for (uint64_t left = info.dataBytes; err.isOk() && left > 0;) {
                        const size_t want = left < BufferBytes ? static_cast<size_t>(left) : BufferBytes;
                        auto [n, rerr] = file->read(WritableByteSpan(buf, want), WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break; // shorter than it said
                        if (rerr.isError()) {
                                err = rerr;
                                break;
                        }
                        err = dsp.write(ByteSpan(buf, n), WaitForever).error();
                        left -= n;
                }
                if (err == Error::Interrupted) return Shell::ExitInterrupted;
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
                   "Play sound files (WAV) on /dev/dsp, or on device dspN; Ctrl+C stops")

BMDS_NAMESPACE_END
