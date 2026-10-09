/**
 * @file      sinetone.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A sine at a set frequency and level, added into a stereo float mix:
 *        `audio tone`'s and the system sounds' source (`devplan/audio.md`
 *        §16, §17).
 *
 * A rotating phasor (a complex multiply a frame, renormalized each fill)
 * rather than `sinf` per frame. Each `fill()` fades in over the fade time
 * after `start()` and out after `fadeOut()`, so the tone starts and stops
 * without a click.
 *
 * @par Thread Safety
 * Not thread-safe; used by one task.
 */
class SineTone {
        public:
                /** @brief Which channels carry the tone (the others are silent). */
                enum Slots : uint8_t {
                        Left = 1,
                        Right = 2,
                        Both = 3
                };

                /** @brief The fade without one given. */
                static constexpr uint32_t DefaultFadeUs = 5000;

                /**
                 * @brief Starts a tone of @p frequencyHz at @p rateHz and @p levelCentiDb
                 *        (hundredths of a dB below full scale), in @p slots, fading in
                 *        and out over @p fadeUs.
                 */
                void start(uint32_t frequencyHz, uint32_t rateHz, int32_t levelCentiDb, Slots slots,
                           uint32_t fadeUs = DefaultFadeUs);

                /** @brief Begins the fade out; `isFinished()` once it ends. */
                void fadeOut();

                /** @brief Returns true once faded out (or never started). */
                bool isFinished() const { return _fadePosition == 0 && !_fadingIn; }

                /**
                 * @brief Adds @p frames stereo frames into @p mix.
                 * @hotpath
                 */
                void fill(float *mix, size_t frames);

        private:
                float    _re = 1; // the phasor
                float    _im = 0;
                float    _stepRe = 1; // its rotation a frame
                float    _stepIm = 0;
                float    _amplitude = 0;
                float    _fadeStep = 0;     // the gain a frame of fade: 1 / _fadeFrames
                uint32_t _fadeFrames = 1;   // the fade's length
                uint32_t _fadePosition = 0; // the gain is _fadePosition / _fadeFrames
                bool     _fadingIn = false; // else out (or done)
                Slots    _slots = Both;
};

/**
 * @brief A short sequence of tones: a system sound, or one tone of a set
 *        length (`devplan/audio.md` §17).
 *
 * @par Thread Safety
 * Not thread-safe; used by one task.
 */
class ToneSequence {
        public:
                /** @brief One tone. */
                struct Note {
                                uint16_t frequencyHz; ///< 0: a rest.
                                uint16_t ms;          ///< Its length, fades included.
                                int16_t  centiDb;     ///< Its level below full scale.
                                uint16_t fadeUs;      ///< Its fade in and out.
                };

                /**
                 * @brief Starts @p notes (kept by the caller) at @p rateHz in @p slots.
                 */
                void start(Span<const Note> notes, uint32_t rateHz, SineTone::Slots slots);

                /** @brief Starts one note of @p ms (as `audio tone`). */
                void startTone(uint32_t frequencyHz, uint32_t ms, int32_t centiDb, uint32_t rateHz,
                               SineTone::Slots slots);

                /** @brief Fades the note playing out now and drops the rest. */
                void cancel();

                /** @brief True once every note has played and faded. */
                bool isFinished() const { return _index >= _notes.size() && _tone.isFinished(); }

                /**
                 * @brief Adds @p frames stereo frames into @p mix. A new note starts at
                 *        the next fill after the last one fades.
                 * @return False if this was its last fill (the fade's end, or nothing).
                 * @hotpath
                 */
                bool fill(float *mix, size_t frames);

        private:
                void next();

                SineTone         _tone;
                Span<const Note> _notes;
                size_t           _index = 0;      // the next note
                uint32_t         _framesLeft = 0; // of the note playing, before its fade out
                uint32_t         _rateHz = 0;
                SineTone::Slots  _slots = SineTone::Both;
};

/**
 * @brief The system sounds (`devplan/audio.md` §6.4, §17). Their numbers
 *        become ABI in step 3: append only.
 */
enum class SystemSound : uint8_t {
        Bell,   ///< The terminal's BEL.
        Click,  ///< UI feedback: a press.
        Error,  ///< UI feedback: something failed.
        Notify, ///< UI feedback: something to look at.
};

/** @brief System sounds in all. */
inline constexpr size_t SystemSoundCount = 4;

/** @brief Returns the notes of @p sound. */
Span<const ToneSequence::Note> systemSoundNotes(SystemSound sound);

/** @brief Returns the sound's name (`bell`, `click`, `error`, `notify`): the `audio` command's word. */
const char *systemSoundName(SystemSound sound);

BMDS_NAMESPACE_END
