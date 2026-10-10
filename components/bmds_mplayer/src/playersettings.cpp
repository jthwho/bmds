/**
 * @file      playersettings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * mplayer.conf: what the media player keeps between runs (devplan/mediaplayer.md §6f).
 */

#include "playersettings.h"

#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView RepeatNames[] = {"off", "all", "one"};

        // A whole number, a '-' allowed before it, within [min, max].
        bool parseNumber(StringView text, int64_t min, int64_t max, int64_t &out) {
                const bool negative = text.startsWith('-');
                if (negative) text = text.mid(1);
                if (text.isEmpty() || text.size() > 18) return false;
                int64_t v = 0;
                for (const char c : text) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + (c - '0');
                }
                if (negative) v = -v;
                if (v < min || v > max) return false;
                out = v;
                return true;
        }

        void readBool(StringView text, bool &out) {
                bool b = false;
                if (IniReader::toBool(text, b).isOk()) out = b;
                return;
        }

} // namespace

Error PlayerSettings::parse(StringView text) {
        IniReader reader(text);
        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                if (item.kind != IniReader::Kind::Entry) continue;
                const StringView group = item.name, key = item.key, value = item.value;
                int64_t          n = 0;
                if (group == "Playback") {
                        if (key == "Volume" && parseNumber(value, QuietestDb, 0, n)) {
                                volumeDb = int32_t(n);
                        } else if (key == "Muted") {
                                readBool(value, muted);
                        } else if (key == "Repeat") {
                                for (size_t i = 0; i < 3; ++i)
                                        if (value.equalsIgnoreCase(RepeatNames[i])) repeat = Playlist::Repeat(i);
                        } else if (key == "Shuffle") {
                                readBool(value, shuffled);
                        } else if (key == "Entry" && parseNumber(value, 1, int64_t(Playlist::MaxEntries), n)) {
                                entry = size_t(n - 1);
                        } else if (key == "Position" && parseNumber(value, 0, INT64_MAX / 10, n)) {
                                positionMs = uint64_t(n);
                        }
                } else if (group == "Window") {
                        if (key == "Width" && parseNumber(value, 1, 4000, n)) {
                                width = uint16_t(n);
                        } else if (key == "Height" && parseNumber(value, 1, 4000, n)) {
                                height = uint16_t(n);
                        } else if (key == "Tab" && parseNumber(value, 0, 15, n)) {
                                tab = uint32_t(n);
                        }
                } else if (group == "Folders") {
                        if (key == "Last" && value.startsWith('/') && lastFolder.assign(value).isError())
                                return Error::NoMem;
                }
        }
        return Error::Ok;
}

Error PlayerSettings::write(String &out) const {
        out.clear();
        const StringView r = RepeatNames[size_t(repeat) < 3 ? size_t(repeat) : 0];
        Error            err = out.appendf("# The media player's settings: written by it.\n\n[Playback]\nVolume=%ld\n"
                                                      "Muted=%s\nRepeat=%.*s\nShuffle=%s\n",
                                           long(volumeDb), muted ? "true" : "false", int(r.size()), r.data(),
                                shuffled ? "true" : "false");
        if (err.isOk() && entry != NoEntry)
                err = out.appendf("Entry=%zu\nPosition=%llu\n", entry + 1, static_cast<unsigned long long>(positionMs));
        if (err.isOk()) err = out.append("\n[Window]\n");
        if (err.isOk() && width != 0 && height != 0)
                err = out.appendf("Width=%u\nHeight=%u\n", unsigned(width), unsigned(height));
        if (err.isOk()) err = out.appendf("Tab=%lu\n", static_cast<unsigned long>(tab));
        if (err.isOk() && !lastFolder.isEmpty())
                err = out.appendf("\n[Folders]\nLast=%.*s\n", int(lastFolder.size()), lastFolder.cStr());
        return err.isError() ? Error(Error::NoMem) : Error(Error::Ok);
}

BMDS_NAMESPACE_END
