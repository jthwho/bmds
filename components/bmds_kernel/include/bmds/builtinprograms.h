/**
 * @file      builtinprograms.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/programlauncher.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/** @brief A program built into the firmware, by name (`BMDS_PROGRAM`). */
struct ProgramEntry {
                const char    *name; ///< Its name in `/bin`; an interface once shipped.
                BuiltinProgram program;
};

/**
 * @brief The programs built into the firmware: what `/bin` lists and runs
 *        (`devplan/programs.md` §17).
 *
 * A program registers itself where it is defined, with `BMDS_PROGRAM`;
 * nothing else lists them. On the target the entries are collected from a
 * linker section, sorted by name; the host tests supply their own.
 *
 * @par Thread Safety
 * Immutable: safe from any task.
 */
namespace BuiltinPrograms {

        /** @brief Returns every entry, sorted by name. */
        Span<const ProgramEntry> all();

        /** @brief Returns the entry named @p name, or null. */
        const ProgramEntry *find(StringView name);

        namespace detail {
                /** @brief Host tests: the entries `all()` returns (sorted by name). */
                void setForHost(Span<const ProgramEntry> entries);
        } // namespace detail

} // namespace BuiltinPrograms

BMDS_NAMESPACE_END

/**
 * @def BMDS_PROGRAM
 * @brief Builds a program into the firmware as `/bin/Name` (a name that is
 *        an identifier; an alias is a second entry with the same @p Main).
 * @param Name       Its name.
 * @param Main       `int (*)(const BuiltinProgram::Context &)`.
 * @param StackBytes Its task's stack.
 */
#define BMDS_PROGRAM(Name, Main, StackBytes) BMDS_PROGRAM_AS(Name, #Name, Main, StackBytes)

/**
 * @def BMDS_PROGRAM_AS
 * @brief `BMDS_PROGRAM` for a name that isn't an identifier (`[`).
 * @param Id         An identifier for the object.
 * @param Name       Its name, a string literal.
 */
#define BMDS_PROGRAM_AS(Id, Name, Main, StackBytes)                                                                    \
        [[gnu::used, gnu::section(".bmds_prog." Name)]] constexpr ::bmds::ProgramEntry bmdsProgram_##Id = {Name,       \
                                                                                                          {Main, StackBytes}};

