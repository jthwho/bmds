/**
 * @file      audiocalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The audio system calls (target only, devplan/audio.md §18): the ABI's
 * structs to and from an fd's AudioFile.
 */

#include <cstdint>
#include <cstring>
#include <bmds/audiodevice.h>
#include <bmds/audiofile.h>
#include <bmds/errno.h>
#include <bmds/sys.h>
#include "running.h"

using bmds::AudioFile;
using bmds::Error;
using bmds::KernelCall;
using bmds::Process;
using bmds::SampleFormat;

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

        // Copies the caller's struct (its `size` first) into a zeroed full one.
        template <typename T> int32_t copyIn(const T *in, T &full) {
                full = T{};
                if (in == nullptr) return -BMDS_EFAULT;
                const uint32_t size = in->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                std::memcpy(&full, in, size < sizeof(T) ? size : sizeof(T));
                return 0;
        }

        // Copies the fields of @p full that fit in the caller's struct, leaving its `size`.
        template <typename T> int32_t copyOut(const T &full, T *out) {
                if (out == nullptr) return -BMDS_EFAULT;
                const uint32_t size = out->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                const size_t n = size < sizeof(T) ? size : sizeof(T);
                std::memcpy(reinterpret_cast<uint8_t *>(out) + sizeof(uint32_t),
                            reinterpret_cast<const uint8_t *>(&full) + sizeof(uint32_t), n - sizeof(uint32_t));
                return 0;
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
                if (c.flags != 0 || c.format >= bmds::SampleFormats::Count || c.channels > 255) return -BMDS_EINVAL;
                AudioFile::Config k;
                k.format = static_cast<SampleFormat>(c.format);
                k.channels = static_cast<uint8_t>(c.channels);
                k.rateHz = c.rate;
                k.ringFrames = c.ring_frames;
                k.startFrames = c.start_frames;
                k.wakeFrames = c.wake_frames;
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
        return copyOut(c, got);
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

} // extern "C"
