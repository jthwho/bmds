/**
 * @file      speechfile.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/speaker.h>
#include <bmds/stream.h>
#include <bmds/stringview.h>
#include <bmds/voices.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A speech fd (`audio_speak`, `devplan/speech.md` §4): UTF-8 text
 *        written, 16 kHz mono signed 16-bit samples read.
 *
 * A write takes what Pico's input holds (a short count; `WouldBlock` when
 * nothing fit: read first). A read makes whole samples up to its size, and
 * returns 0 once everything written has been said up to the last NUL; text
 * after it waits for more (a sentence is said once text follows it). Neither
 * waits: synthesis runs in the reader's thread.
 *
 * @par Thread Safety
 * Thread-safe: calls serialize on its lock.
 */
class SpeechFile final : public Stream {
        public:
                /** @brief How it speaks (`bmds_speak_config_t`). */
                struct Config {
                                StringView       voice; ///< Empty: `speech/voice`.
                                Speaker::Prosody prosody;
                                bool             markup = false;
                };

                /** @brief Its state (`bmds_speak_info_t`). */
                struct Info {
                                StringView       voice;
                                Speaker::Prosody prosody;
                                bool             markup = false;
                                uint64_t         frames = 0; ///< Samples read so far.
                };

                /**
                 * @brief Opens a speaker on @p config's voice.
                 * @return A read-write handle; `Invalid` (prosody out of range, not a
                 *         voice's name), `NotExist` (no such voice), `NoMem`, `Busy`
                 *         (`Voices::MaxInUse` voices in use), `CorruptData` or
                 *         `NotSupported` (a damaged voice).
                 * @blocking While a voice not in place is read.
                 */
                static Result<FileHandle::Ptr> open(const Config &config);

                /** @brief Returns its state. The voice's name is valid while it is open. */
                Info info();

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                SpeechFile *speechFile() override { return this; }
                void        handleClosed(uint8_t mode) override;

        private:
                SpeechFile() = default;
                ~SpeechFile() = default;

                static constexpr size_t BlockSamples = 256; // a read into an odd address goes through this

                Mutex          _lock;
                Speaker        _speaker;
                Voices::Voice *_voice = nullptr;
                Config         _config;
                uint64_t       _frames = 0;
                int16_t        _block[BlockSamples] = {};
};

BMDS_NAMESPACE_END
