/**
 * @file      playlist.h
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
#include <bmds/list.h>
#include <bmds/memspace.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The media player's playlist (`devplan/mediaplayer.md` §6f), pure:
 *        files with what is known of them, the entry playing, and the order
 *        they play in (the list's, or shuffled), repeat.
 *
 * The texts live in one arena; removing entries leaves their texts until a
 * compaction (when more than half is unused).
 *
 * Shuffle plays every entry once before any again: an order made when it is
 * turned on (the entry playing first) and again at the end of a round with
 * Repeat All (never starting with the one just played). Entries added while
 * shuffling go somewhere in what is left of the round.
 *
 * Removing the entry playing leaves none current, and the next is the one
 * that followed it.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class Playlist {
        public:
                /** @brief Entries at most. */
                static constexpr size_t MaxEntries = 10000;

                /** @brief No entry (`current()`, `next()`). */
                static constexpr size_t NoEntry = SIZE_MAX;

                /** @brief What happens at the end. */
                enum class Repeat : uint8_t {
                        Off, ///< Stops after the last.
                        All, ///< Starts again (shuffled again).
                        One, ///< Plays the same entry again (Next still moves on).
                };

                /** @brief What is known of an entry's file. */
                struct Info {
                                StringView title;
                                StringView artist;
                                StringView album;
                                uint32_t   lengthMs = 0;      ///< 0: unknown.
                                bool       estimated = false; ///< The length is an estimate.
                                bool       known = false;     ///< Read from the file (not a list's hint).
                                StringView format;            ///< "MP3", "MIDI"...
                };

                explicit Playlist(MemSpace space = MemSpace::Psram);

                /** @brief Returns the number of entries. */
                size_t count() const { return _entries.size(); }

                /** @brief Returns entry @p index's path. */
                StringView path(size_t index) const { return view(_entries[index].path); }

                /** @brief Returns entry @p index's information (views into the playlist). */
                Info info(size_t index) const;

                /** @brief Returns entry @p index's error (`Ok` unless it failed to play). */
                Error error(size_t index) const { return _entries[index].error; }

                /**
                 * @brief Adds @p path before entry @p at (`count()` or more: at the end).
                 * @return `Ok`, `Full` (`MaxEntries`), `Invalid` (empty, or over 64 KB) or
                 *         `NoMem` (unchanged).
                 */
                Error insert(size_t at, StringView path);

                /** @brief Adds @p path at the end (as `insert()`). */
                Error append(StringView path) { return insert(_entries.size(), path); }

                /** @brief Sets entry @p index's information. @return `Ok` or `NoMem` (unchanged). */
                Error setInfo(size_t index, const Info &info);

                /** @brief Sets entry @p index's error (`Ok` clears it). */
                void setError(size_t index, Error error) {
                        _entries[index].error = error;
                        return;
                }

                /** @brief Removes entry @p index. */
                void remove(size_t index);

                /** @brief Moves entry @p from to place @p to (both below `count()`). */
                void move(size_t from, size_t to);

                /** @brief Removes every entry. */
                void clear();

                /** @brief Returns the entry playing, or `NoEntry`. */
                size_t current() const { return _currentGone ? NoEntry : orderEntry(_at); }

                /** @brief Makes entry @p index the one playing (chosen: a new round if shuffling). */
                void setCurrent(size_t index);

                /** @brief Returns the repeat mode. */
                Repeat repeat() const { return _repeat; }

                /** @brief Sets the repeat mode. */
                void setRepeat(Repeat repeat) {
                        _repeat = repeat;
                        return;
                }

                /** @brief Returns true while shuffling. */
                bool isShuffled() const { return _shuffled; }

                /**
                 * @brief Turns shuffling on (a new order, the current entry first) or off.
                 * @return `Ok` or `NoMem` (off).
                 */
                Error setShuffled(bool shuffled);

                /** @brief Seeds the shuffle's generator (for tests; the player seeds it randomly). */
                void setSeed(uint32_t seed) {
                        _random = seed != 0 ? seed : 1;
                        return;
                }

                /**
                 * @brief The current entry ended (or nothing is current and play starts):
                 *        makes the one that plays now current (Repeat One: the same).
                 * @return It, or `NoEntry` (the end: nothing current, the start next).
                 */
                size_t follow();

                /**
                 * @brief Next (the user's): the next in the order, Repeat One or not.
                 * @return It, or `NoEntry` (past the end with Repeat Off).
                 */
                size_t next();

                /** @brief Previous: the one before in the order. @return It, or `NoEntry`. */
                size_t previous();

        private:
                struct Text {
                                uint32_t at = 0;
                                uint16_t length = 0;
                };

                struct Entry {
                                Text     path;
                                Text     title;
                                Text     artist;
                                Text     album;
                                Text     format;
                                uint32_t lengthMs = 0;
                                Error    error = Error::Ok;
                                bool     estimated = false;
                                bool     known = false;
                };

                StringView view(const Text &t) const { return StringView(_texts.data() + t.at, t.length); }
                Error      store(StringView s, Text &out);
                void       release(const Text &t) { _unused += t.length; }
                void       release(const Entry &e);
                void       compact();
                size_t     orderEntry(size_t position) const; // NoEntry past the end
                size_t     orderSize() const { return _shuffled ? _order.size() : _entries.size(); }
                Error      shuffleFrom(size_t first, size_t avoid); // a new order: first first, avoid not first
                uint32_t   random(uint32_t below);
                size_t     step(bool byUser);

                List<Entry>    _entries;
                List<char>     _texts;
                List<uint32_t> _order; // shuffling: the entries in the order they play
                size_t         _unused = 0;
                size_t         _at = NoEntry;        // the current place in the order; NoEntry: none
                bool           _currentGone = false; // the current entry was removed: _at is the next
                bool           _shuffled = false;
                Repeat         _repeat = Repeat::Off;
                uint32_t       _random = 0x9e3779b9;
};

BMDS_NAMESPACE_END
