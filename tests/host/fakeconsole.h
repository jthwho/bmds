/**
 * @file      fakeconsole.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A Console for host tests: records output, serves queued input.
 */

#pragma once

#include <string>
#include <bmds/console.h>

class FakeConsole final : public bmds::Console {
        public:
                using Console::write;

                const char *name() const override { return "fake"; }
                uint32_t    capabilities() const override { return Ansi; }
                Size        size() const override { return Size(); }

                bmds::Result<size_t> read(bmds::WritableByteSpan buf, unsigned int) override { return tryRead(buf); }

                bmds::Result<size_t> tryRead(bmds::WritableByteSpan buf) override {
                        size_t n = 0;
                        while (n < buf.size() && _inPos < input.size())
                                buf[n++] = static_cast<uint8_t>(input[_inPos++]);
                        if (n == 0) return bmds::makeError<size_t>(bmds::Error::WouldBlock);
                        return bmds::makeResult(n);
                }

                bmds::Result<size_t> write(bmds::ByteSpan data, unsigned int) override {
                        output.append(reinterpret_cast<const char *>(data.data()), data.size());
                        return bmds::makeResult(data.size());
                }

                bmds::Error flush(unsigned int) override { return bmds::Error(); }

                /** @brief Returns and clears the output so far. */
                std::string take() {
                        std::string out;
                        out.swap(output);
                        return out;
                }

                std::string input;
                std::string output;

        private:
                size_t _inPos = 0;
};
