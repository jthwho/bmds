/**
 * @file      clipboard.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/clipboardconversions.h>
#include <bmds/clipboardtype.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A clipboard: what was copied, as typed offers (`devplan/clipboard.md`).
 *
 * Each selection (`Selection`) holds one offer at a time: the types a copy
 * can be read as, each either stored now (bytes in PSRAM, kept until a newer
 * offer replaces them) or rendered by the offer's owner when asked for
 * (`Owner::requested`, given a pipe's write end). A newer offer replaces the
 * older one, whose owner is told (`Owner::lost`). A reader asks for one
 * offered type with `request()` and reads it from the handle it gets: stored
 * bytes straight from their block, a type on request from the pipe.
 * `ClipboardReader` adds conversion.
 *
 * The clipboard knows no window manager and no application. Clipboards are
 * numbered (`clipboard()`): 0 is the seat's, made by `init()` with its files
 * `/dev/clip0` (the clipboard's text) and `/dev/clip0p` (`Primary`'s).
 *
 * Owner calls are made on the requesting task, never with the clipboard's
 * data lock held, one at a time per clipboard; they must not wait long and
 * must not call the clipboard (queue the work: a write to the pipe waits for
 * its reader). After `withdraw()` returns, none is in progress or comes.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class Clipboard {
        public:
                /** @brief The selections (append only: their numbers are frozen). */
                enum class Selection : uint8_t {
                        Clipboard = 0, ///< Ctrl+C and Ctrl+V's.
                        Primary = 1,   ///< The last selection; the middle button pastes it.
                        Drag = 2,      ///< Drag and drop's (§5).
                };

                /** @brief How many selections there are. */
                static constexpr size_t Selections = 3;

                /** @brief The most types an offer has. */
                static constexpr size_t MaxTypes = 8;

                /** @brief The most clipboards. */
                static constexpr size_t MaxClipboards = 1;

                /** @brief The longest owner's name kept, in bytes. */
                static constexpr size_t MaxOwnerName = 15;

                /** @brief The bytes stored at most, all selections together, unless configured. */
                static constexpr size_t DefaultLimitBytes = 1u << 20;

                /** @brief The configuration file `init()` reads. */
                static constexpr StringView ConfigPath{"/boot/config/clipboard.conf"};

                /** @brief Who renders an offer's types on request, and is told it lost it. */
                struct Owner {
                                /**
                                 * @brief Asked for @p type (normal form) of offer @p serial: write
                                 *        it to @p write and drop the handle (that ends it).
                                 */
                                using Requested = void (*)(void *arg, uint32_t serial, StringView type,
                                                           FileHandle::Ptr write);
                                /** @brief Offer @p serial was replaced or cleared by someone else. */
                                using Lost = void (*)(void *arg, uint32_t serial);

                                Requested  requested = nullptr; ///< Null: every type is stored.
                                Lost       lost = nullptr;      ///< May be null.
                                void      *arg = nullptr;
                                StringView name;        ///< Shown by `clip list` (`MaxOwnerName` kept).
                                int32_t    process = 0; ///< Its process; 0: not known.
                                uint8_t    window = 0;  ///< The window copied from; 0: none.
                };

                /** @brief One type of an offer, as `describe()` gives it. */
                struct TypeInfo {
                                char    type[ClipboardType::BufferBytes] = {}; ///< Its normal form.
                                int64_t size = -1;                             ///< Stored bytes; -1: on request.
                };

                /** @brief A selection's offer, as `describe()` gives it. */
                struct Description {
                                uint32_t serial = 0;                   ///< 0: empty.
                                char     owner[MaxOwnerName + 1] = {}; ///< Its owner's name.
                                int32_t  process = 0;
                                uint8_t  window = 0;
                                uint8_t  count = 0; ///< Types.
                                TypeInfo types[MaxTypes];
                };

                /** @brief Told of a configuration line it doesn't take (its number and text). */
                using Report = ClipboardConversions::Report;

                Clipboard();
                ~Clipboard();

                Clipboard(const Clipboard &) = delete;
                Clipboard &operator=(const Clipboard &) = delete;

                /**
                 * @brief Makes clipboard 0 (a boot stage, after `/boot`): reads
                 *        `ConfigPath` and adds `/dev/clip0` and `/dev/clip0p`.
                 * @return `Ok`, or what failed (the clipboard works without its files).
                 */
                static Error init();

                /**
                 * @brief Makes clipboard @p number if it isn't yet (`init()`; tests),
                 *        with no configuration and no files.
                 * @return It, or `NotSupported` (over `MaxClipboards`).
                 */
                static Result<Clipboard *> create(size_t number);

                /**
                 * @brief Adds clipboard @p number's files to `/dev`: `clipN` (the
                 *        clipboard's text) and `clipNp` (`Primary`'s).
                 * @return `Ok`, `NotExist` (no such clipboard), or the VFS's error.
                 */
                static Error addFiles(size_t number);

                /** @brief Returns clipboard @p number, or `NotExist`. */
                static Result<Clipboard *> clipboard(size_t number);

                /**
                 * @brief Takes the configuration @p text (`clipboard.conf`: `[Limits]
                 *        Bytes`, `[Convert]`), copied; @p report (may be null) is told
                 *        of lines it doesn't take. Before the clipboard is used.
                 * @return `Ok` or `NoMem`.
                 */
                Error configure(StringView text, Report report, void *arg);

                /** @brief Returns the conversions (configured, then built in), or null (no memory). */
                const ClipboardConversions *conversions() const { return _conversions; }

                /** @brief Returns the bytes it stores at most. */
                size_t limit() const { return _limit; }

                /**
                 * @brief Makes a new offer of @p types (normalized; `MaxTypes` at
                 *        most, no type twice) on @p selection, none stored yet,
                 *        replacing the one there.
                 * @return Its serial (never 0), or `Invalid` (a bad type, none, too
                 *         many, a type twice), `NoMem`.
                 */
                Result<uint32_t> offer(Selection selection, Span<const StringView> types, const Owner &owner);

                /**
                 * @brief Stores @p bytes as @p type of offer @p serial (a copy; a type
                 *        stored before is replaced).
                 * @return `Ok`, `InvalidState` (not the offer there now), `NotFound`
                 *         (not one of its types), `NoSpace` (over `limit()`), `NoMem`.
                 */
                Error store(Selection selection, uint32_t serial, StringView type, ByteSpan bytes);

                /**
                 * @brief Copies @p bytes as every one of @p types (one stored block),
                 *        a new offer without an owner, from @p name.
                 * @return Its serial, or `offer()`'s and `store()`'s errors.
                 */
                Result<uint32_t> copy(Selection selection, Span<const StringView> types, ByteSpan bytes,
                                      StringView name, int32_t process);

                /**
                 * @brief Empties @p selection: offer @p serial only, or whatever is
                 *        there (0; its owner is told).
                 * @return `Ok` (also when empty), or `InvalidState` (another offer).
                 */
                Error clear(Selection selection, uint32_t serial);

                /**
                 * @brief An owner going: offer @p serial keeps its stored types and
                 *        loses the rest (and empties when none is left). When it
                 *        returns, no owner call is in progress.
                 * @return `Ok`, or `InvalidState` (not the offer there now).
                 */
                Error withdraw(Selection selection, uint32_t serial);

                /**
                 * @brief Opens @p type of @p selection's offer for reading.
                 * @param taken [out] Its normal form (`ClipboardType::BufferBytes`); may be null.
                 * @return The read end: bytes, then `EndOfFile`. `Empty`, `NotFound`
                 *         (not offered), `Invalid` (not a type), `NotExist` (its owner
                 *         went), `NoMem`.
                 * @blocking Only while an owner is called.
                 */
                Result<FileHandle::Ptr> request(Selection selection, StringView type, char *taken);

                /** @brief Describes @p selection's offer into @p out (`serial` 0: empty). */
                void describe(Selection selection, Description &out) const;

                /** @brief Returns the size of @p selection's text if stored as text, else 0 (`stat`). */
                uint64_t storedTextSize(Selection selection) const;

                /** @brief Returns the bytes stored now, all selections together. */
                size_t storedBytes() const;

        private:
                struct Offer;

                void   release(Offer *offer);                                  // frees it and its blocks; not locked
                void   tellLost(Offer *offer);                                 // its owner, under the call lock
                Offer *take(Selection selection, uint32_t serial, Error &err); // removes it; locked

                mutable Mutex         _lock;     // the offers and the counts
                Mutex                 _callLock; // owner calls, one at a time
                Offer                *_offers[Selections] = {};
                uint32_t              _serial = 0;
                size_t                _stored = 0;
                size_t                _limit = DefaultLimitBytes;
                String                _config;                // the configuration's text, kept for its views
                ClipboardConversions *_conversions = nullptr; // PSRAM
};

BMDS_NAMESPACE_END
