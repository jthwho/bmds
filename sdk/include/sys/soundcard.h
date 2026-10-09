/**
 * @file      soundcard.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * OSS, the Open Sound System's programming interface, as BMDS provides it
 * (devplan/audio.md §18): `/dev/dsp`, `/dev/audio` and `/dev/mixer`, with
 * the SDK's `ioctl()` carrying these requests out on the native audio calls
 * (`bmds/sys.h`). Written for BMDS; the numbers are Linux's
 * (`include/uapi/linux/soundcard.h`), which programs were built against.
 *
 * Playback only: capture (`SNDCTL_DSP_GETISPACE`, recording sources) fails
 * until BMDS has it. Requests not listed fail with `EINVAL`.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_SYS_SOUNDCARD_H
#define BMDS_SYS_SOUNDCARD_H

#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The OSS version this answers to (`OSS_GETVERSION`): Linux's. */
#define SOUND_VERSION 0x030802

/* --- /dev/dsp -------------------------------------------------------- */

#define SNDCTL_DSP_RESET _IO('P', 0)                       /**< Drops what is queued. */
#define SNDCTL_DSP_SYNC _IO('P', 1)                        /**< Waits until what is queued has played. */
#define SNDCTL_DSP_SPEED _IOWR('P', 2, int)                /**< Sets the rate; returns the one chosen. */
#define SNDCTL_DSP_STEREO _IOWR('P', 3, int)               /**< 0 mono, 1 stereo. */
#define SNDCTL_DSP_GETBLKSIZE _IOWR('P', 4, int)           /**< The fragment size in bytes. */
#define SNDCTL_DSP_SETFMT _IOWR('P', 5, int)               /**< Sets an `AFMT_` format; returns the one chosen. */
#define SNDCTL_DSP_SAMPLESIZE SNDCTL_DSP_SETFMT            /**< The old name. */
#define SNDCTL_DSP_CHANNELS _IOWR('P', 6, int)             /**< Sets the channels; returns the number chosen. */
#define SNDCTL_DSP_POST _IO('P', 8)                        /**< Plays what is queued now. */
#define SNDCTL_DSP_SUBDIVIDE _IOWR('P', 9, int)            /**< Accepted, ignored. */
#define SNDCTL_DSP_SETFRAGMENT _IOWR('P', 10, int)         /**< `0xMMMMSSSS`: M fragments of 2^S bytes. */
#define SNDCTL_DSP_GETFMTS _IOR('P', 11, int)              /**< The `AFMT_` formats taken (a mask). */
#define SNDCTL_DSP_GETOSPACE _IOR('P', 12, audio_buf_info) /**< Room to write without waiting. */
#define SNDCTL_DSP_GETISPACE _IOR('P', 13, audio_buf_info) /**< Capture: fails. */
#define SNDCTL_DSP_NONBLOCK _IO('P', 14)                   /**< Sets `O_NONBLOCK`. */
#define SNDCTL_DSP_GETCAPS _IOR('P', 15, int)              /**< `DSP_CAP_` bits. */
#define SNDCTL_DSP_GETTRIGGER _IOR('P', 16, int)           /**< `PCM_ENABLE_OUTPUT` unless paused. */
#define SNDCTL_DSP_SETTRIGGER _IOW('P', 16, int)           /**< Without `PCM_ENABLE_OUTPUT`: paused. */
#define SNDCTL_DSP_GETIPTR _IOR('P', 17, count_info)       /**< Capture: fails. */
#define SNDCTL_DSP_GETOPTR _IOR('P', 18, count_info)       /**< Bytes played. */
#define SNDCTL_DSP_GETODELAY _IOR('P', 23, int)            /**< Bytes until a byte written now is heard. */

#define SOUND_PCM_WRITE_RATE SNDCTL_DSP_SPEED        /**< The old name. */
#define SOUND_PCM_WRITE_CHANNELS SNDCTL_DSP_CHANNELS /**< The old name. */
#define SOUND_PCM_SETFMT SNDCTL_DSP_SETFMT           /**< The old name. */

/* `SNDCTL_DSP_SETFMT`'s formats. */
#define AFMT_QUERY 0x00000000   /**< Changes nothing: returns the format. */
#define AFMT_MU_LAW 0x00000001  /**< G.711 mu-law. */
#define AFMT_A_LAW 0x00000002   /**< G.711 A-law. */
#define AFMT_U8 0x00000008      /**< Unsigned 8-bit. */
#define AFMT_S16_LE 0x00000010  /**< Signed 16-bit, little-endian. */
#define AFMT_S16_BE 0x00000020  /**< Signed 16-bit, big-endian. */
#define AFMT_S8 0x00000040      /**< Signed 8-bit. */
#define AFMT_S16_NE AFMT_S16_LE /**< Native 16-bit: little-endian. */

/* `SNDCTL_DSP_GETCAPS` bits. */
#define DSP_CAP_REVISION 0x000000ffu /**< 0. */
#define DSP_CAP_TRIGGER 0x00001000u  /**< `SNDCTL_DSP_SETTRIGGER` works. */
#define DSP_CAP_MULTI 0x00004000u    /**< Opened more than once (each open is mixed). */

/* `SNDCTL_DSP_GETTRIGGER` and `SNDCTL_DSP_SETTRIGGER` bits. */
#define PCM_ENABLE_INPUT 0x00000001  /**< Capture: never set. */
#define PCM_ENABLE_OUTPUT 0x00000002 /**< Playing (not paused). */

/** @brief `SNDCTL_DSP_GETOSPACE`: the room in the buffer. */
typedef struct audio_buf_info {
                int fragments;  /**< Whole fragments free. */
                int fragstotal; /**< Fragments in all. */
                int fragsize;   /**< A fragment's bytes. */
                int bytes;      /**< Bytes free. */
} audio_buf_info;

/** @brief `SNDCTL_DSP_GETOPTR`: how far playing has got. */
typedef struct count_info {
                int bytes;  /**< Bytes played since the open (or the last configure). */
                int blocks; /**< Fragments played since the last `SNDCTL_DSP_GETOPTR`. */
                int ptr;    /**< Where in the buffer it is playing, in bytes. */
} count_info;

/* --- /dev/mixer ------------------------------------------------------ */

/* The mixer's channels: BMDS has `VOLUME` and `PCM`, both the device's volume. */
#define SOUND_MIXER_NRDEVICES 25
#define SOUND_MIXER_VOLUME 0
#define SOUND_MIXER_BASS 1
#define SOUND_MIXER_TREBLE 2
#define SOUND_MIXER_SYNTH 3
#define SOUND_MIXER_PCM 4
#define SOUND_MIXER_SPEAKER 5
#define SOUND_MIXER_LINE 6
#define SOUND_MIXER_MIC 7
#define SOUND_MIXER_CD 8
#define SOUND_MIXER_IMIX 9
#define SOUND_MIXER_ALTPCM 10
#define SOUND_MIXER_RECLEV 11
#define SOUND_MIXER_IGAIN 12
#define SOUND_MIXER_OGAIN 13
#define SOUND_MIXER_LINE1 14
#define SOUND_MIXER_LINE2 15
#define SOUND_MIXER_LINE3 16
#define SOUND_MIXER_DIGITAL1 17
#define SOUND_MIXER_DIGITAL2 18
#define SOUND_MIXER_DIGITAL3 19
#define SOUND_MIXER_PHONEIN 20
#define SOUND_MIXER_PHONEOUT 21
#define SOUND_MIXER_VIDEO 22
#define SOUND_MIXER_RADIO 23
#define SOUND_MIXER_MONITOR 24
#define SOUND_MIXER_NONE 31

/* Masks of channels. */
#define SOUND_MASK_VOLUME (1 << SOUND_MIXER_VOLUME)
#define SOUND_MASK_PCM (1 << SOUND_MIXER_PCM)

/* What the mixer has (read only). */
#define SOUND_MIXER_RECSRC 0xff     /**< The recording sources chosen: none. */
#define SOUND_MIXER_DEVMASK 0xfe    /**< The channels it has. */
#define SOUND_MIXER_RECMASK 0xfd    /**< The recording sources it has: none. */
#define SOUND_MIXER_CAPS 0xfc       /**< `SOUND_CAP_` bits: none. */
#define SOUND_MIXER_STEREODEVS 0xfb /**< The channels with two sides. */

/** @brief Reads channel @p dev: left in the low byte, right in the next, 0-100 each. */
#define MIXER_READ(dev) _IOR('M', dev, int)
/** @brief Sets channel @p dev (as `MIXER_READ`); returns what was set. */
#define MIXER_WRITE(dev) _IOWR('M', dev, int)

#define SOUND_MIXER_READ_VOLUME MIXER_READ(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_READ_PCM MIXER_READ(SOUND_MIXER_PCM)
#define SOUND_MIXER_READ_RECSRC MIXER_READ(SOUND_MIXER_RECSRC)
#define SOUND_MIXER_READ_DEVMASK MIXER_READ(SOUND_MIXER_DEVMASK)
#define SOUND_MIXER_READ_RECMASK MIXER_READ(SOUND_MIXER_RECMASK)
#define SOUND_MIXER_READ_STEREODEVS MIXER_READ(SOUND_MIXER_STEREODEVS)
#define SOUND_MIXER_READ_CAPS MIXER_READ(SOUND_MIXER_CAPS)
#define SOUND_MIXER_WRITE_VOLUME MIXER_WRITE(SOUND_MIXER_VOLUME)
#define SOUND_MIXER_WRITE_PCM MIXER_WRITE(SOUND_MIXER_PCM)

/** @brief `SOUND_MIXER_INFO`: the mixer's names. */
typedef struct mixer_info {
                char id[16];         /**< The device: "dsp0". */
                char name[32];       /**< "BMDS dsp0". */
                int  modify_counter; /**< 0. */
                int  fillers[10];    /**< 0. */
} mixer_info;

#define SOUND_MIXER_INFO _IOR('M', 101, mixer_info) /**< The mixer's names. */
#define OSS_GETVERSION _IOR('M', 118, int)          /**< `SOUND_VERSION`. */

#ifdef __cplusplus
}
#endif

#endif /* BMDS_SYS_SOUNDCARD_H */
