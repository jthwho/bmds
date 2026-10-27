/**
 * @file      oss.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SDK's OSS translation (sdk/crt/oss.c, devplan/audio.md §18), run
 * against a fake system call table: what each ioctl asks of the native
 * audio calls and the settings, and what it answers.
 */

#include <doctest.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <sys/soundcard.h>
#include <bmds/errno.h>
#include <bmds/sys.h>
#include "fakefonts.h"

extern "C" {
long __bmds_oss_ioctl(long fd, unsigned long request, void *arg);
long __bmds_oss_room(long fd);
void __bmds_oss_closed(long fd);
}

namespace {

        constexpr int32_t AudioFd = 5;
        constexpr int32_t MixerFd = 6;

        // The fake kernel: one audio fd, one mixer fd, the settings.
        struct Fake {
                        bmds_audio_config_t                config;
                        bmds_audio_status_t                status;
                        std::vector<uint32_t>              ops;
                        std::map<std::string, std::string> settings;
                        std::vector<std::string>           keys; // ids: index into this
        } fake;

        int32_t audioConfig(int32_t fd, const bmds_audio_config_t *set, bmds_audio_config_t *got) {
                if (fd != AudioFd) return -BMDS_ENOTTY;
                if (set != nullptr) {
                        if (set->format > BMDS_AUDIO_ALAW || set->channels < 1 || set->channels > 2)
                                return -BMDS_EINVAL;
                        fake.config = *set;
                        // As the kernel: defaults filled in, the ring a power of two.
                        if (fake.config.ring_frames == 0) fake.config.ring_frames = fake.config.rate / 10;
                        uint32_t p = 1;
                        while (p < fake.config.ring_frames) p <<= 1;
                        fake.config.ring_frames = p;
                        if (fake.config.wake_frames == 0) fake.config.wake_frames = 240;
                        fake.status.ring_frames = p;
                }
                if (got != nullptr) *got = fake.config;
                return 0;
        }

        int32_t audioStatus(int32_t fd, bmds_audio_status_t *status) {
                if (fd == MixerFd) {
                        *status = bmds_audio_status_t{};
                        status->size = sizeof(*status);
                        status->state = BMDS_AUDIO_MIXER;
                        return 0;
                }
                if (fd != AudioFd) return -BMDS_ENOTTY;
                *status = fake.status;
                return 0;
        }

        int32_t audioControl(int32_t fd, uint32_t op, int32_t) {
                if (fd != AudioFd) return -BMDS_ENOTTY;
                fake.ops.push_back(op);
                return 0;
        }

        int32_t sysconfFind(const char *key) {
                for (size_t i = 0; i < fake.keys.size(); ++i)
                        if (fake.keys[i] == key) return static_cast<int32_t>(i);
                return -BMDS_ENOENT;
        }

        int32_t sysconfGet(int32_t id, uint32_t field, char *out, uint32_t size) {
                if (id < 0 || static_cast<size_t>(id) >= fake.keys.size() || field != BMDS_SYSCONF_FIELD_VALUE)
                        return -BMDS_ENOENT;
                const std::string &v = fake.settings[fake.keys[id]];
                if (v.size() + 1 > size) return -BMDS_ERANGE;
                std::memcpy(out, v.c_str(), v.size() + 1);
                return static_cast<int32_t>(v.size());
        }

        int32_t sysconfSet(int32_t id, uint32_t op, const char *value) {
                if (id < 0 || static_cast<size_t>(id) >= fake.keys.size() || op != BMDS_SYSCONF_OP_SET)
                        return -BMDS_EINVAL;
                fake.settings[fake.keys[id]] = value;
                return 0;
        }

        struct Table {
                        uint32_t      magic, abi_version, count, reserved;
                        bmds_sys_fn_t slot[BMDS_SYS_FONT_CLOSE + 1]; // the fonts: libbmdsgame's text
        };

        Table makeTable() {
                Table t = {BMDS_SYS_MAGIC, BMDS_ABI_VERSION, BMDS_SYS_FONT_CLOSE + 1, 0, {}};
                t.slot[BMDS_SYS_AUDIO_CONFIG] = reinterpret_cast<bmds_sys_fn_t>(audioConfig);
                t.slot[BMDS_SYS_AUDIO_STATUS] = reinterpret_cast<bmds_sys_fn_t>(audioStatus);
                t.slot[BMDS_SYS_AUDIO_CONTROL] = reinterpret_cast<bmds_sys_fn_t>(audioControl);
                t.slot[BMDS_SYS_SYSCONF_FIND] = reinterpret_cast<bmds_sys_fn_t>(sysconfFind);
                t.slot[BMDS_SYS_SYSCONF_GET] = reinterpret_cast<bmds_sys_fn_t>(sysconfGet);
                t.slot[BMDS_SYS_SYSCONF_SET] = reinterpret_cast<bmds_sys_fn_t>(sysconfSet);
                FakeFonts::install(t.slot);
                return t;
        }

        Table table = makeTable();

        // A fresh fake: /dev/dsp just opened (8 kHz u8 mono, as the kernel opens it).
        void reset() {
                fake = Fake{};
                fake.config = {sizeof(bmds_audio_config_t), BMDS_AUDIO_U8, 1, 8000, 1024, 512, 40, 0};
                fake.status.size = sizeof(fake.status);
                fake.status.formats = 0x3ff;
                fake.status.min_rate = 4000;
                fake.status.max_rate = 192000;
                fake.status.max_channels = 2;
                fake.status.ring_frames = 1024;
                fake.keys = {"audio/dsp0/volume", "audio/dsp0/mute"};
                fake.settings = {{"audio/dsp0/volume", "-9"}, {"audio/dsp0/mute", "false"}};
                __bmds_oss_closed(AudioFd);
        }

        long ioctlInt(int32_t fd, unsigned long request, int &value) {
                return __bmds_oss_ioctl(fd, request, &value);
        }

} // namespace

extern "C" {
const bmds_sys_t *bmds_sys = reinterpret_cast<const bmds_sys_t *>(&table);
}

TEST_CASE("OSS: requests on an fd that isn't audio fail as ioctls do") {
        reset();
        int v = 0;
        CHECK(ioctlInt(3, SNDCTL_DSP_SPEED, v) == -ENOTTY);
        CHECK(__bmds_oss_room(3) == -1);
        CHECK(ioctlInt(AudioFd, _IOR('P', 99, int), v) == -EINVAL);
        CHECK(ioctlInt(AudioFd, OSS_GETVERSION, v) == 0);
        CHECK(v == SOUND_VERSION);
}

TEST_CASE("OSS: format, channels and rate, answered with what was chosen") {
        reset();
        int v = AFMT_S16_LE;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETFMT, v) == 0);
        CHECK(v == AFMT_S16_LE);
        CHECK(fake.config.format == BMDS_AUDIO_S16LE);
        v = 0x200; // AFMT_MPEG: not taken, the format stays
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETFMT, v) == 0);
        CHECK(v == AFMT_S16_LE);
        v = AFMT_QUERY;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETFMT, v) == 0);
        CHECK(v == AFMT_S16_LE);
        v = 6;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_CHANNELS, v) == 0);
        CHECK(v == 2);
        v = 0;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_STEREO, v) == 0);
        CHECK(v == 0);
        CHECK(fake.config.channels == 1);
        v = 44100;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SPEED, v) == 0);
        CHECK(v == 44100);
        CHECK(fake.config.rate == 44100);
        v = 1000000; // to the most taken
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SPEED, v) == 0);
        CHECK(v == 192000);
        v = 0;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_GETFMTS, v) == 0);
        CHECK(v == (AFMT_MU_LAW | AFMT_A_LAW | AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8));
}

TEST_CASE("OSS: fragments size the ring and the wake mark; the space and delay in bytes") {
        reset();
        int v = AFMT_S16_LE;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SETFMT, v) == 0);
        v = 2;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_CHANNELS, v) == 0);
        v = 48000;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SPEED, v) == 0);
        v = 0x0004000c; // 4 fragments of 4096 bytes: 1024 frames each
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETFRAGMENT, v) == 0);
        CHECK(fake.config.wake_frames == 1024);
        CHECK(fake.config.ring_frames == 4096);
        v = 0;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_GETBLKSIZE, v) == 0);
        CHECK(v == 4096);
        fake.status.queued_frames = 1500;
        fake.status.delay_frames = 2000;
        audio_buf_info info = {};
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_GETOSPACE, &info) == 0);
        CHECK(info.fragsize == 4096);
        CHECK(info.fragstotal == 4);
        CHECK(info.bytes == (4096 - 1500) * 4);
        CHECK(info.fragments == (4096 - 1500) * 4 / 4096);
        CHECK(__bmds_oss_room(AudioFd) == (4096 - 1500) * 4);
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_GETODELAY, v) == 0);
        CHECK(v == 8000);
        fake.status.played_frames = 5000;
        count_info ci = {};
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_GETOPTR, &ci) == 0);
        CHECK(ci.bytes == 20000);
        CHECK(ci.blocks == 4);
        CHECK(ci.ptr == (5000 % 4096) * 4);
        fake.status.played_frames = 7100;
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_GETOPTR, &ci) == 0);
        CHECK(ci.blocks == 2); // 6 fragments' starts passed, 4 told already
        v = AFMT_U8;           // the same fragments in bytes: four times the frames
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SETFMT, v) == 0);
        CHECK(fake.config.wake_frames == 2048);
        CHECK(fake.config.ring_frames == 8192);
        v = AFMT_S16_LE;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SETFMT, v) == 0);
        v = 0x7fff0008; // as many as it likes of 256 bytes
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETFRAGMENT, v) == 0);
        CHECK(fake.config.wake_frames == 64);
        CHECK(fake.config.ring_frames == 8192); // the default (100 ms), rounded
}

TEST_CASE("OSS: without SETFRAGMENT, each change asks for the kernel's sizes afresh") {
        reset();
        int v = 44100;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SPEED, v) == 0);
        CHECK(fake.config.ring_frames == 8192); // 4410 frames, rounded: not the 8 kHz ring kept
        CHECK(fake.config.wake_frames == 240);
        v = 0x0002000a;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SETFRAGMENT, v) == 0);
        CHECK(fake.config.wake_frames == 1024); // u8 mono: 1024 bytes
        __bmds_oss_closed(AudioFd);             // closed and opened again: forgotten
        v = 8000;
        REQUIRE(ioctlInt(AudioFd, SNDCTL_DSP_SPEED, v) == 0);
        CHECK(fake.config.wake_frames == 240);
}

TEST_CASE("OSS: sync, reset, post and the trigger are the audio controls") {
        reset();
        int v = 0;
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_SYNC, nullptr) == 0);
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_RESET, nullptr) == 0);
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_POST, nullptr) == 0);
        v = 0;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETTRIGGER, v) == 0);
        v = PCM_ENABLE_OUTPUT;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_SETTRIGGER, v) == 0);
        const std::vector<uint32_t> want = {BMDS_AUDIO_DRAIN, BMDS_AUDIO_DROP, BMDS_AUDIO_START, BMDS_AUDIO_PAUSE,
                                            BMDS_AUDIO_RESUME};
        CHECK(fake.ops == want);
        fake.status.state = BMDS_AUDIO_PAUSED;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_GETTRIGGER, v) == 0);
        CHECK(v == 0);
        fake.status.state = BMDS_AUDIO_PLAYING;
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_GETTRIGGER, v) == 0);
        CHECK(v == PCM_ENABLE_OUTPUT);
        CHECK(ioctlInt(AudioFd, SNDCTL_DSP_GETCAPS, v) == 0);
        CHECK((v & DSP_CAP_TRIGGER) != 0);
        audio_buf_info info;
        CHECK(__bmds_oss_ioctl(AudioFd, SNDCTL_DSP_GETISPACE, &info) == -EINVAL); // no capture
}

TEST_CASE("OSS: the mixer is the device's volume and mute, its percent linear in dB") {
        reset();
        int v = 0;
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_READ_VOLUME, v) == 0);
        CHECK(v == (85 | 85 << 8)); // -9 dB
        v = 50 << 8;                // left 0, right 50: the louder
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_WRITE_PCM, v) == 0);
        CHECK(fake.settings["audio/dsp0/volume"] == "-30");
        CHECK(v == (50 | 50 << 8));
        v = 100;
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_WRITE_VOLUME, v) == 0);
        CHECK(fake.settings["audio/dsp0/volume"] == "0");
        v = 1;
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_WRITE_VOLUME, v) == 0);
        CHECK(fake.settings["audio/dsp0/volume"] == "-59");
        CHECK(v == (2 | 2 << 8)); // -59 dB reads back as 2 %
        v = 0;
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_WRITE_VOLUME, v) == 0);
        CHECK(fake.settings["audio/dsp0/mute"] == "true");
        CHECK(fake.settings["audio/dsp0/volume"] == "-59"); // kept for unmuting
        CHECK(v == 0);
        v = 40;
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_WRITE_VOLUME, v) == 0);
        CHECK(fake.settings["audio/dsp0/mute"] == "false");
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_READ_DEVMASK, v) == 0);
        CHECK(v == (SOUND_MASK_VOLUME | SOUND_MASK_PCM));
        CHECK(ioctlInt(MixerFd, SOUND_MIXER_READ_RECMASK, v) == 0);
        CHECK(v == 0);
        mixer_info info;
        CHECK(__bmds_oss_ioctl(MixerFd, SOUND_MIXER_INFO, &info) == 0);
        CHECK(std::string(info.id) == "dsp0");
        CHECK(ioctlInt(MixerFd, SNDCTL_DSP_SPEED, v) == -EINVAL); // not a dsp
        CHECK(__bmds_oss_room(MixerFd) == -1);
}
