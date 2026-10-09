/**
 * @file      audiocalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The audio system calls (target only, devplan/audio.md §18, §19): the ABI's
 * structs to and from an fd's AudioFile or DecodedFile.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bmds/audiodevice.h>
#include <bmds/audiofile.h>
#include <bmds/decodedfile.h>
#include <bmds/errno.h>
#include <bmds/synthfile.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::AudioFile;
using bmds::Codec;
using bmds::DecodedFile;
using bmds::Decoder;
using bmds::Error;
using bmds::KernelCall;
using bmds::Process;
using bmds::SampleFormat;
using bmds::Synth;
using bmds::SynthFile;
using bmds::detail::copyIn;
using bmds::detail::copyOut;
using bmds::detail::copyText;

namespace {

        static_assert(BMDS_AUDIO_U8 == static_cast<uint32_t>(SampleFormat::U8) &&
                              BMDS_AUDIO_S8 == static_cast<uint32_t>(SampleFormat::S8) &&
                              BMDS_AUDIO_S16LE == static_cast<uint32_t>(SampleFormat::S16Le) &&
                              BMDS_AUDIO_S16BE == static_cast<uint32_t>(SampleFormat::S16Be) &&
                              BMDS_AUDIO_S24LE == static_cast<uint32_t>(SampleFormat::S24Le) &&
                              BMDS_AUDIO_S24_3LE == static_cast<uint32_t>(SampleFormat::S24_3Le) &&
                              BMDS_AUDIO_S32LE == static_cast<uint32_t>(SampleFormat::S32Le) &&
                              BMDS_AUDIO_F32LE == static_cast<uint32_t>(SampleFormat::F32Le) &&
                              BMDS_AUDIO_MULAW == static_cast<uint32_t>(SampleFormat::MuLaw) &&
                              BMDS_AUDIO_ALAW == static_cast<uint32_t>(SampleFormat::ALaw) &&
                              bmds::SampleFormats::Count == BMDS_AUDIO_ALAW + 1,
                      "the ABI's formats are SampleFormat's");
        static_assert(BMDS_AUDIO_IDLE == static_cast<uint32_t>(AudioFile::State::Idle) &&
                              BMDS_AUDIO_WAITING == static_cast<uint32_t>(AudioFile::State::Waiting) &&
                              BMDS_AUDIO_PLAYING == static_cast<uint32_t>(AudioFile::State::Playing) &&
                              BMDS_AUDIO_PAUSED == static_cast<uint32_t>(AudioFile::State::Paused) &&
                              BMDS_AUDIO_DRAINING == static_cast<uint32_t>(AudioFile::State::Draining) &&
                              BMDS_AUDIO_MIXER == static_cast<uint32_t>(AudioFile::State::Mixer),
                      "the ABI's states are AudioFile's");
        static_assert(BMDS_SOUND_BELL == static_cast<uint32_t>(bmds::SystemSound::Bell) &&
                              BMDS_SOUND_CLICK == static_cast<uint32_t>(bmds::SystemSound::Click) &&
                              BMDS_SOUND_ERROR == static_cast<uint32_t>(bmds::SystemSound::Error) &&
                              BMDS_SOUND_NOTIFY == static_cast<uint32_t>(bmds::SystemSound::Notify) &&
                              bmds::SystemSoundCount == BMDS_SOUND_NOTIFY + 1,
                      "the ABI's sounds are SystemSound's");
        static_assert(BMDS_AUDIO_GAIN_MUTED == AudioFile::MutedCentiDb, "the ABI's silent gain");
        static_assert(BMDS_AUDIO_REALTIME == AudioFile::Realtime && BMDS_AUDIO_EXCLUSIVE == AudioFile::Exclusive,
                      "the ABI's flags are AudioFile's");
        static_assert(BMDS_CODEC_WAV == static_cast<uint32_t>(Codec::Wav) &&
                              BMDS_CODEC_MP3 == static_cast<uint32_t>(Codec::Mp3) &&
                              BMDS_CODEC_VORBIS == static_cast<uint32_t>(Codec::Vorbis) &&
                              BMDS_CODEC_FLAC == static_cast<uint32_t>(Codec::Flac) &&
                              BMDS_CODEC_MIDI == static_cast<uint32_t>(Codec::Midi),
                      "the ABI's codecs are Codec's");
        static_assert(BMDS_DECODE_UNKNOWN_FRAMES == Decoder::UnknownFrames, "the ABI's unknown length");
        static_assert(sizeof(bmds_synth_event_t) == sizeof(Synth::Event) &&
                              offsetof(bmds_synth_event_t, frame) == offsetof(Synth::Event, frame) &&
                              offsetof(bmds_synth_event_t, length) == offsetof(Synth::Event, length) &&
                              offsetof(bmds_synth_event_t, flags) == offsetof(Synth::Event, flags) &&
                              offsetof(bmds_synth_event_t, data) == offsetof(Synth::Event, data) &&
                              sizeof(bmds_synth_event_t::data) == Synth::MaxEventBytes,
                      "bmds_synth_event_t is Synth::Event");
        static_assert(BMDS_SYNTH_QUEUE == Synth::QueueEvents, "the ABI's queue length");

        // The synth fd behind @p handle (an fd's), or the errno.
        int32_t synthFileOf(const bmds::FileHandle::Ptr &handle, SynthFile *&file) {
                if (!handle) return -BMDS_EBADF;
                file = handle->stream().synthFile();
                return file != nullptr ? 0 : -BMDS_ENOTTY;
        }

        // The decoded file behind @p handle (an fd's), or the errno.
        int32_t decodedFileOf(const bmds::FileHandle::Ptr &handle, DecodedFile *&file) {
                if (!handle) return -BMDS_EBADF;
                file = handle->stream().decodedFile();
                return file != nullptr ? 0 : -BMDS_ENOTTY;
        }

        // The audio file behind @p handle (an fd's), or the errno.
        int32_t audioFileOf(const bmds::FileHandle::Ptr &handle, AudioFile *&file) {
                if (!handle) return -BMDS_EBADF;
                file = handle->stream().audioFile();
                return file != nullptr ? 0 : -BMDS_ENOTTY;
        }

} // namespace

extern "C" {

int32_t bmds_k_audio_config(int32_t fd, const bmds_audio_config_t *set, bmds_audio_config_t *got) {
        KernelCall            call;
        bmds::FileHandle::Ptr handle = call->handle(fd);
        AudioFile            *file = nullptr;
        int32_t               r = audioFileOf(handle, file);
        if (r < 0) return r;
        if (set != nullptr) {
                bmds_audio_config_t c;
                r = copyIn(set, c);
                if (r < 0) return r;
                if ((c.flags & ~(BMDS_AUDIO_REALTIME | BMDS_AUDIO_EXCLUSIVE)) != 0 ||
                    c.format >= bmds::SampleFormats::Count || c.channels > 255)
                        return -BMDS_EINVAL;
                AudioFile::Config k;
                k.format = static_cast<SampleFormat>(c.format);
                k.channels = static_cast<uint8_t>(c.channels);
                k.rateHz = c.rate;
                k.ringFrames = c.ring_frames;
                k.startFrames = c.start_frames;
                k.wakeFrames = c.wake_frames;
                k.flags = c.flags;
                r = Process::errnoFor(file->configure(k));
                if (r < 0) return r;
        }
        if (got == nullptr) return 0;
        const AudioFile::Config k = file->config();
        bmds_audio_config_t     c = {};
        c.format = static_cast<uint32_t>(k.format);
        c.channels = k.channels;
        c.rate = k.rateHz;
        c.ring_frames = k.ringFrames;
        c.start_frames = k.startFrames;
        c.wake_frames = k.wakeFrames;
        c.flags = k.flags;
        return copyOut(c, got);
}

int32_t bmds_k_audio_cycle(int32_t fd, bmds_audio_cycle_t *cycle) {
        KernelCall            call;
        bmds::FileHandle::Ptr handle = call->handle(fd);
        AudioFile            *file = nullptr;
        int32_t               r = audioFileOf(handle, file);
        if (r < 0) return r;
        if (cycle != nullptr && cycle->size < sizeof(uint32_t)) return -BMDS_EINVAL;
        // Raised from its first period: the device must forget it before its task is deleted.
        if (cycle != nullptr) bmds::ProgramThread::self().realtime = true;
        bmds::RtStream::Period p;
        const Error            err = file->cycle(cycle != nullptr ? &p : nullptr);
        if (err.isError()) return Process::errnoFor(err);
        if (cycle == nullptr) return 0;
        bmds_audio_cycle_t c = {};
        c.frames = p.frames;
        c.channels = p.channels;
        c.rate = p.rateHz;
        c.out = p.out;
        c.in = p.in;
        c.position = p.position;
        c.deadline_ns = p.deadlineNs;
        c.play_ns = p.playNs;
        c.skipped = p.skipped;
        c.late = p.late;
        c.throttled = p.throttled;
        return copyOut(c, cycle);
}

int32_t bmds_k_audio_status(int32_t fd, bmds_audio_status_t *status) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        AudioFile                  *file = nullptr;
        const int32_t               r = audioFileOf(handle, file);
        if (r < 0) return r;
        if (status == nullptr) return -BMDS_EFAULT;
        const AudioFile::Status k = file->status();
        bmds_audio_status_t     s = {};
        s.state = static_cast<uint32_t>(k.state);
        s.device = k.device;
        s.device_rate = k.deviceRate;
        s.period_frames = k.periodFrames;
        s.formats = (1u << bmds::SampleFormats::Count) - 1;
        s.min_rate = bmds::PcmStream::MinRateHz;
        s.max_rate = bmds::PcmStream::MaxRateHz;
        s.max_channels = 2;
        s.ring_frames = k.ringFrames;
        s.written_frames = k.writtenFrames;
        s.played_frames = k.playedFrames;
        s.timestamp_ns = k.timestampNs;
        s.queued_frames = k.queuedFrames;
        s.delay_frames = k.delayFrames;
        s.underruns = k.underruns;
        s.gain_cdb = k.gainCentiDb;
        s.late_periods = k.latePeriods;
        s.limited_frames = k.limitedFrames;
        return copyOut(s, status);
}

int32_t bmds_k_audio_control(int32_t fd, uint32_t op, int32_t value) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        AudioFile                  *file = nullptr;
        const int32_t               r = audioFileOf(handle, file);
        if (r < 0) return r;
        if (op != BMDS_AUDIO_GAIN && value != 0) return -BMDS_EINVAL;
        Error err = Error::Invalid;
        switch (op) {
                case BMDS_AUDIO_START: err = file->start(); break;
                case BMDS_AUDIO_PAUSE: err = file->setPaused(true); break;
                case BMDS_AUDIO_RESUME: err = file->setPaused(false); break;
                case BMDS_AUDIO_DRAIN: err = file->drain(); break;
                case BMDS_AUDIO_DROP: err = file->drop(); break;
                case BMDS_AUDIO_GAIN: err = file->setGain(value); break;
                default: break;
        }
        return Process::errnoFor(err);
}

int32_t bmds_k_sound_play(uint32_t sound) {
        KernelCall call;
        if (sound >= bmds::SystemSoundCount) return -BMDS_EINVAL;
        (void)bmds::SystemSounds::play(static_cast<bmds::SystemSound>(sound));
        return 0;
}

int32_t bmds_k_audio_decode(int32_t fd, const bmds_decode_config_t *config) {
        KernelCall           call;
        bmds_decode_config_t c;
        int32_t              r = copyIn(config, c);
        if (r < 0) return r;
        if ((c.flags & ~BMDS_O_CLOEXEC) != 0) return -BMDS_EINVAL;
        const bool native = c.format == BMDS_DECODE_NATIVE;
        if (!native && c.format != BMDS_AUDIO_S16LE && c.format != BMDS_AUDIO_S32LE && c.format != BMDS_AUDIO_F32LE)
                return -BMDS_EINVAL;
        const auto                          format = native ? SampleFormat::S16Le : static_cast<SampleFormat>(c.format);
        bmds::Result<bmds::FileHandle::Ptr> opened = bmds::makeError<bmds::FileHandle::Ptr>(Error::BadDescriptor);
        if (fd == -1) {
                if (c.data == nullptr && c.length != 0) return -BMDS_EFAULT;
                opened = DecodedFile::open(bmds::ByteSpan(static_cast<const uint8_t *>(c.data), c.length), format,
                                           native);
        } else {
                bmds::FileHandle::Ptr source = call->handle(fd);
                if (!source || !source->canRead()) return -BMDS_EBADF;
                opened = DecodedFile::open(source, format, native);
        }
        if (opened.error().isError()) return Process::errnoFor(opened.error());
        return call->addHandle(std::move(opened.value()), c.flags);
}

int32_t bmds_k_audio_decode_info(int32_t fd, bmds_decode_info_t *info) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        DecodedFile                *file = nullptr;
        const int32_t               r = decodedFileOf(handle, file);
        if (r < 0) return r;
        if (info == nullptr) return -BMDS_EFAULT;
        const Decoder       &d = file->decoder();
        const Decoder::Info &k = d.info();
        bmds_decode_info_t   i = {};
        i.codec = static_cast<uint32_t>(k.codec);
        copyText(Decoder::name(k.codec), i.name, sizeof(i.name));
        i.format = static_cast<uint32_t>(d.format());
        i.channels = k.channels;
        i.rate = k.rateHz;
        i.bitrate = k.bitrate;
        i.frames = k.frames;
        i.flags = k.estimated ? BMDS_DECODE_ESTIMATED : 0;
        i.source_bits = k.sourceBits;
        i.tags = static_cast<uint32_t>(d.tagCount());
        i.native_format = static_cast<uint32_t>(k.format);
        i.frame_bytes = static_cast<uint32_t>(d.frameBytes());
        return copyOut(i, info);
}

int32_t bmds_k_audio_decode_tag(int32_t fd, uint32_t index, char *key, uint32_t key_size, char *value,
                                uint32_t value_size) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        DecodedFile                *file = nullptr;
        const int32_t               r = decodedFileOf(handle, file);
        if (r < 0) return r;
        if ((key == nullptr && key_size != 0) || (value == nullptr && value_size != 0)) return -BMDS_EFAULT;
        if (index >= file->decoder().tagCount()) return -BMDS_ENOENT;
        const Decoder::Tag t = file->decoder().tag(index);
        copyText(t.key, key, key_size);
        copyText(t.value, value, value_size);
        return static_cast<int32_t>(t.value.size());
}

int32_t bmds_k_audio_synth(const bmds_synth_config_t *config) {
        KernelCall          call;
        bmds_synth_config_t c;
        int32_t             r = copyIn(config, c);
        if (r < 0) return r;
        // Checked here, so that Invalid from the open is the font's (damaged).
        const uint32_t rate = c.rate != 0 ? c.rate : 48000;
        const uint32_t channels = c.channels != 0 ? c.channels : 2;
        if ((c.flags & ~BMDS_O_CLOEXEC) != 0 || rate < 8000 || rate > 96000 || channels > 2 ||
            (c.format != BMDS_AUDIO_F32LE && c.format != BMDS_AUDIO_S16LE) || c.max_voices > Synth::MaxVoices)
                return -BMDS_EINVAL;
        bmds::String font;
        if (c.font != nullptr) {
                r = call->absolutePath(c.font, font);
                if (r < 0) return r;
        }
        SynthFile::Config k;
        k.rateHz = rate;
        k.channels = static_cast<uint8_t>(channels);
        k.format = static_cast<SampleFormat>(c.format);
        k.maxVoices = static_cast<uint16_t>(c.max_voices);
        k.font = font.view();
        auto [handle, err] = SynthFile::open(k, bmds::Vfs::Context());
        if (err == Error::Invalid) return -BMDS_EIO; // a damaged font
        if (err.isError()) return Process::errnoFor(err);
        return call->addHandle(std::move(handle), c.flags);
}

int32_t bmds_k_audio_synth_info(int32_t fd, bmds_synth_info_t *info) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        SynthFile                  *file = nullptr;
        const int32_t               r = synthFileOf(handle, file);
        if (r < 0) return r;
        if (info == nullptr) return -BMDS_EFAULT;
        const SynthFile::Info k = file->info();
        bmds_synth_info_t     i = {};
        i.rate = k.config.rateHz;
        i.channels = k.config.channels;
        i.format = static_cast<uint32_t>(k.config.format);
        i.frames = k.synth.frames;
        i.voices = k.synth.voices;
        i.max_voices = k.synth.maxVoices;
        i.peak_voices = k.synth.peakVoices;
        i.notes_at_limit = k.synth.notesAtLimit;
        i.queued = k.synth.queued;
        i.queue_full = k.synth.queueFull;
        i.ignored = k.synth.ignored;
        i.presets = static_cast<uint32_t>(k.presets);
        i.font_bytes = static_cast<uint32_t>(k.fontBytes);
        i.cpu_permille = k.cpuPermille;
        i.cpu_permille_max = k.cpuPermilleMax;
        copyText(file->fontPath(), i.font, sizeof(i.font));
        return copyOut(i, info);
}

} // extern "C"
