/**
 * @file      controlswindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/builtinprograms.h>
#include <bmds/combobox.h>
#include <bmds/config.h>
#include <bmds/detailsview.h>
#include <bmds/guiwindow.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>
#include <bmds/messagebox.h>
#include <bmds/programlauncher.h>
#include <bmds/widget.h>
#include "controlsbook.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The Controls window (`devplan/controls.md` §7d): an application's actions and their
 *        keys and pad controls, each changed by pressing the new one.
 *
 * @par Thread Safety
 * The application's loop, as its widgets.
 */
class ControlsWindow final : public GuiWindow {
        public:
                /** @brief Runs the window on application @p id (empty: the first with actions). */
                static int run(const BuiltinProgram::Context &context, StringView id, StringView home);

                ControlsWindow(GuiApplication &app, const Config &config) : GuiWindow(app, config) {}
                ~ControlsWindow() override;

                /** @brief Makes its widgets, and loads application @p id (empty: the first). */
                Error build(StringView id, StringView home);

        protected:
                bool keyFirst(const KeyEvent &event) override;
                void padEvent(const PadEvent &event) override;

        private:
                // The actions as rows: their label, group, keys and pad controls.
                class Model final : public ItemModel {
                        public:
                                ControlsBook *book = nullptr;

                                size_t      count() const override;
                                StringView  text(size_t row) const override;
                                const Icon *icon(size_t, uint16_t) const override { return nullptr; }
                                size_t      columns() const override { return 4; }
                                StringView  columnTitle(size_t column) const override;
                                StringView  columnText(size_t row, size_t column, Span<char> buffer) const override;
                                uint16_t    columnWidth(size_t column) const override;
                };

                enum class Capture : uint8_t {
                        None,
                        Keys,
                        Pad,
                };

                static void appChanged(void *arg, ComboBox &box, int index);
                static void activated(void *arg, ItemView &view, size_t row);
                static void clicked(void *arg, Button &button);
                static void answered(void *arg, MessageBox::Answer answer);

                void load(StringView id);
                void start(Capture capture, bool adding);
                void captured(StringView text,
                              bool       wholeAxis); // a key or pad control (a pair's end: waits for the other)
                void apply(StringView text);   // the binding chosen: set, conflicts asked about, saved
                void steal(uint64_t others);   // the binding taken from @p others
                void save();
                void stop(StringView status);
                void refresh();
                int  action() const; // the current row's action, or -1

                ControlsBook           *_book = nullptr;
                List<ControlsBook::App> _apps{MemSpace::Psram};
                String                  _home{MemSpace::Psram};
                Model                   _model;
                ComboBox               *_appBox = nullptr;
                DetailsView            *_view = nullptr;
                Label                  *_status = nullptr;
                Capture                 _capture = Capture::None;
                bool                    _adding = false;
                int                     _action = -1;
                bool                    _secondHalf = false; // an axis's: its high end next
                char                    _first[64] = {};     // its low end, captured
                char                    _captured[64] = {};  // what was pressed (asked about)
                char                    _old[256] = {};      // the binding before (put back on No)
                uint64_t                _asking = 0;         // the actions asked to give it up
                uint32_t                _modifier = 0;       // a modifier key down alone, so far
                bool                    _otherKey = false;   // a key went down with it
};

BMDS_NAMESPACE_END
