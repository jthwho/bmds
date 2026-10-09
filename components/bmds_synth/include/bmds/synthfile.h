/**
 * @file      synthfile.h
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
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/sampleformat.h>
#include <bmds/soundfont.h>
#include <bmds/stream.h>
#include <bmds/string.h>
#include <bmds/synth.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A synth fd (`audio_synth`, `devplan/audio.md` §22): MIDI events
 *        written with their frames, samples read.
 *
 * Fonts are loaded once and shared by path while any synth plays them
 * (`SoundFonts`). Writes take `Synth::Event` records (`bmds_synth_event_t`),
 * whole ones; reads render whole frames, in `F32Le` or `S16Le`. Neither waits.
 *
 * @par Thread Safety
 * Thread-safe: calls serialize on its lock.
 */
class SynthFile final : public Stream {
        public:
                /** @brief How it plays (`bmds_synth_config_t`). */
                struct Config {
                                uint32_t     rateHz = 48000;
                                uint8_t      channels = 2;
                                SampleFormat format = SampleFormat::F32Le;
                                uint16_t     maxVoices = 0;
                                StringView   font; ///< A path; empty: `synth/font`.
                };

                /** @brief Its state (`bmds_synth_info_t`). */
                struct Info {
                                Synth::Stats synth;
                                Config       config;
                                size_t       presets = 0;
                                size_t       fontBytes = 0;
                                uint32_t     cpuPermille = 0;    ///< The last read's render time over its frames' time.
                                uint32_t     cpuPermilleMax = 0; ///< The most since open.
                };

                /** @brief The font `synth/font` names when none is given. */
                static constexpr const char *DefaultFont = "/sd0/p0/share/soundfonts/TimGM6mb.sf2";

                /**
                 * @brief Opens a synth on @p config's font (`context` for a relative path).
                 * @return A read-write handle, or `Invalid` (a rate, channel count,
                 *         format or voice count), the font's open or load error
                 *         (`NotExist`, `Invalid` for a damaged one, `NoMem`).
                 * @blocking While a font loads (TimGM6mb: 0.72 s from the card); one loaded
                 *           before and kept (`synth/keep-fonts`) doesn't.
                 */
                static Result<FileHandle::Ptr> open(const Config &config, const Vfs::Context &context);

                /** @brief Returns its state. */
                Info info();

                /** @brief Returns its font's path. */
                StringView fontPath() const { return _fontPath; }

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                SynthFile *synthFile() override { return this; }
                void       handleClosed(uint8_t mode) override;

        private:
                SynthFile() = default;
                ~SynthFile() = default;

                Mutex      _lock;
                Synth      _synth;
                SoundFont *_font = nullptr;
                String     _fontPath;
                Config     _config;
                uint32_t   _cpuPermille = 0;
                uint32_t   _cpuPermilleMax = 0;
};

/**
 * @brief The fonts loaded, shared by path (`devplan/audio.md` §22). A font
 *        stays while a synth plays it, and after (`synth/keep-fonts`) until
 *        memory runs short: it is a `Memory::Reclaimer`, the longest idle font
 *        freed first. A font whose file changed loads again.
 *
 * @par Thread Safety
 * Thread-safe; it also serializes opening and closing synths on a font
 * (`Synth`'s rule).
 */
namespace SoundFonts {

        /**
         * @brief Boot stage: the (empty) list of fonts loaded, and MIDI files as a
         *        decoder format (`Decoder::addFormat()`). @return `Ok`.
         */
        Error init();

        /** @brief Sets @p out to the font synths play when none is named (`synth/font`). */
        Error defaultFont(String &out);

        /**
         * @brief Opens @p synth on the font at @p path (loading it if no one
         *        has), with @p config and the gain `synth/gain` sets.
         * @return The font, or its error.
         * @blocking While the font loads.
         */
        Result<SoundFont *> openSynth(StringView path, const Synth::Config &config, Synth &synth);

        /** @brief Closes @p synth, and frees @p font if no synth plays it now. */
        void closeSynth(Synth &synth, SoundFont *font);

} // namespace SoundFonts

BMDS_NAMESPACE_END
