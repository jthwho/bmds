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
#include <bmds/stats.h>
#include <bmds/stream.h>
#include <bmds/string.h>
#include <bmds/synth.h>
#include <bmds/synthengine.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A synth fd (`audio_synth`, `devplan/audio.md` §22, §26): MIDI events
 *        written with their frames, samples read, through either engine
 *        (`SynthEngines`).
 *
 * Fonts are loaded once and shared by path while any synth plays them
 * (`SoundFonts`); a bank in `/rom` is read in place. Writes take
 * `SynthEngine::Event` records (`bmds_synth_event_t`), whole ones; reads render
 * whole frames, in `F32Le` or `S16Le`. Neither waits.
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
                                StringView
                                        font; ///< The instruments' path (a font, a bank); empty: the engine's setting.
                                SynthEngine::Kind engine = SynthEngine::Kind::Default; ///< Open: resolved.
                };

                /** @brief Its state (`bmds_synth_info_t`). */
                struct Info {
                                SynthEngine::Stats synth;
                                Config       config;
                                size_t       presets = 0;
                                size_t       fontBytes = 0;
                                uint32_t     cpuPermille = 0;    ///< The last read's render time over its frames' time.
                                uint32_t     cpuPermilleMax = 0; ///< The most since open.
                };

                /** @brief The font `synth/font` names when none is given. */
                static constexpr const char *DefaultFont = "/sd0/p0/share/soundfonts/TimGM6mb.sf2";

                /**
                 * @brief Opens a synth of @p config's engine on its instruments (`context` for a
                 *        relative path), as `SynthEngines::open()`.
                 * @return A read-write handle, or `Invalid` (a rate, channel count,
                 *         format or voice count), the instruments' open or load error
                 *         (`NotExist`, `Invalid` for a damaged one, `NoMem`).
                 * @blocking While a font loads (TimGM6mb: 0.72 s from the card); one loaded
                 *           before and kept (`synth/keep-fonts`) doesn't, nor a bank.
                 */
                static Result<FileHandle::Ptr> open(const Config &config, const Vfs::Context &context);

                /** @brief Returns its state. */
                Info info();

                /** @brief Returns its instruments' path (a font's, a bank's). */
                StringView fontPath() const { return _fontPath; }

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                SynthFile *synthFile() override { return this; }
                void       handleClosed(uint8_t mode) override;

        private:
                SynthFile();
                ~SynthFile() = default;

                static void readStats(void *arg, Span<int64_t> values);

                Mutex        _lock;
                SynthEngine *_engine = nullptr;
                String       _fontPath;
                Config     _config;
                uint32_t   _cpuPermille = 0;
                uint32_t   _cpuPermilleMax = 0;
                int8_t       _statsNumber = -1; // N of its counters, synth.N (-1: none)
                Stats::Group _stats;            // devplan/sysmon.md §6
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
        Result<SoundFont *> openSynth(StringView path, const SynthEngine::Config &config, Synth &synth);

        /** @brief Closes @p synth, and frees @p font if no synth plays it now. */
        void closeSynth(Synth &synth, SoundFont *font);

} // namespace SoundFonts

/**
 * @brief The synth engines as the system has them (`devplan/audio.md` §26): which one a request
 *        means, the instruments each plays when none is named, and an engine opened on them
 *        with `synth/gain`. Synth fds and MIDI files open theirs here.
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace SynthEngines {

        /** @brief The bank the FM engine plays when none is named, while `synth/fm-bank` is unset. */
        inline constexpr const char *DefaultBank = "/rom/share/synth/dmxopl3.op2";

        /** @brief Returns the engine `Kind::Default` stands for with no instruments named: the system synth's. */
        SynthEngine::Kind systemKind();

        /**
         * @brief Returns the engine @p kind means for instruments @p path (absolute; empty: its
         *        setting's): itself, or for `Default` with a path, the file's own kind (an OP2
         *        bank FM, anything else the wavetable), else `systemKind()`.
         * @blocking A path's first 8 bytes are read.
         */
        SynthEngine::Kind resolve(SynthEngine::Kind kind, StringView path);

        /** @brief Sets @p out to the instruments @p kind plays when none is named (`synth/font`, `synth/fm-bank`). */
        Error defaultInstruments(SynthEngine::Kind kind, String &out);

        /**
         * @brief Opens an engine of @p kind (resolved by `resolve()`) on instruments @p path
         *        (absolute; empty: its setting's), with @p config and `synth/gain`'s gain.
         * @return The engine (`close()` it), or `Invalid` (a configuration, or damaged
         *         instruments), the file's error, `NoMem`.
         * @blocking While a font loads; a bank on the card is read (12 KB), one in `/rom` mapped.
         */
        Result<SynthEngine *> open(SynthEngine::Kind kind, StringView path, const SynthEngine::Config &config);

        /** @brief Closes @p engine (`open()`'s), letting go of its instruments; null: nothing. */
        void close(SynthEngine *engine);

        /** @brief Returns @p kind's name: "wavetable", "fm" ("default" for the request). */
        const char *name(SynthEngine::Kind kind);

} // namespace SynthEngines

BMDS_NAMESPACE_END
