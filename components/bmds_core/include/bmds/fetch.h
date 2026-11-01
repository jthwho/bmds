/**
 * @file      fetch.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where a fetched file's bytes go (devplan/packages.md §23): what `pkg` and `update fetch`
 *        hand a `FetchFunction`.
 */
struct FetchSink {
                /**
                 * @brief The bytes start at @p start of a file of @p total bytes (`UINT64_MAX`: not
                 *        said), before any: 0 when a resumed fetch starts again. @return `Ok`, or an
                 *        error that stops the fetch (returned by it).
                 */
                Error (*begin)(void *arg, uint64_t start, uint64_t total) = nullptr;
                /** @brief The next bytes. @return `Ok`, or an error that stops the fetch. */
                Error (*body)(void *arg, ByteSpan bytes) = nullptr;
                void *arg = nullptr;
};

/**
 * @brief Fetches @p url from byte @p from into @p sink; the URL after redirects to @p finalUrl when
 *        not null. @return `Ok`, or the error with what happened in @p why.
 */
using FetchFunction = Error (*)(StringView url, uint64_t from, const FetchSink &sink, String *finalUrl, String &why);

/**
 * @brief The system's fetcher: HTTP and HTTPS over the network (bmds_pkg's `fetchsystem.cpp`, the
 *        main firmware only). Declared weak: test `&systemFetch != nullptr` first (recovery has none).
 * @blocking
 */
[[gnu::weak]] Error systemFetch(StringView url, uint64_t from, const FetchSink &sink, String *finalUrl, String &why);

BMDS_NAMESPACE_END
