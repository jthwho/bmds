/**
 * @file      clipboardconversions.h
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
#include <bmds/stringview.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The converters between clipboard types and the way from an offered
 *        type to a wanted one (`devplan/clipboard.md` C6).
 *
 * A converter is built in (`ClipboardConverter`) or a command: a filter
 * program from stdin to stdout, configured as `FROM>TO=COMMAND [ARG...]` in an
 * INI text's `[Convert]` group (the separating `=` is the first after TO's
 * parameters). The first converter added for a pair wins, so
 * the configuration is read before `addBuiltins()`, and a configured
 * converter replaces a built-in one. `find()` takes the shortest way, at most
 * `MaxSteps` converters, from the earliest offered type it can.
 *
 * The converters' types and commands are views of the caller's text, which
 * must outlive the table.
 *
 * @par Thread Safety
 * A value: filled by one thread, then read by any.
 */
class ClipboardConversions {
        public:
                /** @brief The built-in converters (append only). */
                enum class Builtin : uint8_t {
                        None = 0,      ///< A command.
                        UriListToText, ///< `text/uri-list` to text: paths, a line each.
                        Latin1ToText,  ///< ISO 8859-1 (ASCII's superset) to text.
                        TextToLatin1,  ///< Text to ISO 8859-1, `?` for what it hasn't.
                };

                /** @brief A converter. */
                struct Converter {
                                StringView from;                    ///< The type it reads.
                                StringView to;                      ///< The type it writes.
                                StringView command;                 ///< A command's line (`Exec`'s quoting).
                                Builtin    builtin = Builtin::None; ///< Or built in.
                                uint32_t   fromHash = 0;            ///< `from`'s normal form's hash.
                                uint32_t   toHash = 0;              ///< `to`'s.
                };

                /** @brief The most converters kept. */
                static constexpr size_t MaxConverters = 32;

                /** @brief The most converters on a way. */
                static constexpr size_t MaxSteps = 2;

                /** @brief The most offered types `find()` looks at. */
                static constexpr size_t MaxOffered = 8;

                /** @brief A way from an offered type to the wanted one. */
                struct Path {
                                uint8_t offered = 0;              ///< The offered type's index.
                                uint8_t steps = 0;                ///< Converters on it (0: the type as is).
                                uint8_t converter[MaxSteps] = {}; ///< Their indexes, in order.
                };

                /** @brief Told of a line of the configuration that isn't a converter. */
                using Report = void (*)(void *arg, uint32_t line, StringView text);

                /**
                 * @brief Adds a command from @p from to @p to (views kept).
                 * @return `Ok`, `Invalid` (a type isn't valid, or the command is
                 *         empty), `Exists` (the pair has one: kept), or `Full`.
                 */
                Error add(StringView from, StringView to, StringView command);

                /**
                 * @brief Adds the `[Convert]` group's converters of the INI text
                 *        @p text (views kept); @p report (may be null) is told of
                 *        each entry that isn't one (a pair already there is not
                 *        reported).
                 * @return `Ok`, or `Full` (the rest not added).
                 */
                Error read(StringView text, Report report, void *arg);

                /** @brief Adds the built-in converters (after the configured ones). @return `Ok` or `Full`. */
                Error addBuiltins();

                /** @brief Returns the converters' number. */
                size_t count() const { return _count; }

                /** @brief Returns converter @p index (below `count()`). */
                const Converter &converter(size_t index) const { return _converters[index]; }

                /**
                 * @brief Finds the shortest way to @p wanted from @p offered (the
                 *        owner's order: the earliest first, `MaxOffered` at most).
                 * @return The way; `NotFound`, or `Invalid` (@p wanted isn't a type).
                 */
                Result<Path> find(Span<const StringView> offered, StringView wanted) const;

        private:
                Error addConverter(StringView from, StringView to, StringView command, Builtin builtin);

                Converter _converters[MaxConverters];
                uint8_t   _count = 0;
};

/**
 * @brief A built-in converter, run on data in pieces of any size.
 *
 * @par Thread Safety
 * A value: one per transfer.
 */
class ClipboardConverter {
        public:
                /** @brief Converts as @p builtin (`None`: copies). */
                explicit ClipboardConverter(ClipboardConversions::Builtin builtin) : _builtin(builtin) {}

                /** @brief The most bytes `put()` writes for @p bytes bytes. */
                static constexpr size_t maxOutput(size_t bytes) { return bytes * 2 + MaxFinish; }

                /** @brief The most bytes `finish()` writes. */
                static constexpr size_t MaxFinish = 8;

                /**
                 * @brief Converts @p data into @p out, which has room for
                 *        `maxOutput(data.size())` bytes.
                 * @return The bytes written.
                 */
                size_t put(ByteSpan data, uint8_t *out);

                /**
                 * @brief Ends the data: writes what was held back (at most
                 *        `MaxFinish` bytes) into @p out, and starts again.
                 * @return The bytes written.
                 */
                size_t finish(uint8_t *out);

        private:
                // Where a `text/uri-list` line is.
                enum class Uri : uint8_t {
                        LineStart,
                        Scheme,
                        Host,
                        Path,
                        Percent,
                        Plain,
                        Comment
                };

                size_t uriByte(uint8_t b, uint8_t *out);
                size_t uriFlush(uint8_t *out);

                ClipboardConversions::Builtin _builtin;
                Utf8Decoder                   _utf8;
                Uri                           _uri = Uri::LineStart;
                uint8_t                       _held = 0;        // `file://`'s bytes matched, or a %'s digits
                char                          _hex[2] = {};     // a %'s digits so far
                bool                          _newline = false; // a line done: LF before the next
};

BMDS_NAMESPACE_END
