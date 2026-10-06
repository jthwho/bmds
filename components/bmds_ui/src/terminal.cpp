/**
 * @file      terminal.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The terminal program: a shell in a window (devplan/windows.md §4e).
 */

#include <cstdio>
#include <bmds/font.h>
#include <bmds/memory.h>
#include <bmds/painter.h>
#include <bmds/shell.h>
#include <bmds/staticobject.h>
#include <bmds/terminal.h>
#include <bmds/terminalkeys.h>
#include <bmds/terminalview.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t CellWidth = 8;
        constexpr uint16_t CellHeight = 16;

        constinit StaticObject<Terminal> terminals[Terminal::MaxTerminals];
        constinit ProgramLauncher       *launcher = nullptr;

} // namespace

Result<Terminal *> Terminal::terminal(size_t number) {
        if (number >= MaxTerminals) return makeError<Terminal *>(Error::NotFound);
        if (!terminals[number].isCreated()) terminals[number].create(static_cast<uint8_t>(number));
        return makeResult(&*terminals[number]);
}

Terminal *Terminal::byWindow(uint8_t id) {
        if (id == 0) return nullptr;
        for (StaticObject<Terminal> &t : terminals)
                if (t.isCreated() && t->window() == id) return &*t;
        return nullptr;
}

void Terminal::setLauncher(ProgramLauncher *programs) {
        launcher = programs;
        return;
}

Terminal::Terminal(uint8_t number) : _console(_name, MemSpace::Psram), _number(number) {
        std::snprintf(_name, sizeof(_name), "term%u", number);
        std::snprintf(_title, sizeof(_title), "Terminal %u", number);
}

Result<uint8_t> Terminal::open(WindowManager &wm, const Rect &rect) {
        const uint8_t shown = window();
        if (shown != 0) {
                const Error err = wm.raise(shown);
                return err.isError() ? makeError<uint8_t>(err) : makeResult(shown);
        }
        if (_cells == nullptr) {
                _cells = static_cast<TextCell *>(Memory::alloc(MemSpace::Internal, sizeof(TextCell) * MaxColumns));
                if (_cells == nullptr) return makeError<uint8_t>(Error::NoMem);
        }
        WindowManager::WindowConfig config;
        config.rect = rect;
        config.title = _title;
        config.redraw = redraw;
        config.closed = closed;
        config.key = keyTyped;
        config.resized = resized;
        config.arg = this;
        // Sized for the area the window gets (snapped, kept on the screen) before it draws.
        const Rect client =
                WindowFrame(Rect{rect.x, rect.y, uint16_t((rect.width + 3) & ~3u), rect.height}, config.decorations)
                        .client();
        uint16_t   columns = static_cast<uint16_t>(client.width / CellWidth);
        uint16_t   rows = static_cast<uint16_t>(client.height / CellHeight);
        if (columns > MaxColumns) columns = MaxColumns;
        if (columns == 0 || rows == 0) return makeError<uint8_t>(Error::Invalid);
        Error err = _console.resize(columns, rows);
        if (err.isError()) return makeError<uint8_t>(err);
        _wm = &wm;
        for (uint32_t &w : _cursorRows) w = 0;
        auto [id, oerr] = wm.open(config);
        if (oerr.isError()) return makeError<uint8_t>(oerr);
        _window.store(id, std::memory_order_release);
        auto [placed, perr] = wm.clientRect(id);
        if (perr.isOk() && (placed.width / CellWidth != columns || placed.height / CellHeight != rows)) {
                // Placed smaller than asked (the screen's edge): the size it got.
                columns = static_cast<uint16_t>(placed.width / CellWidth < MaxColumns ? placed.width / CellWidth
                                                                                      : MaxColumns);
                rows = static_cast<uint16_t>(placed.height / CellHeight);
                (void)_console.resize(columns, rows);
                (void)wm.update(id);
        }
        _console.setChangedHook(changed, this);
        if (!_started) {
                Shell::Startup startup;
                startup.launcher = launcher;
                err = Shell::spawn(_console, _name, startup);
                if (err.isError()) return makeError<uint8_t>(err);
                _started = true;
        }
        return makeResult(id);
}

void Terminal::redraw(void *arg, Painter &painter) {
        static_cast<Terminal *>(arg)->draw(painter);
        return;
}

void Terminal::closed(void *arg) {
        static_cast<Terminal *>(arg)->_window.store(0, std::memory_order_release);
        return;
}

// A key, as a terminal sends it, into the shell's input. On the window manager's task.
void Terminal::keyTyped(void *arg, const KeyEvent &event) {
        auto *self = static_cast<Terminal *>(arg);
        bool  application = false;
        {
                ScreenConsole::Locked locked(self->_console);
                application = locked.screen().isApplicationCursorKeys();
        }
        char         bytes[TerminalKeys::MaxBytes];
        const size_t n = TerminalKeys::encode(event, application, bytes);
        if (n > 0) (void)self->_console.pushInput(ByteSpan(reinterpret_cast<const uint8_t *>(bytes), n));
        return;
}

// The client area's new size (on the window manager's task, or the caller's, locked): the
// screen resized to the cells that fit; the window redraws all of it next.
void Terminal::resized(void *arg, uint16_t width, uint16_t height) {
        auto          *self = static_cast<Terminal *>(arg);
        const uint16_t columns = static_cast<uint16_t>(width / CellWidth < MaxColumns ? width / CellWidth : MaxColumns);
        const uint16_t rows = static_cast<uint16_t>(height / CellHeight);
        if (columns > 0 && rows > 0) (void)self->_console.resize(columns, rows);
        return;
}

// Output arrived (on the writer's task): drawn at the next round.
void Terminal::changed(void *arg) {
        auto         *self = static_cast<Terminal *>(arg);
        const uint8_t id = self->window();
        if (id != 0 && self->_wm != nullptr) (void)self->_wm->update(id);
        return;
}

// The rows that changed, those under the damage, those the cursor was drawn
// on and is on; the margin past the cells where damaged. On the window
// manager's task. The screen is locked a row at a time, so output goes on
// while rows are drawn: a row written meanwhile is dirty again, and asks for
// the next round (`drawn()` comes first).
void Terminal::draw(Painter &painter) {
        const Font          &font = Fonts::unscii16;
        const WindowPalette &palette = painter.palette();
        _console.drawn();
        uint16_t columns = 0, rows = 0;
        {
                ScreenConsole::Locked locked(_console);
                columns = locked.screen().columns();
                rows = locked.screen().rows();
        }
        uint32_t   damaged[(TerminalScreen::MaxSide + 31) / 32] = {};
        const Rect cellsArea{0, 0, uint16_t(columns * CellWidth), uint16_t(rows * CellHeight)};
        for (size_t i = 0; i < painter.damageCount(); ++i) {
                const Rect d = painter.damageAt(i).intersected(cellsArea);
                if (d.isEmpty()) continue;
                for (uint32_t r = d.y / CellHeight; r <= (d.bottom() - 1u) / CellHeight; ++r)
                        damaged[r / 32] |= 1u << (r % 32);
        }
        for (uint16_t r = 0; r < rows; ++r) {
                const uint32_t bit = 1u << (r % 32);
                size_t         n = 0;
                bool           cursorHere = false;
                {
                        ScreenConsole::Locked locked(_console);
                        TerminalScreen       &screen = locked.screen();
                        if (screen.rows() != rows || screen.columns() != columns) break; // resized: all dirty
                        cursorHere = screen.isCursorVisible() && screen.cursorRow() == r;
                        const bool need = screen.isRowDirty(r) || (damaged[r / 32] & bit) != 0 ||
                                          (_cursorRows[r / 32] & bit) != 0 || cursorHere;
                        if (!need) continue;
                        n = TerminalView::row(screen, r, palette, cursorHere ? screen.cursorColumn() : -1, _cells,
                                              MaxColumns);
                        screen.clearRowDamage(r);
                }
                if (cursorHere) {
                        _cursorRows[r / 32] |= bit;
                } else {
                        _cursorRows[r / 32] &= ~bit;
                }
                (void)painter.cells(0, static_cast<uint16_t>(r * CellHeight), Span<const TextCell>(_cells, n), font,
                                    cellsArea.width);
        }
        // The margins right of and below the cells, where damaged.
        const uint32_t back = palette.color(WindowPalette::Role::TerminalBackground);
        for (size_t i = 0; i < painter.damageCount(); ++i) {
                const Rect d = painter.damageAt(i);
                if (d.right() > cellsArea.width) {
                        const uint16_t x = d.x > cellsArea.width ? d.x : cellsArea.width;
                        (void)painter.fill(Rect{x, d.y, uint16_t(d.right() - x), d.height}, back);
                }
                if (d.bottom() > cellsArea.height) {
                        const uint16_t y = d.y > cellsArea.height ? d.y : cellsArea.height;
                        (void)painter.fill(Rect{d.x, y, d.width, uint16_t(d.bottom() - y)}, back);
                }
        }
        return;
}

BMDS_NAMESPACE_END
