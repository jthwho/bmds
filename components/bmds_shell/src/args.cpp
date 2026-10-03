/**
 * @file      args.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/args.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isSeparator(char c) {
                return c == ' ' || c == '\t';
        }

} // namespace

Result<size_t> tokenizeArgs(char *line, Span<char *> argv) {
        // `in` reads and `out` writes the same buffer. Quotes and escapes only
        // ever drop characters, so `out` never overtakes `in`.
        const char *in = line;
        char       *out = line;
        size_t      argc = 0;
        for (;;) {
                while (isSeparator(*in)) ++in;
                if (*in == '\0' || *in == '#') break;
                if (argc == argv.size()) return makeError<size_t>(Error::Full);
                argv[argc++] = out;
                while (*in != '\0' && !isSeparator(*in)) {
                        if (*in == '"') {
                                ++in;
                                while (*in != '\0' && *in != '"') {
                                        if (*in == '\\' && (in[1] == '"' || in[1] == '\\')) ++in;
                                        *out++ = *in++;
                                }
                                if (*in == '\0') return makeError<size_t>(Error::Invalid);
                                ++in;
                        } else if (*in == '\'') {
                                ++in;
                                while (*in != '\0' && *in != '\'') *out++ = *in++;
                                if (*in == '\0') return makeError<size_t>(Error::Invalid);
                                ++in;
                        } else if (*in == '\\' && in[1] != '\0') {
                                ++in;
                                *out++ = *in++;
                        } else {
                                *out++ = *in++;
                        }
                }
                // Read the separator before terminating: `out` may equal `in` here.
                const char next = *in;
                *out++ = '\0';
                if (next == '\0') break;
                ++in;
        }
        return makeResult(argc);
}

BMDS_NAMESPACE_END
