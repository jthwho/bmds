/**
 * @file      settingstable.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Settings' pages as data (`devplan/controlpanel.md` §5, §8): each
 *        group's sections and rows, where every system setting is placed,
 *        and the words shown for them. Pure: the host tests read it.
 *
 * A row names a key, or a pattern whose `*` stands for one component
 * (`net/ * /ipv4/mode`, `wm/palette/ *`: a row for each key it matches, in key
 * order). A row without a key is a section's heading.
 *
 * @par Thread Safety
 * Rodata and reentrant functions.
 */
namespace SettingsTable {

        /** @brief How a row is edited, where its type alone doesn't say. */
        enum class Editor : uint8_t {
                Auto,     ///< By its type (`devplan/controlpanel.md` §6.1).
                Slider,   ///< An `int` as a slider, its value and unit beside it.
                List,     ///< A `string` as a list (`separator` between its items).
                Folder,   ///< A `path` or `string` with Choose... picking a folder.
                File,     ///< As `Folder`, picking a file.
                Zone,     ///< A time zone (`devplan/controlpanel.md` §6.4).
                Mode,     ///< The display's mode (its list, and Custom).
                Password, ///< A `secret` set through Set Password... (`Password::set()`).
        };

        /** @brief A row's flags. */
        enum Flags : uint8_t {
                Confirm = 1, ///< Asks "Keep this setting?" and reverts by itself (§6.3).
                AtStart = 2, ///< Read by the next `display start`.
                AtBoot = 4,  ///< Read at boot.
                Empty = 8,   ///< May be empty: a `Zone` offers "Same as the system".
        };

        /** @brief A row of a page, or a section's heading (`key` null, `label` its title). */
        struct Row {
                        const char *key = nullptr;   ///< A key, or a pattern with `*` components.
                        const char *label = nullptr; ///< Null: the key's last component made a label; `%s`: the `*`.
                        const char *help = nullptr;  ///< Null: the key's own.
                        const char *choices =
                                nullptr;            ///< An enum's words' labels: `word=Label|...` (null: capitalized).
                        const char *unit = nullptr; ///< After a number ("ms").
                        Editor      editor = Editor::Auto;
                        uint8_t     flags = 0;
                        char        separator = ';'; ///< A list's, between its items.
                        /**
                         * When it applies (shown only then): conditions joined by `,`, each
                         * `KEY=VALUE[|VALUE...]`, `KEY!=VALUE` or `KEY~ITEM` (a `;` list holding
                         * ITEM). A `*` in KEY is the row's (null: always).
                         */
                        const char *when = nullptr;
        };

        /** @brief A group's page. */
        struct Page {
                        const char     *id;    ///< `settings ID` opens it.
                        const char     *title; ///< In the sidebar and over the page.
                        const char     *icon;  ///< A freedesktop icon name.
                        Span<const Row> rows;  ///< Empty: All Settings.
        };

        /** @brief Returns the pages, in the sidebar's order (All Settings last). */
        Span<const Page> pages();

        /** @brief Returns the page whose id is @p id, or -1. */
        int findPage(StringView id);

        /**
         * @brief Returns true if @p key matches @p pattern (`*` one component),
         *        with @p star set to what the first `*` stood for (empty: none).
         */
        bool matches(StringView pattern, StringView key, StringView &star);

        /** @brief Where @p key is placed: its page and row. */
        struct Place {
                        int        page = -1; ///< -1: on no page (All Settings only).
                        const Row *row = nullptr;
                        const Row *section = nullptr; ///< The heading over it, or null.
                        StringView star;              ///< What its row's `*` stood for.
        };

        /**
         * @brief Returns where @p key is placed: the row naming it, else the first
         *        pattern matching it. A page shows a key only on the row placing it.
         */
        Place place(StringView key);

        /**
         * @brief Writes @p row's label for @p key into @p out: its own (its `%s` the
         *        `*`), else @p key's last component made a label ("active-frame":
         *        "Active frame"). @return Its length.
         */
        size_t label(const Row &row, StringView key, Span<char> out);

        /** @brief Writes @p word made a label into @p out ("cpu-graph": "Cpu graph"). @return Its length. */
        size_t labelOf(StringView word, Span<char> out);

        /**
         * @brief Writes the label @p choices give enum word @p word into @p out, else
         *        @p word with its first letter capitalized. @return Its length.
         */
        size_t choiceLabel(const char *choices, StringView word, Span<char> out);

        /** @brief Reads a setting's current value for `applies()`: false if there is no such key. */
        using ValueOf = bool (*)(void *arg, StringView key, char *out, size_t size);

        /**
         * @brief Returns true if every condition of @p when holds (null or empty: true), each
         *        key's `*` @p star, read through @p valueOf. A key that can't be read fails it.
         */
        bool applies(const char *when, StringView star, ValueOf valueOf, void *arg);

        /** @brief Returns true if @p text holds @p needle, ignoring ASCII case (an empty needle: true). */
        bool containsFolded(StringView text, StringView needle);

} // namespace SettingsTable

BMDS_NAMESPACE_END
