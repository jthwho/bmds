/**
 * @file      iopin.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One pad as people are shown it (devplan/io.md §3.10, §3.11): its row in a
 * table (`io`, the I/O Manager's Pins tab) and its details (`io pin N`, the
 * inspector). Made from what the registry, the board and the pad's registers
 * say; pure, host-tested. `Io::readPins()` and `Io::readPin()` (<bmds/io.h>)
 * make them on the system.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/iochip.h>
#include <bmds/iopad.h>
#include <bmds/ioregistry.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/** @brief Input signals one pad feeds through the matrix, at most (a pad usually feeds none or one). */
inline constexpr size_t IoMaxPadInputs = 8;

/**
 * @brief One pad's row: what a table of every pad shows.
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoPinRow {
                static constexpr size_t NameBytes = 8;      ///< `GPIO54` and its NUL.
                static constexpr size_t FunctionBytes = 32; ///< The use's text, NUL included.
                static constexpr size_t WhereBytes = 16;    ///< `EXT1.20` and its NUL.

                uint8_t     pad = 0;                         ///< Its number.
                IoClass     resourceClass = IoClass::Free;   ///< Its class.
                char        level = '-';                     ///< `ioLevelChar()`.
                char        name[NameBytes] = {};            ///< `GPIO14`.
                char        owner[IoHolder::NameBytes] = {}; ///< Who holds it, or "".
                char        function[FunctionBytes] = {};    ///< The holder's role, else the board's label, or "".
                char        route[IoRouteTextBytes] = {};    ///< `ioRouteText()`.
                const char *direction = "-";                 ///< `ioDirectionText()` (rodata).
                const char *pull = "-";                      ///< `ioPullText()` (rodata).
                char        where[WhereBytes] = {}; ///< The connector pin it is brought out to (`UEXT.3`), or "".
                IoPadState  state;                  ///< What its registers say.
                bool        strapping = false;      ///< A strapping pin (the chip's).
                bool        forced = false;         ///< Taken with force from its holder.
};

/** @brief A line of a pad's details (`IoPinDetail`), in the order shown. */
enum class IoPinField : uint8_t {
        Board,     ///< The board's label and connector pin.
        Note,      ///< The board's note (empty: not shown).
        Class,     ///< Its class.
        Owner,     ///< Its holder and role.
        Forced,    ///< Who it was forced from (empty: not shown).
        Chip,      ///< Strapping, LP IO, analog, the chip's note.
        Functions, ///< The IO_MUX functions, the selected one starred.
        Route,     ///< `ioRouteText()`.
        Direction, ///< `ioDirectionText()`.
        Output,    ///< The matrix output signal, enable, latch, open drain.
        Input,     ///< Input enable, level, the matrix inputs it feeds.
        Pull,      ///< `ioPullText()`.
        Drive,     ///< Drive strength, 0-3.
        LpIo,      ///< Whether the LP IO domain has it.
};

/** @brief The number of `IoPinField`s. */
inline constexpr size_t IoPinFieldCount = 14;

/**
 * @brief Everything about one pad, as lines of text by `IoPinField`.
 *
 * @par Thread Safety
 * A plain value.
 */
struct IoPinDetail {
                static constexpr size_t TextBytes = 128; ///< A line's room, NUL included.

                uint8_t pad = 0;                               ///< Its number.
                char    name[IoPinRow::NameBytes] = {};        ///< `GPIO14`.
                char    text[IoPinFieldCount][TextBytes] = {}; ///< Each field's line; "" for one not shown.

                /** @brief Field @p field's line. */
                const char *at(IoPinField field) const { return text[static_cast<size_t>(field)]; }
};

/** @brief What a pad's board wiring says of it (from `BoardPin`, `BoardConnector`). */
struct IoPinBoard {
                const char *label = "";                ///< The board's label (`RMII TXD1`), or "".
                const char *note = "";                 ///< The board's note, or "".
                char where[IoPinRow::WhereBytes] = {}; ///< The connector pin it is brought out to (`UEXT.3`), or "".
};

/**
 * @brief The level worth showing: `1`/`0` read at the pad while its input is
 *        on, else the output latch's while it drives, `-` for neither, `?`
 *        for an LP IO pad (its level is the LP domain's).
 */
char ioLevelChar(const IoPadState &state);

/**
 * @brief The input signals pad @p pad feeds, from every input signal's
 *        source (`Io::readInputSources()`'s), into @p out.
 * @return How many (at most `out.size()`).
 */
size_t ioPadInputs(Span<const int16_t> sources, unsigned pad, Span<uint16_t> out);

/**
 * @brief Pad @p pad's row.
 * @param inputs The input signals it feeds (`ioPadInputs()`).
 */
void ioPinRow(const IoChip &chip, unsigned pad, const IoPadState &state, Span<const uint16_t> inputs,
              const IoHolder &holder, const IoPinBoard &board, IoPinRow &out);

/**
 * @brief Pad @p pad's details.
 * @param inputs The input signals it feeds (`ioPadInputs()`).
 */
void ioPinDetail(const IoChip &chip, unsigned pad, const IoPadState &state, Span<const uint16_t> inputs,
                 const IoHolder &holder, const IoPinBoard &board, IoPinDetail &out);

/** @brief The field's name (`Board`, `Functions`, `LP IO`). */
const char *ioPinFieldName(IoPinField field);

BMDS_NAMESPACE_END
