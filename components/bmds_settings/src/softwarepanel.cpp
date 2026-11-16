/**
 * @file      softwarepanel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Software page's sources and keys (devplan/thirdparty.md §5), over `pkg --json`.
 */

#include <cstdio>
#include <bmds/detailsview.h>
#include <bmds/formlayout.h>
#include <bmds/guiapplication.h>
#include <bmds/lineedit.h>
#include "settingswindow.h"
#include "softwarepanel.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // A list as tall as this many rows, its header too: a page's row (FormLayout asks).
        class ShortList final : public DetailsView {
                public:
                        ShortList(Widget *parent, uint16_t rows) : DetailsView(parent), _rows(rows) {}

                        GuiSize sizeHint() const override {
                                return GuiSize{200, uint16_t(headerHeight() + _rows * rowHeight() + 4)};
                        }

                private:
                        uint16_t _rows;
        };

        // @p text quoted for the shell: in '', a ' as '\''.
        void quoted(String &out, StringView text) {
                (void)out.append('\'');
                for (char c : text) {
                        if (c == '\'')
                                (void)out.append("'\\''");
                        else
                                (void)out.append(c);
                }
                (void)out.append('\'');
                return;
        }

        const char *kindLabel(StringView kind) {
                return kind == StringView("trusted")   ? "Built in, trusted"
                       : kind == StringView("package") ? "Built in, for packages"
                       : kind == StringView("dev")     ? "The dev key"
                       : kind == StringView("added")   ? "Added"
                                                       : "";
        }

} // namespace

// ---- AddSourceDialog

AddSourceDialog::AddSourceDialog(GuiApplication &app, SoftwarePanel &panel)
    : Dialog(app, "Add Source", 560, 290), _panel(panel) {}

Error AddSourceDialog::build() {
        auto *prompt = Widget::create<Label>(content(), "&Address of the source (its index.bmpi's folder):");
        auto *row = Widget::create<BoxLayout>(content(), BoxLayout::Direction::Horizontal);
        if (prompt == nullptr || row == nullptr) return Error::NoMem;
        row->setSpacing(6);
        _url = Widget::create<LineEdit>(row);
        _look = Widget::create<Button>(row, "&Look");
        _found = Widget::create<TextBlock>(content());
        if (_url == nullptr || _look == nullptr || _found == nullptr) return Error::NoMem;
        prompt->setBuddy(_url);
        _url->setStretch(1);
        _url->setAccepted(urlAccepted, this);
        _url->setEdited(urlEdited, this);
        _look->setClicked(lookClicked, this);
        _found->setStretch(1);
        _add = addButton("&Add", Accepted);
        (void)addButton("Cancel", Rejected);
        setCancelResult(Rejected);
        return _add == nullptr ? Error::NoMem : Error::Ok;
}

Error AddSourceDialog::openEmpty() {
        _url->setText(StringView());
        _found->setText("An http(s) address, or file:/FOLDER for a folder of packages on the card. "
                        "Look shows who signed it before anything is added.");
        // Its longest text when it opens: the buttons are sized then.
        _add->setText("&Trust and Add");
        _add->setEnabled(false);
        const Error err = open();
        if (err.isOk()) _url->setFocus();
        return err;
}

StringView AddSourceDialog::url() const {
        return _url->text().trimmed();
}

void AddSourceDialog::looked(StringView text, bool canAdd, bool newKey) {
        _found->setText(text);
        _add->setText(newKey ? StringView("&Trust and Add") : StringView("&Add"));
        _add->setEnabled(canAdd);
        _look->setEnabled(true);
        return;
}

void AddSourceDialog::look() {
        if (url().isEmpty()) return;
        _add->setEnabled(false);
        _look->setEnabled(false);
        _found->setText("Looking...");
        _panel.look(url());
        return;
}

void AddSourceDialog::lookClicked(void *arg, Button &) {
        static_cast<AddSourceDialog *>(arg)->look();
        return;
}

void AddSourceDialog::urlAccepted(void *arg, LineEdit &) {
        static_cast<AddSourceDialog *>(arg)->look();
        return;
}

void AddSourceDialog::urlEdited(void *arg, LineEdit &) {
        // What was looked at isn't what is typed any more.
        auto *self = static_cast<AddSourceDialog *>(arg);
        self->_add->setEnabled(false);
        return;
}

// ---- SoftwarePanel

SoftwarePanel::SoftwarePanel(Widget *parent, SettingsWindow &window)
    : BoxLayout(parent, BoxLayout::Direction::Vertical), _window(window), _job(window.application()),
      _dialog(window.application(), *this) {}

SoftwarePanel::~SoftwarePanel() {
        _window.forget(this);
}

Error SoftwarePanel::build() {
        setSpacing(6);
        auto *sources = Widget::create<Label>(this, "Sources: where packages come from");
        _sourceView = Widget::create<ShortList>(this, uint16_t(5));
        auto *sourceButtons = Widget::create<BoxLayout>(this, BoxLayout::Direction::Horizontal);
        auto *keys = Widget::create<Label>(this, "Keys: who packages may be signed by");
        _keyView = Widget::create<ShortList>(this, uint16_t(4));
        auto *keyButtons = Widget::create<BoxLayout>(this, BoxLayout::Direction::Horizontal);
        _state = Widget::create<Label>(this);
        if (sources == nullptr || _sourceView == nullptr || sourceButtons == nullptr || keys == nullptr ||
            _keyView == nullptr || keyButtons == nullptr || _state == nullptr)
                return Error::NoMem;
        sourceButtons->setSpacing(6);
        keyButtons->setSpacing(6);
        _addSource = Widget::create<Button>(sourceButtons, "A&dd Source...");
        _removeSource = Widget::create<Button>(sourceButtons, "Re&move Source");
        _update = Widget::create<Button>(sourceButtons, "&Update Lists");
        _addKey = Widget::create<Button>(keyButtons, "Add &Key from File...");
        _removeKey = Widget::create<Button>(keyButtons, "Remove K&ey");
        if (_addSource == nullptr || _removeSource == nullptr || _update == nullptr || _addKey == nullptr ||
            _removeKey == nullptr)
                return Error::NoMem;
        for (Button *b : {_addSource, _removeSource, _update, _addKey, _removeKey}) b->setClicked(clicked, this);

        const StringView       sourceTitles[3] = {"Source", "Signed by", "Packages"};
        const uint16_t         sourceWidths[3] = {250, 196, 70};
        const ItemModel::Align sourceAligns[3] = {ItemModel::Align::Left, ItemModel::Align::Left,
                                                  ItemModel::Align::Right};
        _sourceModel.setColumns(Span<const StringView>(sourceTitles), Span<const uint16_t>(sourceWidths),
                                Span<const ItemModel::Align>(sourceAligns));
        const StringView       keyTitles[3] = {"Key", "Id", "Kind"};
        const uint16_t         keyWidths[3] = {220, 136, 160};
        const ItemModel::Align keyAligns[3] = {ItemModel::Align::Left, ItemModel::Align::Left, ItemModel::Align::Left};
        _keyModel.setColumns(Span<const StringView>(keyTitles), Span<const uint16_t>(keyWidths),
                             Span<const ItemModel::Align>(keyAligns));
        for (DetailsView *v : {_sourceView, _keyView}) {
                v->setSingleSelection(true);
                v->setChanged(selectionChanged, this);
        }
        _sourceView->setModel(&_sourceModel);
        _keyView->setModel(&_keyModel);

        Error err = _dialog.build();
        if (err.isError()) return err;
        _dialog.setFinished(addFinished, this);
        _job.setHandlers(onRecord, onFinished, this);
        updateButtons();
        refresh();
        return Error::Ok;
}

void SoftwarePanel::say(StringView text) {
        _state->setText(text);
        return;
}

void SoftwarePanel::refresh() {
        if (_job.isRunning()) return; // read again when it ends
        _thenKeys = true;
        run(Stage::Sources, "source list");
        return;
}

void SoftwarePanel::run(Stage stage, StringView args) {
        _stage = stage;
        _failure.clear();
        if (stage == Stage::Sources) _sources.clear();
        if (stage == Stage::Keys) _keys.clear();
        const Error err = _job.start(_window.program(), args);
        if (err.isError()) {
                _stage = Stage::Idle;
                char text[96];
                std::snprintf(text, sizeof(text), "pkg couldn't be started: %s", err.desc());
                say(StringView(text));
                if (stage == Stage::Look) _dialog.looked(StringView(text), false, false);
        }
        updateButtons();
        return;
}

void SoftwarePanel::look(StringView url) {
        if (_job.isRunning()) {
                _dialog.looked("Busy with another change: look again in a moment.", false, false);
                return;
        }
        _lookLocation.clear();
        _lookId.clear();
        _lookText.clear();
        _lookPackages = 0;
        _lookSigned = false;
        _lookNew = false;
        _lookKey = false;
        String args(MemSpace::Psram);
        (void)args.append("source add --dry-run ");
        quoted(args, url);
        run(Stage::Look, args.view());
        return;
}

void SoftwarePanel::lookRecord(const PkgRecord &r) {
        if (r.type == PkgRecord::Type::Source) {
                (void)_lookLocation.assign(r.location.view());
                _lookPackages = r.packages;
                _lookSigned = r.isSigned;
        } else if (r.type == PkgRecord::Type::Key) {
                _lookKey = true;
                _lookNew = r.kind.view() == StringView("new");
                (void)_lookId.assign(r.id.view());
                if (_lookNew) {
                        (void)_lookText.appendf(
                                "%llu packages, signed by a key this system doesn't know:\n\n"
                                "%s, key %s:\n%s\nfrom %s\n\n"
                                "Anyone who controls that address can publish packages you would then install. "
                                "Add it only if you trust its publisher: compare the key with the one they "
                                "publish.",
                                static_cast<unsigned long long>(_lookPackages),
                                r.comment.isEmpty() ? "(no name)" : r.comment.cStr(), r.id.cStr(), r.key.cStr(),
                                r.from.cStr());
                } else {
                        (void)_lookText.appendf("%llu packages, signed by %s (%s, %s).",
                                                static_cast<unsigned long long>(_lookPackages), r.comment.cStr(),
                                                r.id.cStr(), kindLabel(r.kind.view()));
                }
        } else if (r.type == PkgRecord::Type::Error) {
                (void)_failure.assign(r.text.view());
        }
        return;
}

void SoftwarePanel::onRecord(void *arg, const PkgRecord &r) {
        auto *self = static_cast<SoftwarePanel *>(arg);
        switch (self->_stage) {
                case Stage::Sources:
                        if (r.type == PkgRecord::Type::Source) {
                                SourceRow row;
                                (void)row.location.assign(r.location.view());
                                (void)row.keyName.assign(r.keyName.view());
                                row.listed = r.listed;
                                row.packages = r.packages;
                                (void)self->_sources.append(std::move(row));
                        }
                        break;
                case Stage::Keys:
                        if (r.type == PkgRecord::Type::Key) {
                                KeyRow row;
                                (void)row.id.assign(r.id.view());
                                (void)row.kind.assign(r.kind.view());
                                (void)row.comment.assign(r.comment.view());
                                (void)self->_keys.append(std::move(row));
                        }
                        break;
                case Stage::Look: self->lookRecord(r); break;
                case Stage::Update:
                        if (r.type == PkgRecord::Type::Source) {
                                ++self->_sourcesRead;
                                if (!r.error.isEmpty()) {
                                        ++self->_sourcesFailed;
                                        if (self->_failure.isEmpty()) (void)self->_failure.assign(r.text.view());
                                }
                        }
                        break;
                default:
                        if (r.type == PkgRecord::Type::Error && self->_failure.isEmpty())
                                (void)self->_failure.assign(r.text.view());
                        break;
        }
        return;
}

void SoftwarePanel::onFinished(void *arg, int status) {
        auto       *self = static_cast<SoftwarePanel *>(arg);
        const Stage stage = self->_stage;
        self->_stage = Stage::Idle;
        const bool failed = status != 0;
        String     text(MemSpace::Psram);
        switch (stage) {
                case Stage::Sources:
                        self->fillSources();
                        if (self->_thenKeys) {
                                self->_thenKeys = false;
                                self->run(Stage::Keys, "key list");
                                return;
                        }
                        break;
                case Stage::Keys: self->fillKeys(); break;
                case Stage::Look:
                        if (failed || self->_lookLocation.isEmpty()) {
                                self->_dialog.looked(self->_failure.isEmpty() ? StringView("It couldn't be read.")
                                                                              : self->_failure.view(),
                                                     false, false);
                        } else if (!self->_lookSigned) {
                                (void)text.appendf("%llu packages in the folder, without a signed index: each "
                                                   "package's own signature is checked as it installs.",
                                                   static_cast<unsigned long long>(self->_lookPackages));
                                self->_dialog.looked(text.view(), true, false);
                        } else {
                                self->_dialog.looked(self->_lookText.view(), self->_lookKey, self->_lookNew);
                        }
                        break;
                case Stage::Add:
                case Stage::Remove:
                case Stage::AddKey:
                case Stage::RemoveKey:
                        if (failed) {
                                self->say(self->_failure.isEmpty() ? StringView("It didn't work.")
                                                                   : self->_failure.view());
                        } else {
                                self->say(stage == Stage::Add      ? StringView("Source added.")
                                          : stage == Stage::Remove ? StringView("Source removed.")
                                          : stage == Stage::AddKey ? StringView("Key added.")
                                                                   : StringView("Key removed."));
                        }
                        self->refresh();
                        return;
                case Stage::Update:
                        if (self->_sourcesFailed == 0) {
                                (void)text.appendf("Read %u sources.", unsigned(self->_sourcesRead));
                        } else {
                                (void)text.appendf("Read %u of %u sources: %s",
                                                   unsigned(self->_sourcesRead - self->_sourcesFailed),
                                                   unsigned(self->_sourcesRead), self->_failure.cStr());
                        }
                        self->say(text.view());
                        self->refresh();
                        return;
                case Stage::Idle: break;
        }
        self->updateButtons();
        return;
}

void SoftwarePanel::fillSources() {
        const int was = _sourceView->current();
        _sourceModel.clear();
        for (const SourceRow &s : _sources) {
                const StringView signer = s.keyName.view();
                char             n[24];
                std::snprintf(n, sizeof(n), "%llu", static_cast<unsigned long long>(s.packages));
                const StringView count = s.listed ? StringView(n) : StringView("unread");
                const bool       network = !s.location.view().startsWith("file:");
                const StringView by = !signer.isEmpty() ? signer
                                      : network         ? StringView("any key in the keyring")
                                                        : StringView("each package checked");
                const StringView cols[2] = {by, count}; // after the name, column 0
                (void)_sourceModel.add(s.location.view(), nullptr, nullptr, Span<const StringView>(cols));
        }
        const int    row = was >= 0 && size_t(was) < _sourceModel.count() ? was : -1;
        const size_t r = row >= 0 ? size_t(row) : 0;
        _sourceView->resetTo(Span<const size_t>(&r, row >= 0 ? 1 : 0), row);
        return;
}

void SoftwarePanel::fillKeys() {
        const int was = _keyView->current();
        _keyModel.clear();
        for (const KeyRow &k : _keys) {
                const StringView cols[2] = {k.id.view(), kindLabel(k.kind.view())}; // after the name
                (void)_keyModel.add(k.comment.isEmpty() ? StringView("(no name)") : k.comment.view(), nullptr, nullptr,
                                    Span<const StringView>(cols));
        }
        const int    row = was >= 0 && size_t(was) < _keyModel.count() ? was : -1;
        const size_t r = row >= 0 ? size_t(row) : 0;
        _keyView->resetTo(Span<const size_t>(&r, row >= 0 ? 1 : 0), row);
        return;
}

void SoftwarePanel::updateButtons() {
        const bool idle = !_job.isRunning();
        const int  source = _sourceView->current();
        const int  key = _keyView->current();
        _addSource->setEnabled(idle);
        _update->setEnabled(idle);
        _addKey->setEnabled(idle);
        _removeSource->setEnabled(idle && source >= 0 && size_t(source) < _sources.size());
        // Only an added key can be removed: the others are built into the firmware.
        _removeKey->setEnabled(idle && key >= 0 && size_t(key) < _keys.size() &&
                               _keys[size_t(key)].kind.view() == StringView("added"));
        return;
}

void SoftwarePanel::selectionChanged(void *arg, ItemView &) {
        static_cast<SoftwarePanel *>(arg)->updateButtons();
        return;
}

void SoftwarePanel::clicked(void *arg, Button &button) {
        auto *self = static_cast<SoftwarePanel *>(arg);
        if (self->_job.isRunning()) return;
        GuiApplication &app = self->_window.application();
        if (&button == self->_addSource) {
                if (self->_dialog.openEmpty().isError()) self->say("The dialog can't open");
        } else if (&button == self->_update) {
                self->_sourcesRead = 0;
                self->_sourcesFailed = 0;
                self->say("Reading the sources' lists...");
                self->run(Stage::Update, "update");
        } else if (&button == self->_addKey) {
                self->_window.chooseFileFor(keyFileChosen, self);
        } else if (&button == self->_removeSource) {
                const int at = self->_sourceView->current();
                if (at < 0 || size_t(at) >= self->_sources.size()) return;
                (void)self->_pending.assign(self->_sources[size_t(at)].location.view());
                String text(MemSpace::Psram);
                (void)text.appendf("Remove the source %s?\n\nPackages installed from it stay installed, but no "
                                   "longer upgrade. Its key is removed too if no other source uses it.",
                                   self->_pending.cStr());
                (void)MessageBox::show(app, MessageBox::Kind::Question, "Remove Source", text.view(),
                                       MessageBox::Buttons::YesNo, removeAnswered, self);
        } else if (&button == self->_removeKey) {
                const int at = self->_keyView->current();
                if (at < 0 || size_t(at) >= self->_keys.size()) return;
                const KeyRow &k = self->_keys[size_t(at)];
                (void)self->_pending.assign(k.id.view());
                String text(MemSpace::Psram);
                (void)text.appendf("Remove the key %s (%s)?\n\nWhat it signed can't be installed or upgraded "
                                   "until it is added again, and sources pinned to it stop updating.",
                                   k.comment.cStr(), k.id.cStr());
                (void)MessageBox::show(app, MessageBox::Kind::Question, "Remove Key", text.view(),
                                       MessageBox::Buttons::YesNo, removeKeyAnswered, self);
        }
        return;
}

void SoftwarePanel::addFinished(void *arg, Dialog &, int result) {
        auto *self = static_cast<SoftwarePanel *>(arg);
        if (result != AddSourceDialog::Accepted || self->_lookLocation.isEmpty() || self->_job.isRunning()) return;
        // What was shown is what is trusted: --trust names the key the look found.
        String args(MemSpace::Psram);
        (void)args.append("source add ");
        if (self->_lookSigned) {
                (void)args.append("--trust ");
                quoted(args, self->_lookId.view());
                (void)args.append(' ');
        } else {
                (void)args.append("-y ");
        }
        quoted(args, self->_lookLocation.view());
        self->say("Adding the source...");
        self->run(Stage::Add, args.view());
        return;
}

void SoftwarePanel::removeAnswered(void *arg, MessageBox::Answer answer) {
        auto *self = static_cast<SoftwarePanel *>(arg);
        if (answer != MessageBox::Answer::Yes || self->_job.isRunning()) return;
        String args(MemSpace::Psram);
        (void)args.append("source remove -y ");
        quoted(args, self->_pending.view());
        self->run(Stage::Remove, args.view());
        return;
}

void SoftwarePanel::removeKeyAnswered(void *arg, MessageBox::Answer answer) {
        auto *self = static_cast<SoftwarePanel *>(arg);
        if (answer != MessageBox::Answer::Yes || self->_job.isRunning()) return;
        String args(MemSpace::Psram);
        (void)args.append("key remove ");
        quoted(args, self->_pending.view());
        self->run(Stage::RemoveKey, args.view());
        return;
}

void SoftwarePanel::keyFileChosen(void *arg, StringView path) {
        auto *self = static_cast<SoftwarePanel *>(arg);
        if (self->_job.isRunning()) return;
        String args(MemSpace::Psram);
        (void)args.append("key add ");
        quoted(args, path);
        self->run(Stage::AddKey, args.view());
        return;
}

BMDS_NAMESPACE_END
