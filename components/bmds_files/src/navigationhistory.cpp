/**
 * @file      navigationhistory.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A window's Back and Forward (devplan/filebrowser.md §4h).
 */

#include <utility>
#include <bmds/navigationhistory.h>

BMDS_NAMESPACE_BEGIN

Error NavigationHistory::visit(StringView path) {
        if (!_paths.isEmpty() && _paths[_at].view() == path) return Error::Ok;
        String entry(MemSpace::Psram);
        if (entry.assign(path).isError()) return Error::NoMem;
        if (_paths.reserve(_paths.isEmpty() ? 1 : _at + 2).isError()) return Error::NoMem;
        if (!_paths.isEmpty()) _paths.truncate(_at + 1); // what was ahead
        (void)_paths.append(std::move(entry));
        if (_paths.size() > MaxKept) _paths.removeAt(0);
        _at = _paths.size() - 1;
        return Error::Ok;
}

StringView NavigationHistory::back() {
        if (!canGoBack()) return StringView();
        --_at;
        return _paths[_at].view();
}

StringView NavigationHistory::forward() {
        if (!canGoForward()) return StringView();
        ++_at;
        return _paths[_at].view();
}

StringView NavigationHistory::parentOf(StringView path) {
        while (path.size() > 1 && path.endsWith('/')) path = path.left(path.size() - 1);
        const size_t slash = path.lastIndexOf('/');
        if (slash == StringView::NPos) return StringView();
        return slash == 0 ? path.left(1) : path.left(slash);
}

BMDS_NAMESPACE_END
