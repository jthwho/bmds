/**
 * @file      speaker.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>

// Pico's handles (thirdparty/svox-pico/lib/picoapi.h).
struct pico_system;
struct pico_resource;
struct pico_engine;

BMDS_NAMESPACE_BEGIN

/**
 * @brief Text to speech: one SVOX Pico engine over a voice (`devplan/speech.md`).
 *
 * UTF-8 text goes in (`put()`), 16 kHz mono signed 16-bit samples come out
 * (`render()`). A voice is two files' bytes, read where they lie for as long
 * as the speaker is open (a mapping of `/rom`, or a copy). Pico works in one
 * pool, `PoolBytes` of PSRAM, taken at `open()`: nothing else is allocated.
 *
 * Text is spoken up to a sentence's end, or up to a NUL byte (said even
 * without one); what follows waits for more. Speed, pitch and volume apply
 * from the next text put. Without markup, tags in the text are read as text.
 *
 * @par Thread Safety
 * Not thread-safe; used by one thread at a time.
 */
class Speaker {
        public:
                /** @brief The samples' rate. */
                static constexpr uint32_t RateHz = 16000;

                /** @brief Pico's pool: its engine's 1,000,000 bytes, and the system's (measured 1,015,560). */
                static constexpr size_t PoolBytes = 1024 * 1024;

                /** @brief The speed, pitch and volume ranges, in percent of the voice's. */
                static constexpr uint16_t MinSpeed = 20;
                static constexpr uint16_t MaxSpeed = 500;
                static constexpr uint16_t MinPitch = 50;
                static constexpr uint16_t MaxPitch = 200;
                static constexpr uint16_t MinVolume = 1;
                static constexpr uint16_t MaxVolume = 500;

                /**
                 * @brief A voice's two files: text analysis (`LANG_ta.bin`) and signal generation
                 *        (`LANG_*_sg.bin`), and how far it may be raised above the loudest voices
                 *        without peaking higher (`Voices` knows Pico's six).
                 */
                struct Voice {
                                ByteSpan analysis;
                                ByteSpan signal;
                                float    boostDb = 0;
                };

                /**
                 * @brief How it speaks: percent of the voice's speed and pitch, and of the
                 *        loudest level that doesn't clip (`picoVolume()`; over about 115, peaks
                 *        can).
                 */
                struct Prosody {
                                uint16_t speed = 100;
                                uint16_t pitch = 100;
                                uint16_t volume = 100;
                };

                constexpr Speaker() = default;
                Speaker(const Speaker &) = delete;
                Speaker &operator=(const Speaker &) = delete;
                ~Speaker() { close(); }

                /**
                 * @brief Opens an engine on @p voice, obeying the text's markup if @p markup.
                 * @return `Ok`; `InvalidState` (open already), `Invalid` (@p prosody out of
                 *         range), `NoMem` (no pool), `CorruptData` (a file isn't a voice's,
                 *         a damaged one, or a voice missing a part), `NotSupported` (more
                 *         than the pool holds: no voice there is).
                 */
                Error open(const Voice &voice, bool markup, const Prosody &prosody);

                /** @brief Closes it, the text not yet spoken dropped. */
                void close();

                /** @brief Returns true while open. */
                bool isOpen() const { return _engine != nullptr; }

                /**
                 * @brief Sets the speed, pitch and volume for the text put from now on.
                 * @return `Ok`, `Invalid` (out of range), `InvalidState` (not open).
                 */
                Error setProsody(const Prosody &prosody);

                /**
                 * @brief Puts UTF-8 @p text (a character may be split between calls).
                 * @return The bytes taken: fewer than given when Pico's input is full
                 *         (0: render first); `InvalidState` (not open).
                 */
                Result<size_t> put(ByteSpan text);

                /**
                 * @brief Makes up to `out.size()` samples.
                 * @return The samples made; 0 when everything put has been said, up to
                 *         its last sentence's end or NUL. `InvalidState` (not open),
                 *         `LibraryFailure` (Pico failed: it is reset, its text dropped).
                 */
                Result<size_t> render(Span<int16_t> out);

                /** @brief Drops the text not yet spoken and the samples not yet read. */
                void reset();

                /**
                 * @brief Returns Pico's volume for @p prosody and a voice boost of @p boostDb:
                 *        the level at which the loudest of Pico's voices peaks at -1.5 dBFS
                 *        (between samples too, where Pico's output peaks up to 2 dB above them
                 *        and the engine's resampler finds it), at `Prosody::volume` 100. Pico's
                 *        own 100 clips, and its peaks rise as the pitch falls: measured over six
                 *        voices, five texts, pitches 50-200 and speeds 20-500 (810 runs).
                 */
                static uint16_t picoVolume(const Prosody &prosody, float boostDb);

                /** @brief Returns the bytes of Pico's pool in use now. */
                size_t poolUsed() const;

        private:
                // Pico's speech data comes an item at a time, at most 255 bytes, and a smaller
                // buffer is an error: so through this.
                static constexpr size_t StagingBytes = 256;

                void          *_pool = nullptr;
                pico_system   *_system = nullptr;
                pico_resource *_analysis = nullptr;
                pico_resource *_signal = nullptr;
                pico_engine   *_engine = nullptr;
                bool           _voiceDefined = false;
                float          _boostDb = 0;
                uint16_t       _stagedAt = 0;  // the next byte of _staging to give
                uint16_t       _stagedEnd = 0; // its end
                uint8_t        _staging[StagingBytes] = {};
};

BMDS_NAMESPACE_END
