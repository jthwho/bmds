/**
 * @file      treeview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/itemview.h>
#include <bmds/treerows.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A tree of folders (`devplan/filebrowser.md` §4g): indented rows with
 *        + and − boxes, as Windows' tree.
 *
 * Its rows are its own (`TreeRows`): roots added with `addRoot()`, children
 * given when a row is expanded (`Expanding` asks; the owner calls
 * `setChildren()`). A press on a box, a double click, +, − and the arrows
 * expand and collapse (Left on a collapsed row goes to its parent, Right on
 * an expanded one to its first child). One row is selected at most;
 * `Changed` tells when it moves.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class TreeView : public ItemView {
        public:
                /** @brief Told that row @p row is being expanded: the owner gives its children. */
                using Expanding = void (*)(void *arg, TreeView &view, size_t row);

                /** @brief Told that the rows changed (expanded, collapsed, given, removed). */
                using RowsChanged = void (*)(void *arg, TreeView &view);

                static constexpr uint16_t RowHeight = 18;
                static constexpr uint16_t Indent = 16;

                explicit TreeView(Widget *parent);

                /** @brief Sets the icons: a root's, a folder's, an expanded folder's (the caller's; null: none). */
                void setIcons(const Icon *root, const Icon *folder, const Icon *open);

                /**
                 * @brief Draws a root's child named @p name with @p icon (the caller's; null:
                 *        none) rather than a folder's: `/trash` in the file browser's tree.
                 */
                void setChildIcon(StringView name, const Icon *icon);

                /** @brief `setPathIcon()`'s paths at most. */
                static constexpr size_t MaxPathIcons = 4;

                /**
                 * @brief Draws the row whose path is @p path (shown or not yet) with @p icon (the
                 *        caller's) rather than a folder's: a network mount's in the file browser.
                 * @return `Ok`, `Full` (`MaxPathIcons` paths already), `Invalid` (too long).
                 */
                Error setPathIcon(StringView path, const Icon *icon);

                /** @brief Forgets every `setPathIcon()`. */
                void clearPathIcons();

                /** @brief Adds a root (its name a path, `/sd0/p0`). @return `Ok` or `NoMem`. */
                Error addRoot(StringView path);

                /**
                 * @brief Shows @p names as @p row's children and expands it (none: it
                 *        has no children); the selected row kept.
                 * @return `Ok`, `OutOfRange`, or `NoMem`.
                 */
                Error setChildren(size_t row, Span<const StringView> names);

                /** @brief Expands @p row (asks `Expanding`), if it may have children. */
                void expand(size_t row);

                /** @brief Collapses @p row; the selection moves to it if it was under it. */
                void collapse(size_t row);

                /** @brief Removes every row. */
                void clear();

                /** @brief Returns the rows. */
                const TreeRows &rows() const { return _rows; }

                /** @brief Selects the row whose path is @p path, if shown. @return Its row, or -1. */
                int selectPath(StringView path);

                void setExpanding(Expanding fn, void *arg) {
                        _expanding = fn;
                        _expandingArg = arg;
                        return;
                }

                /** @brief Sets what is told of any change to the rows (after it). */
                void setRowsChanged(RowsChanged fn, void *arg) {
                        _rowsChanged = fn;
                        _rowsChangedArg = arg;
                        return;
                }

                Rect nameRect(size_t row) const override;
                void mousePressed(const WindowManager::MouseEvent &event) override;
                bool keyPressed(const KeyEvent &event) override;

        protected:
                int         rowAt(uint32_t x, uint32_t y) const override;
                bool        onName(size_t row, uint32_t x, uint32_t y) const override;
                ContentRect rowRect(size_t row) const override;
                size_t      neighbour(size_t row, int dx, int dy) const override;
                void        relayout() override;
                void        activate(size_t row) override;
                void        paintRows(Painter &painter, const Rect &viewport) override;
                void        dropHeld(size_t row) override { expand(row); } // a drag held over a folder opens it

        private:
                // The rows as a model.
                class Model final : public ItemModel {
                        public:
                                explicit Model(const TreeRows &rows) : _rows(rows) {}
                                size_t      count() const override { return _rows.count(); }
                                StringView  text(size_t row) const override { return _rows.name(row); }
                                const Icon *icon(size_t, uint16_t) const override { return nullptr; }

                        private:
                                const TreeRows &_rows;
                };

                static uint32_t textX(uint8_t depth) { return uint32_t(depth) * Indent + Indent + 16 + 4; }
                bool            onBox(size_t row, uint32_t x) const;
                void            changedRows(int keep); // rows changed: the model reset, row @p keep selected

                TreeRows    _rows;
                Model       _model{_rows};
                const Icon *_rootIcon = nullptr;
                const Icon *_folderIcon = nullptr;
                const Icon *_openIcon = nullptr;
                const Icon *_childIcon = nullptr; // setChildIcon()'s, for a root's child named _childName
                char        _childName[32] = {};
                struct PathIcon {
                                const Icon *icon = nullptr; // null: the entry is free
                                char        path[64] = {};  // Vfs::MountInfo's
                };
                PathIcon    _pathIcons[MaxPathIcons];
                void        tellRowsChanged() {
                        if (_rowsChanged != nullptr) _rowsChanged(_rowsChangedArg, *this);
                        return;
                }

                Expanding   _expanding = nullptr;
                void       *_expandingArg = nullptr;
                RowsChanged _rowsChanged = nullptr;
                void       *_rowsChangedArg = nullptr;
};

BMDS_NAMESPACE_END
