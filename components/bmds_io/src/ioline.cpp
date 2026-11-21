/**
 * @file      ioline.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A line request's config checked by Linux's rules (devplan/io.md §12.1).
 */

#include <bmds/ioline.h>

BMDS_NAMESPACE_BEGIN

namespace IoLine {

        namespace {

                using C = IoLineConfig;

                constexpr uint64_t Valid = C::ActiveLow | C::Input | C::Output | C::EdgeRising | C::EdgeFalling |
                                           C::OpenDrain | C::OpenSource | C::PullUp | C::PullDown | C::BiasDisabled |
                                           C::ClockRealtime | C::ClockHte;
                constexpr uint64_t Edges = C::EdgeRising | C::EdgeFalling;
                constexpr uint64_t Drives = C::OpenDrain | C::OpenSource;
                constexpr uint64_t Biases = C::PullUp | C::PullDown | C::BiasDisabled;
                constexpr uint32_t MaxDebounceUs = 1000000;

                int bits(uint64_t v) {
                        int n = 0;
                        for (; v != 0; v &= v - 1) ++n;
                        return n;
                }

        } // namespace

        Error checkFlags(uint64_t flags) {
                if ((flags & ~Valid) != 0) return Error::Invalid;
                if ((flags & C::ClockRealtime) && (flags & C::ClockHte)) return Error::Invalid;
                if ((flags & C::Input) && (flags & C::Output)) return Error::Invalid;
                if ((flags & Edges) && !(flags & C::Input)) return Error::Invalid;
                if ((flags & Drives) && !(flags & C::Output)) return Error::Invalid;
                if (bits(flags & Drives) > 1) return Error::Invalid;
                if ((flags & Biases) && !(flags & (C::Input | C::Output))) return Error::Invalid;
                if (bits(flags & Biases) > 1) return Error::Invalid;
                if (flags & C::ClockHte) return Error::NotSupported; // no hardware timestamping engine
                return Error::Ok;
        }

        Error resolve(const IoLineConfig &config, size_t lineCount, IoLineSettings *out) {
                if (lineCount == 0 || lineCount > C::MaxLines || config.attrCount > C::MaxAttrs) return Error::Invalid;
                for (uint32_t p : config.padding)
                        if (p != 0) return Error::Invalid;
                for (uint32_t a = 0; a < config.attrCount; ++a) {
                        const C::Attr &attr = config.attrs[a];
                        if (attr.padding != 0) return Error::Invalid;
                        if (attr.id == C::AttrFlags) {
                                if (const Error err = checkFlags(attr.value); err.isError()) return err;
                        } else if (attr.id == C::AttrDebounce) {
                                if (attr.value > MaxDebounceUs) return Error::Invalid;
                        } else if (attr.id != C::AttrOutputValues) {
                                return Error::Invalid;
                        }
                }
                if (const Error err = checkFlags(config.flags); err.isError()) return err;
                for (size_t i = 0; i < lineCount; ++i) {
                        const uint64_t bit = 1ull << i;
                        uint64_t       flags = config.flags;
                        bool           flagsFound = false, valueFound = false, debounceFound = false;
                        IoLineSettings s;
                        for (uint32_t a = 0; a < config.attrCount; ++a) {
                                const C::Attr &attr = config.attrs[a];
                                if ((attr.mask & bit) == 0) continue;
                                if (attr.id == C::AttrFlags && !flagsFound) {
                                        flags = attr.value;
                                        flagsFound = true;
                                } else if (attr.id == C::AttrOutputValues && !valueFound) {
                                        s.value = (attr.value & bit) != 0;
                                        valueFound = true;
                                } else if (attr.id == C::AttrDebounce && !debounceFound) {
                                        s.debounceUs = uint32_t(attr.value);
                                        debounceFound = true;
                                }
                        }
                        s.direction = (flags & C::Input)    ? IoLineSettings::Direction::Input
                                      : (flags & C::Output) ? IoLineSettings::Direction::Output
                                                            : IoLineSettings::Direction::AsIs;
                        s.drive = (flags & C::OpenDrain)    ? IoLineSettings::Drive::OpenDrain
                                  : (flags & C::OpenSource) ? IoLineSettings::Drive::OpenSource
                                                            : IoLineSettings::Drive::PushPull;
                        s.bias = (flags & C::PullUp)         ? IoLineSettings::Bias::PullUp
                                 : (flags & C::PullDown)     ? IoLineSettings::Bias::PullDown
                                 : (flags & C::BiasDisabled) ? IoLineSettings::Bias::Disabled
                                                             : IoLineSettings::Bias::AsIs;
                        s.activeLow = (flags & C::ActiveLow) != 0;
                        s.edges = uint8_t(((flags & C::EdgeRising) ? IoLineSettings::Rising : 0) |
                                          ((flags & C::EdgeFalling) ? IoLineSettings::Falling : 0));
                        s.realtime = (flags & C::ClockRealtime) != 0;
                        if (s.direction != IoLineSettings::Direction::Output) s.value = false;
                        if (s.direction != IoLineSettings::Direction::Input) s.debounceUs = 0;
                        out[i] = s;
                }
                return Error::Ok;
        }

} // namespace IoLine

BMDS_NAMESPACE_END
