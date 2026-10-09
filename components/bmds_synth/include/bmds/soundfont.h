/**
 * @file      soundfont.h
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

struct tsf;

BMDS_NAMESPACE_BEGIN

/**
 * @brief A SoundFont (SF2, or SF3 with Vorbis samples) loaded into PSRAM
 *        (`devplan/audio.md` §22): its presets and 16-bit samples, shared by
 *        every `Synth` playing it.
 *
 * Fonts are untrusted input: loading checks what TinySoundFont needs
 * (fuzzed).
 *
 * @par Thread Safety
 * Immutable once loaded: any thread may read it. `destroy()` only once no
 * `Synth` uses it.
 */
class SoundFont {
        public:
                /** @brief Where a font is read from (a file's stream, memory). */
                class Reader {
                        public:
                                /**
                                 * @brief Reads exactly @p bytes into @p out.
                                 * @return `Ok`, or an error (also a short read).
                                 */
                                virtual Error read(void *out, size_t bytes) = 0;

                                /** @brief Skips @p bytes. @return `Ok`, or an error. */
                                virtual Error skip(size_t bytes) = 0;

                        protected:
                                ~Reader() = default;
                };

                /** @brief A preset: its bank, number and name. */
                struct Preset {
                                uint16_t    bank = 0;
                                uint16_t    number = 0;
                                const char *name = "";
                };

                /**
                 * @brief Loads a font from @p reader.
                 * @return The font, or `NoMem`, `Invalid` (not a SoundFont, damaged,
                 *         no samples), or the reader's error.
                 * @blocking While it reads (TimGM6mb: 0.41 s from the RAM disk).
                 */
                static Result<SoundFont *> load(Reader &reader);

                /** @brief Frees it (no `Synth` may use it any more). */
                void destroy();

                /** @brief Returns its presets' count. */
                size_t presetCount() const;

                /** @brief Returns preset @p index (below `presetCount()`). */
                Preset preset(size_t index) const;

                /** @brief Returns the bytes of PSRAM it holds. */
                size_t bytes() const { return _bytes; }

                /** @brief Returns TinySoundFont's master copy (for `Synth`). */
                ::tsf *handle() const { return _tsf; }

        private:
                SoundFont() = default;
                ~SoundFont() = default;

                ::tsf *_tsf = nullptr;
                size_t _bytes = 0;
};

BMDS_NAMESPACE_END
