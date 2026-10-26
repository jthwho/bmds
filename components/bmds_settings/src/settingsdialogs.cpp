/**
 * @file      settingsdialogs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Settings' own dialogs: a time zone from the database's folders, a password asked twice
 * (devplan/controlpanel.md §5, §6.4).
 */

#include <cstdio>
#include <cstring>
#include <bmds/detailsview.h>
#include <bmds/formlayout.h>
#include <bmds/lineedit.h>
#include <bmds/password.h>
#include <bmds/shacrypt.h>
#include <bmds/vfs.h>

#include "settingsdialogs.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView SameAsSystem("Same as the system");
        constexpr StringView Other("Other");

        // The names in folder @p path, folders' (or files') only, in the order read.
        template <typename Fn> void eachName(StringView path, bool folders, Fn fn) {
                auto [dir, err] = Vfs::openDirectory(path, Vfs::Context{"/", nullptr});
                if (err.isError()) return;
                for (;;) {
                        Result<FileInfo> e = dir.next();
                        if (e.isError()) break;
                        const FileInfo  &info = e.value();
                        const StringView name = info.name.view();
                        if (name.isEmpty() || name.startsWith('.')) continue;
                        if ((info.type == FileType::Directory) == folders) fn(name);
                }
                return;
        }

        int rowOf(const ListItemModel &m, StringView text) {
                for (size_t i = 0; i < m.count(); ++i)
                        if (m.text(i) == text) return int(i);
                return -1;
        }

} // namespace

// ---- ZoneDialog

ZoneDialog::ZoneDialog(GuiApplication &app) : Dialog(app, "Time Zone", 440, 280) {}

ZoneDialog::~ZoneDialog() = default;

Error ZoneDialog::build() {
        auto *row = Widget::create<BoxLayout>(content(), BoxLayout::Direction::Horizontal);
        if (row == nullptr) return Error::NoMem;
        row->setStretch(1);
        row->setSpacing(8);
        _regionView = Widget::create<DetailsView>(row);
        _placeView = Widget::create<DetailsView>(row);
        if (_regionView == nullptr || _placeView == nullptr) return Error::NoMem;
        const StringView       regionTitle[1] = {"Region"};
        const StringView       placeTitle[1] = {"Place"};
        const uint16_t         regionWidth[1] = {150};
        const uint16_t         placeWidth[1] = {230};
        const ItemModel::Align left[1] = {ItemModel::Align::Left};
        _regions.setColumns(Span<const StringView>(regionTitle), Span<const uint16_t>(regionWidth),
                            Span<const ItemModel::Align>(left));
        _places.setColumns(Span<const StringView>(placeTitle), Span<const uint16_t>(placeWidth),
                           Span<const ItemModel::Align>(left));
        for (DetailsView *v : {_regionView, _placeView}) {
                v->setSingleSelection(true);
                v->setStretch(1);
        }
        _regionView->setMaximumWidth(176);
        _regionView->setModel(&_regions);
        _placeView->setModel(&_places);
        _regionView->setChanged(regionChanged, this);
        _placeView->setActivated(placeChosen, this);
        (void)addButton("OK", Accepted);
        (void)addButton("Cancel", Rejected);
        setCancelResult(Rejected);
        return Error::Ok;
}

void ZoneDialog::fillRegions(StringView select) {
        _filling = true;
        _regions.clear();
        if (_allowEmpty) (void)_regions.add(SameAsSystem, nullptr, nullptr);
        eachName(StringView(ZoneDir), true, [this](StringView name) { (void)_regions.add(name, nullptr, nullptr); });
        (void)_regions.add(Other, nullptr, nullptr);
        int row = rowOf(_regions, select);
        if (row < 0) row = 0;
        const size_t r = size_t(row);
        _regionView->resetTo(Span<const size_t>(&r, 1), row);
        _regionView->showRow(r);
        _filling = false;
        return;
}

bool ZoneDialog::regionIsOther(size_t row) const {
        return row + 1 == _regions.count(); // the last
}

void ZoneDialog::fillPlaces(StringView select) {
        _places.clear();
        const int cur = _regionView->current();
        if (cur >= 0 && !(_allowEmpty && cur == 0)) {
                if (regionIsOther(size_t(cur))) {
                        eachName(StringView(ZoneDir), false,
                                 [this](StringView name) { (void)_places.add(name, nullptr, nullptr); });
                } else {
                        // The region's places, and those a level down (America/Argentina/Salta).
                        char path[96];
                        std::snprintf(path, sizeof(path), "%s/%.*s", ZoneDir, int(_regions.text(size_t(cur)).size()),
                                      _regions.text(size_t(cur)).data());
                        eachName(StringView(path), false,
                                 [this](StringView name) { (void)_places.add(name, nullptr, nullptr); });
                        eachName(StringView(path), true, [this, &path](StringView sub) {
                                char inner[128];
                                std::snprintf(inner, sizeof(inner), "%s/%.*s", path, int(sub.size()), sub.data());
                                eachName(StringView(inner), false, [this, sub](StringView name) {
                                        char      both[64];
                                        const int n = std::snprintf(both, sizeof(both), "%.*s/%.*s", int(sub.size()),
                                                                    sub.data(), int(name.size()), name.data());
                                        if (n > 0 && size_t(n) < sizeof(both))
                                                (void)_places.add(StringView(both, size_t(n)), nullptr, nullptr);
                                });
                        });
                }
        }
        const int    row = rowOf(_places, select);
        const size_t r = row >= 0 ? size_t(row) : 0;
        _placeView->resetTo(Span<const size_t>(&r, row >= 0 ? 1 : 0), row);
        if (row >= 0) _placeView->showRow(r);
        return;
}

Error ZoneDialog::open(StringView zone, bool allowEmpty) {
        _allowEmpty = allowEmpty;
        // Region/Place, else a zone not in a region (Other), else the system's.
        const size_t slash = zone.indexOf('/');
        StringView   region, place;
        if (zone.isEmpty() && allowEmpty) {
                region = SameAsSystem;
        } else if (slash != StringView::NPos) {
                region = zone.left(slash);
                place = zone.mid(slash + 1);
        } else {
                region = Other;
                place = zone;
        }
        fillRegions(region);
        fillPlaces(place);
        return Dialog::open();
}

StringView ZoneDialog::zone() {
        _chosen.clear();
        const int r = _regionView->current(), p = _placeView->current();
        if (r < 0 || (_allowEmpty && r == 0)) return _chosen.view(); // the system's
        if (p < 0) return _chosen.view();
        if (!regionIsOther(size_t(r))) {
                (void)_chosen.append(_regions.text(size_t(r)));
                (void)_chosen.append('/');
        }
        (void)_chosen.append(_places.text(size_t(p)));
        return _chosen.view();
}

bool ZoneDialog::hasChoice() const {
        const int r = _regionView->current();
        return (r >= 0 && _allowEmpty && r == 0) || _placeView->current() >= 0;
}

void ZoneDialog::regionChanged(void *arg, ItemView &) {
        auto *self = static_cast<ZoneDialog *>(arg);
        if (!self->_filling) self->fillPlaces(StringView());
        return;
}

void ZoneDialog::placeChosen(void *arg, ItemView &, size_t) {
        static_cast<ZoneDialog *>(arg)->finish(Accepted);
        return;
}

// ---- PasswordDialog

PasswordDialog::PasswordDialog(GuiApplication &app) : Dialog(app, "Set Password", 340, 56) {}

Error PasswordDialog::build() {
        auto *form = Widget::create<BoxLayout>(content(), BoxLayout::Direction::Vertical);
        if (form == nullptr) return Error::NoMem;
        form->setSpacing(8);
        for (int i = 0; i < 2; ++i) {
                auto *row = Widget::create<BoxLayout>(form, BoxLayout::Direction::Horizontal);
                if (row == nullptr) return Error::NoMem;
                row->setSpacing(8);
                auto *label = Widget::create<Label>(row, i == 0 ? "&New password" : "&Again");
                auto *edit = Widget::create<LineEdit>(row);
                if (label == nullptr || edit == nullptr) return Error::NoMem;
                label->setMinimumWidth(128);
                label->setBuddy(edit);
                edit->setStretch(1);
                edit->setMasked(true);
                edit->setMaxLength(ShaCrypt::MaxPassword);
                (i == 0 ? _first : _second) = edit;
        }
        (void)addButton("OK", 1);
        (void)addButton("Cancel", 0);
        setCancelResult(0);
        return Error::Ok;
}

Error PasswordDialog::open() {
        _first->setText(StringView());
        _second->setText(StringView());
        const Error err = Dialog::open();
        if (err.isOk()) setFocusWidget(_first);
        return err;
}

Error PasswordDialog::take(const char *&problem) {
        Error err = Error::Ok;
        problem = nullptr;
        const StringView a = _first->text(), b = _second->text();
        if (a.isEmpty()) {
                problem = "empty: unchanged (Remove takes it away)";
                err = Error::Invalid;
        } else if (a != b) {
                problem = "the two differ: unchanged";
                err = Error::Invalid;
        } else {
                err = Password::set(a);
                if (err.isError()) problem = err.desc();
        }
        _first->setText(StringView());
        _second->setText(StringView());
        return err;
}

// ---- NameDialog

NameDialog::NameDialog(GuiApplication &app) : Dialog(app, "Name", 340, 28) {}

Error NameDialog::build() {
        auto *row = Widget::create<BoxLayout>(content(), BoxLayout::Direction::Horizontal);
        if (row == nullptr) return Error::NoMem;
        row->setSpacing(8);
        _prompt = Widget::create<Label>(row, "&Name:");
        _edit = Widget::create<LineEdit>(row);
        if (_prompt == nullptr || _edit == nullptr) return Error::NoMem;
        _prompt->setBuddy(_edit);
        _edit->setStretch(1);
        _edit->setMaxLength(64);
        (void)addButton("OK", Accepted);
        (void)addButton("Cancel", Rejected);
        setCancelResult(Rejected);
        return Error::Ok;
}

Error NameDialog::open(StringView title, StringView prompt, StringView initial) {
        setTitle(title);
        _prompt->setText(prompt);
        _edit->setText(initial);
        const Error err = Dialog::open();
        if (err.isOk()) setFocusWidget(_edit);
        return err;
}

StringView NameDialog::name() const {
        return _edit->text().trimmed();
}

BMDS_NAMESPACE_END
