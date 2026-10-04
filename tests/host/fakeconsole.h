/**
 * @file      fakeconsole.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A Console for host tests: records output, serves queued input. Writes
 * are thread-safe (pipeline stages write concurrently); `read()` returns
 * EndOfFile once the queued input is used up.
 */

#pragma once

#include <mutex>
#include <string>
#include <bmds/console.h>

class FakeConsole final : public bmds::Console {
        public:
                using Console::write;

                const char *name() const override { return "fake"; }
                uint32_t    capabilities() const override { return caps; }


                bmds::Result<size_t> write(bmds::ByteSpan data, unsigned int) override {
                        std::lock_guard<std::mutex> lock(_lock);
                        output.append(reinterpret_cast<const char *>(data.data()), data.size());
                        return bmds::makeResult(data.size());
                }

                bmds::Error flush(unsigned int) override { return bmds::Error(); }

                /** @brief Returns and clears the output so far. */
                std::string take() {
                        std::lock_guard<std::mutex> lock(_lock);
                        std::string                 out;
                        out.swap(output);
                        return out;
                }

                std::string input;
                std::string output;
                uint32_t    caps = Ansi; ///< SizeQuery too, for tests of the size request

        protected:
                bmds::Result<size_t> readInput(bmds::WritableByteSpan buf, unsigned int) override {
                        if (interrupt().isRaised()) return bmds::makeError<size_t>(bmds::Error::Interrupted);
                        auto r = tryReadInput(buf);
                        if (r.error() == bmds::Error::WouldBlock)
                                return bmds::makeError<size_t>(bmds::Error::EndOfFile);
                        return r;
                }

                bmds::Result<size_t> tryReadInput(bmds::WritableByteSpan buf) override {
                        size_t n = 0;
                        while (n < buf.size() && _inPos < input.size())
                                buf[n++] = static_cast<uint8_t>(input[_inPos++]);
                        if (n == 0) return bmds::makeError<size_t>(bmds::Error::WouldBlock);
                        return bmds::makeResult(n);
                }

        private:
                std::mutex _lock;
                size_t     _inPos = 0;
};
