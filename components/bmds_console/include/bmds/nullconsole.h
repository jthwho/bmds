/**
 * @file      nullconsole.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/console.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The terminal of what has none: input ends at once, output goes
 *        nowhere. A shell run without a terminal (a command over SSH without
 *        a pty, devplan/network.md §24) runs on it, its work going through
 *        its descriptors.
 *
 * @par Thread Safety
 * Thread-safe (no state of its own).
 */
class NullConsole final : public Console {
        public:
                /** @brief Returns the shared one. */
                static NullConsole &instance();

                constexpr NullConsole() = default;

                const char    *name() const override { return "null"; }
                uint32_t       capabilities() const override { return 0; }
                Result<size_t> write(ByteSpan data, unsigned int) override { return makeResult(data.size()); }
                Error          flush(unsigned int) override { return Error::Ok; }

                using Console::write;

        protected:
                Result<size_t> readInput(WritableByteSpan, unsigned int) override {
                        return makeError<size_t>(Error::EndOfFile);
                }
                Result<size_t> tryReadInput(WritableByteSpan) override { return makeError<size_t>(Error::EndOfFile); }
                bool           hasInput() const override { return true; } // a read ends at once
};

BMDS_NAMESPACE_END
