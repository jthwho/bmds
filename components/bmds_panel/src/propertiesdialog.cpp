/**
 * @file      propertiesdialog.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Properties of files (devplan/filebrowser.md §4k, FB27).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/apps.h>
#include <bmds/clock.h>
#include <bmds/fileformat.h>
#include <bmds/fileops.h>
#include <bmds/foldermodel.h>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>
#include <bmds/trash.h>
#include <bmds/vfs.h>

#include "filejobs.h"
#include "propertiesdialog.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Width = 380;                         // the content's
        constexpr uint16_t LabelWidth = 160;                    // the label column ("Original location:")
        uint16_t           lineHeight() {                       // a label's
                return GuiStyle::buttonHeight(GuiStyle::font());
        }
        constexpr uint16_t IconSide = 32;
        constexpr uint16_t TopHeight = IconSide; // the icon and the name
        constexpr uint16_t RuleHeight = 9;
        constexpr size_t   MaxLines = 12;
        constexpr size_t   MaxValue = Label::MaxText;
        constexpr int      ResultOk = 1, ResultCancel = 2;
        const Vfs::Context Root{"/", nullptr};

        // The dialogs open (due() may come after one is gone).
        constinit PropertiesDialog *openDialogs[PropertiesDialog::MaxOpen] = {};

        // An etched line across the form, between its parts.
        class Rule final : public Widget {
                public:
                        explicit Rule(Widget *parent) : Widget(parent) {}

                        GuiSize sizeHint() const override { return GuiSize{0, RuleHeight}; }

                        void paint(Painter &painter) override {
                                const WindowPalette &p = painter.palette();
                                const Rect          &r = geometry();
                                (void)painter.fill(r, p.color(WindowPalette::Role::Face));
                                const uint16_t y = uint16_t(r.y + r.height / 2 - 1);
                                (void)painter.fill(Rect{r.x, y, r.width, 1}, p.color(WindowPalette::Role::FaceShadow));
                                (void)painter.fill(Rect{r.x, uint16_t(y + 1), r.width, 1},
                                                   p.color(WindowPalette::Role::FaceHighlight));
                                return;
                        }
        };

        // An icon (the dialog's copy of it).
        class Picture final : public Widget {
                public:
                        Picture(Widget *parent, const Icon &icon) : Widget(parent), _icon(icon) {}

                        GuiSize sizeHint() const override { return GuiSize{IconSide, IconSide}; }

                        void paint(Painter &painter) override {
                                const Rect    &r = geometry();
                                const uint32_t face = painter.palette().color(WindowPalette::Role::Face);
                                (void)painter.fill(r, face);
                                if (_icon.isValid()) (void)painter.image(r.x, r.y, _icon, face);
                                return;
                        }

                private:
                        Icon _icon;
        };

        // What the dialog says: its lines, worked out before it is made (its height follows them).
        struct Facts {
                        struct Pair {
                                        char label[24] = {};
                                        char value[MaxValue + 1] = {};
                        };
                        char     title[WindowManager::MaxTitle + 1] = {};
                        char     name[256] = {};
                        bool     renamable = false;
                        IconId   iconId = IconId::Unknown;
                        char     type[FileFormat::MaxType + 1] = {}; // a file's MIME type: its icon
                        Pair     lines[MaxLines];
                        size_t   count = 0;
                        int      sizeLine = -1;      // counted: the size,
                        int      containsLine = -1;  // and what folders hold
                        bool     counts = false;     // a folder among them: counted on the worker
                        bool     filesystem = false; // a filesystem's root: its space in a bar
                        uint64_t used = 0;
                        uint64_t total = 0;
                        uint64_t folders = 0; // the folders selected
                        uint64_t files = 0;   // and the rest
                        uint64_t bytes = 0;   // the files' bytes (when nothing is counted)

                        Pair *add(const char *label) {
                                if (count == MaxLines) return nullptr;
                                Pair &p = lines[count++];
                                std::snprintf(p.label, sizeof(p.label), "%s", label);
                                return &p;
                        }

                        void line(const char *label, StringView value) {
                                Pair *p = add(label);
                                if (p != nullptr)
                                        std::snprintf(p->value, sizeof(p->value), "%.*s", int(value.size()),
                                                      value.data());
                                return;
                        }
        };

        StringView folderOf(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == 0 || slash == StringView::NPos ? StringView("/") : path.left(slash);
        }

        // A size as "12 KB (12,345 bytes)".
        void sizeText(uint64_t bytes, char *out, size_t size) {
                char short_[24];
                formatBytes(bytes, short_, sizeof(short_));
                char      digits[32], grouped[40];
                const int n = std::snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(bytes));
                size_t    k = 0;
                for (int i = 0; i < n && k + 1 < sizeof(grouped); ++i) {
                        if (i > 0 && (n - i) % 3 == 0) grouped[k++] = ',';
                        grouped[k++] = digits[i];
                }
                grouped[k] = '\0';
                if (bytes < 1024) {
                        std::snprintf(out, size, "%s", short_);
                } else {
                        // Cut to the label's room if it must be (a label keeps what fits anyway).
                        if (std::snprintf(out, size, "%s (%s bytes)", short_, grouped) < 0) out[0] = '\0';
                }
                return;
        }

        void timeText(Timestamp t, char *out, size_t size) {
                const DateTime d = Clock::localTime(t).local;
                std::snprintf(out, size, "%04d-%02u-%02u %02u:%02u", int(d.year), unsigned(d.month), unsigned(d.day),
                              unsigned(d.hour), unsigned(d.minute));
                return;
        }

        // A file's type as the views name it ("TXT File"), with its MIME type.
        void typeText(StringView name, StringView mime, char *out, size_t size) {
                const StringView ext = FolderModel::extensionOf(name);
                char             upper[16] = {};
                for (size_t i = 0; i < ext.size() && i + 1 < sizeof(upper); ++i)
                        upper[i] = ext[i] >= 'a' && ext[i] <= 'z' ? char(ext[i] - 'a' + 'A') : ext[i];
                if (ext.isEmpty()) {
                        std::snprintf(out, size, "File (%.*s)", int(mime.size()), mime.data());
                } else {
                        std::snprintf(out, size, "%s File (%.*s)", upper, int(mime.size()), mime.data());
                }
                return;
        }

        // The mount @p path is the root of (its type into @p type), or false.
        bool mountRoot(StringView path, char (&type)[16]) {
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts, Vfs::MaxMounts));
                for (size_t i = 0; i < n && i < Vfs::MaxMounts; ++i) {
                        if (StringView(mounts[i].path) != path) continue;
                        std::snprintf(type, sizeof(type), "%s", mounts[i].type);
                        return true;
                }
                return false;
        }

        bool inTrash(StringView path) {
                const StringView t = Vfs::TrashPath;
                return path.size() > t.size() + 1 && path.startsWith(t) && path[t.size()] == '/' &&
                       path.indexOf('/', t.size() + 1) == StringView::NPos;
        }

        // One item's facts. @return False if it isn't there.
        bool describeOne(StringView path, Facts &f) {
                auto [own, lerr] = Vfs::lstat(path, Root);
                if (lerr.isError()) return false;
                // A broken link: itself.
                Result<FileInfo> followed = Vfs::stat(path, Root);
                const FileInfo  &info = followed.error().isOk() ? followed.value() : own;
                const StringView name = path == "/" ? StringView("/") : FileOps::baseName(path);
                std::snprintf(f.name, sizeof(f.name), "%.*s", int(name.size()), name.data());
                std::snprintf(f.title, sizeof(f.title), "%.*s Properties", int(name.size()), name.data());
                char       fsType[16] = {};
                const bool root = info.isDirectory() && mountRoot(path, fsType);
                const bool trash = info.isDirectory() && FolderActions::isTrash(path);
                const bool trashed = inTrash(path);
                const bool app = info.isDirectory() && Apps::isFolderName(name);
                // Renamed at OK: not a mount, nor the trash or what is in it, nor a virtual folder.
                f.renamable = !root && !trash && !trashed && info.device != 0 && path != "/";
                char text[MaxValue + 1];
                if (trash) {
                        f.iconId = IconId::UserTrash;
                        f.line("Type:", "Trash");
                } else if (root) {
                        f.iconId = IconId::DriveHarddisk;
                        f.line("Type:", "Filesystem");
                } else if (app) {
                        f.iconId = IconId::ApplicationXExecutable;
                        f.line("Type:", "Application");
                } else if (info.isDirectory()) {
                        f.iconId = IconId::Folder;
                        f.line("Type:", "Folder");
                } else if (Vfs::builtinProgram(path, Root) != nullptr) {
                        f.iconId = IconId::ApplicationXExecutable;
                        f.line("Type:", "Built-in program");
                } else {
                        auto [mime, ierr] = Vfs::identify(path, Vfs::IdentifyName | Vfs::IdentifyContent, Root);
                        const StringView m = ierr.isOk() && !mime.isEmpty() ? mime : FileFormat::Binary;
                        std::snprintf(f.type, sizeof(f.type), "%.*s", int(m.size()), m.data());
                        typeText(name, m, text, sizeof(text));
                        f.line("Type:", StringView(text));
                }
                if (own.type == FileType::Link) {
                        auto [target, rerr] = Vfs::readLink(path, Root);
                        if (rerr.isOk()) f.line("Target:", target.view());
                }
                if (trashed) {
                        f.line("Location:", "Trash");
                        Trash::Entry e;
                        if (Trash::find(path.mid(Vfs::TrashPath.size() + 1), e).isOk()) {
                                f.line("Original location:", folderOf(e.path.view()));
                                const StringView d = e.date.view();
                                if (d.size() >= 16) {
                                        std::snprintf(text, sizeof(text), "%.10s %.5s", d.data(), d.data() + 11);
                                        f.line("Deleted:", StringView(text));
                                }
                        }
                } else if (path != "/") {
                        f.line("Location:", folderOf(path));
                }
                if (info.isDirectory()) {
                        ++f.folders;
                        f.counts = true;
                        f.sizeLine = int(f.count);
                        f.line("Size:", "Counting...");
                        f.containsLine = int(f.count);
                        f.line("Contains:", "Counting...");
                } else {
                        ++f.files;
                        f.bytes = info.size;
                        sizeText(info.size, text, sizeof(text));
                        f.line("Size:", StringView(text));
                }
                if (info.modified.ns != 0) {
                        timeText(info.modified, text, sizeof(text));
                        f.line("Modified:", StringView(text));
                }
                if (info.device != 0 && !root) {
                        const uint8_t a = own.attributes;
                        std::snprintf(text, sizeof(text), "%s%s%s%s", (a & FileInfo::ReadOnly) ? "Read-only " : "",
                                      (a & FileInfo::Hidden) || name.startsWith('.') ? "Hidden " : "",
                                      (a & FileInfo::System) ? "System " : "",
                                      (a & (FileInfo::ReadOnly | FileInfo::Hidden | FileInfo::System)) == 0 &&
                                                      !name.startsWith('.')
                                              ? "None"
                                              : "");
                        f.line("Attributes:", StringView(text).trimmed());
                }
                if (root) {
                        f.line("File system:", StringView(fsType));
                        auto [fs, ferr] = Vfs::statfs(path, Root);
                        if (ferr.isOk()) {
                                f.filesystem = true;
                                f.total = fs.totalBytes;
                                f.used = fs.totalBytes > fs.freeBytes ? fs.totalBytes - fs.freeBytes : 0;
                                sizeText(fs.totalBytes, text, sizeof(text));
                                f.line("Capacity:", StringView(text));
                                sizeText(f.used, text, sizeof(text));
                                f.line("Used:", StringView(text));
                                sizeText(fs.freeBytes, text, sizeof(text));
                                f.line("Free:", StringView(text));
                        }
                }
                return true;
        }

        // Several items' facts: their number, types, place and size (counted if a folder is among them).
        bool describeSeveral(StringView paths, size_t n, Facts &f) {
                std::snprintf(f.name, sizeof(f.name), "%zu items", n);
                std::snprintf(f.title, sizeof(f.title), "%zu items Properties", n);
                f.iconId = IconId::Unknown;
                char       firstType[FileFormat::MaxType + 1] = {};
                bool       sameType = true, sameFolder = true, any = false;
                StringView folder;
                for (StringView rest = paths; !rest.isEmpty();) {
                        const size_t     end = rest.indexOf('\n');
                        const StringView p = end == StringView::NPos ? rest : rest.left(end);
                        rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                        if (p.isEmpty()) continue;
                        auto [info, serr] = Vfs::stat(p, Root);
                        if (serr.isError()) continue;
                        StringView type = FileFormat::Directory;
                        if (info.isDirectory()) {
                                ++f.folders;
                                f.counts = true;
                        } else {
                                ++f.files;
                                f.bytes += info.size;
                                auto [mime, ierr] = Vfs::identify(p, Vfs::IdentifyName, Root);
                                type = ierr.isOk() && !mime.isEmpty() ? mime : FileFormat::Binary;
                        }
                        if (!any) {
                                std::snprintf(firstType, sizeof(firstType), "%.*s", int(type.size()), type.data());
                                folder = folderOf(p);
                        } else {
                                sameType = sameType && type == StringView(firstType);
                                sameFolder = sameFolder && folderOf(p) == folder;
                        }
                        any = true;
                }
                if (!any) return false;
                char text[MaxValue + 1];
                if (sameType && StringView(firstType) == FileFormat::Directory) {
                        f.iconId = IconId::Folder;
                        f.line("Type:", "All folders");
                } else if (sameType) {
                        std::snprintf(f.type, sizeof(f.type), "%s", firstType);
                        std::snprintf(text, sizeof(text), "All of type %.48s", firstType);
                        f.line("Type:", StringView(text));
                } else {
                        f.line("Type:", "Various types");
                }
                f.line("Location:", sameFolder ? folder : StringView("Various folders"));
                f.sizeLine = int(f.count);
                if (f.counts) {
                        f.line("Size:", "Counting...");
                        f.containsLine = int(f.count);
                        f.line("Contains:", "Counting...");
                } else {
                        sizeText(f.bytes, text, sizeof(text));
                        f.line("Size:", StringView(text));
                }
                return true;
        }

} // namespace

PropertiesDialog::PropertiesDialog(GuiApplication &app, FolderActions &actions, StringView title, uint16_t height)
    : Dialog(app, title, Width, height), _actions(actions) {}

PropertiesDialog::~PropertiesDialog() {
        for (PropertiesDialog *&d : openDialogs)
                if (d == this) d = nullptr;
        Memory::free(_iconPixels);
}

bool PropertiesDialog::isOpen(const PropertiesDialog *dialog) {
        for (const PropertiesDialog *d : openDialogs)
                if (d == dialog) return true;
        return false;
}

Error PropertiesDialog::show(GuiApplication &app, FolderActions &actions, FileIcons &icons, StringView paths) {
        size_t n = 0;
        for (StringView rest = paths; !rest.isEmpty();) {
                const size_t end = rest.indexOf('\n');
                if (!(end == StringView::NPos ? rest : rest.left(end)).isEmpty()) ++n;
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
        }
        if (n == 0) return Error::Invalid;
        PropertiesDialog **slot = nullptr;
        for (PropertiesDialog *&d : openDialogs)
                if (d == nullptr && slot == nullptr) slot = &d;
        if (slot == nullptr) return Error::Full;
        auto *facts = static_cast<Facts *>(Memory::alloc(MemSpace::Psram, sizeof(Facts)));
        if (facts == nullptr) return Error::NoMem;
        ::new (facts) Facts();
        const StringView first =
                paths.left(paths.indexOf('\n') == StringView::NPos ? paths.size() : paths.indexOf('\n'));
        const bool ok = n == 1 ? describeOne(first.trimmed(), *facts) : describeSeveral(paths, n, *facts);
        if (!ok) {
                Memory::free(facts);
                actions.tell("Properties", "It isn't there any more.");
                return Error::NotExist;
        }
        const uint16_t height =
                uint16_t(TopHeight + RuleHeight + facts->count * lineHeight() + (facts->filesystem ? lineHeight() : 0));
        void          *mem = Memory::alloc(MemSpace::Psram, sizeof(PropertiesDialog));
        if (mem == nullptr) {
                Memory::free(facts);
                return Error::NoMem;
        }
        auto *d = ::new (mem) PropertiesDialog(app, actions, StringView(facts->title), height);
        *slot = d;
        Error err = d->_paths.assign(paths);
        if (err.isOk()) err = d->_name.assign(StringView(facts->name));
        if (err.isOk()) {
                // The widgets from the facts.
                d->_folders = facts->folders;
                d->_files = facts->files;
                BoxLayout *c = d->content();
                if (c != nullptr) c->setSpacing(0); // lines as a form's, close (the height counts them so)
                auto *top = c != nullptr ? Widget::create<BoxLayout>(c, BoxLayout::Direction::Horizontal) : nullptr;
                err = top != nullptr ? Error(Error::Ok) : Error(Error::NoMem);
                if (err.isOk()) {
                        top->setSpacing(12);
                        const Icon *icon = facts->type[0] != '\0' ? icons.forType(StringView(facts->type), IconSide)
                                                                  : icons.named(facts->iconId, IconSide);
                        if (facts->iconId == IconId::UserTrash && icons.isTrashFull())
                                icon = icons.named(IconId::UserTrashFull, IconSide);
                        if (icon == nullptr) icon = icons.named(IconId::Unknown, IconSide);
                        Icon copy;
                        if (icon != nullptr && icon->isValid() && icon->width <= IconSide && icon->height <= IconSide) {
                                const size_t pixels = size_t(icon->width) * icon->height;
                                d->_iconPixels = static_cast<uint32_t *>(
                                        Memory::alloc(MemSpace::Psram, pixels * sizeof(uint32_t)));
                                if (d->_iconPixels != nullptr) {
                                        std::memcpy(d->_iconPixels, icon->pixels, pixels * sizeof(uint32_t));
                                        copy = Icon{icon->width, icon->height, d->_iconPixels};
                                }
                        }
                        (void)Widget::create<Picture>(top, copy);
                        if (facts->renamable) {
                                d->_nameEdit = Widget::create<LineEdit>(top);
                                if (d->_nameEdit != nullptr) {
                                        d->_nameEdit->setStretch(1);
                                        d->_nameEdit->setMaxLength(255);
                                        d->_nameEdit->setText(StringView(facts->name));
                                }
                        } else {
                                auto *l = Widget::create<Label>(top, StringView(facts->name));
                                if (l != nullptr) l->setStretch(1);
                        }
                        (void)Widget::create<Rule>(c);
                        for (size_t i = 0; i < facts->count; ++i) {
                                const Line line = d->addLine(*c, StringView(facts->lines[i].label),
                                                             StringView(facts->lines[i].value));
                                if (int(i) == facts->sizeLine) d->_size = line.value;
                                if (int(i) == facts->containsLine) d->_contains = line.value;
                        }
                        if (facts->filesystem) {
                                auto *bar = Widget::create<ProgressBar>(c);
                                if (bar != nullptr) {
                                        // In KB: a large card's bytes fit the bar's arithmetic either way.
                                        bar->setMaximum(facts->total / 1024);
                                        bar->setValue(facts->used / 1024);
                                }
                        }
                        if (d->addButton("OK", ResultOk) == nullptr || d->addButton("Cancel", ResultCancel) == nullptr)
                                err = Error::NoMem;
                }
        }
        const bool counts = facts->counts;
        Memory::free(facts);
        if (err.isOk()) {
                d->setCancelResult(ResultCancel);
                d->setFinished(finished, d);
                if (d->content() != nullptr) d->content()->relayout();
                err = d->Dialog::open();
        }
        if (err.isError()) {
                d->~PropertiesDialog();
                Memory::free(d);
                return err;
        }
        if (counts) d->startCounting();
        return Error::Ok;
}

PropertiesDialog::Line PropertiesDialog::addLine(BoxLayout &column, StringView label, StringView value) {
        Line  line;
        auto *row = Widget::create<BoxLayout>(&column, BoxLayout::Direction::Horizontal);
        if (row == nullptr) return line;
        line.label = Widget::create<Label>(row, label);
        line.value = Widget::create<Label>(row, value);
        if (line.label != nullptr) line.label->setMinimumWidth(LabelWidth);
        if (line.value != nullptr) line.value->setStretch(1);
        return line;
}

// ---- Counting (FileWorker's Measure)

void PropertiesDialog::startCounting() {
        _counting = _worker.add(FileWorker::Kind::Measure, _paths.view(), StringView()).error().isOk();
        if (!_counting) showCounts(0, 0, true, nullptr);
        return;
}

void PropertiesDialog::notified(void *arg) {
        auto *self = static_cast<PropertiesDialog *>(arg);
        if (self->_posted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->application().post(due, self).isError()) self->_posted.store(false, std::memory_order_release);
        return;
}

void PropertiesDialog::due(void *arg) {
        auto *self = static_cast<PropertiesDialog *>(arg);
        if (!isOpen(self)) return; // closed since: gone
        self->_posted.store(false, std::memory_order_release);
        self->_worker.reap();
        FileWorker::Done done;
        if (self->_worker.takeDone(done)) {
                self->_counting = false;
                self->showCounts(done.totals.items, done.totals.bytes, true, &done.totals);
                return;
        }
        FileWorker::Status s;
        self->_worker.status(s);
        if (s.job != 0) self->showCounts(s.items, s.bytes, false, nullptr);
        return;
}

void PropertiesDialog::showCounts(uint64_t items, uint64_t bytes, bool done, const FileOps::Totals *totals) {
        char text[MaxValue + 1];
        if (_size != nullptr) {
                sizeText(bytes, text, sizeof(text));
                if (!done) std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text), "...");
                _size->setText(StringView(text));
        }
        if (_contains == nullptr) return;
        if (!done || totals == nullptr) {
                std::snprintf(text, sizeof(text), done ? "Couldn't count them" : "%llu items...",
                              static_cast<unsigned long long>(items));
        } else {
                // What the folders hold, the items selected not counted.
                const uint64_t folders = totals->folders > _folders ? totals->folders - _folders : 0;
                const uint64_t all = totals->items - totals->folders;
                const uint64_t files = all > _files ? all - _files : 0;
                std::snprintf(text, sizeof(text), "%llu file%s, %llu folder%s", static_cast<unsigned long long>(files),
                              files == 1 ? "" : "s", static_cast<unsigned long long>(folders), folders == 1 ? "" : "s");
        }
        _contains->setText(StringView(text));
        return;
}

// ---- Closing

void PropertiesDialog::rename() {
        if (_nameEdit == nullptr) return;
        String name(MemSpace::Psram);
        if (name.assign(_nameEdit->text()).isError() || name.view().trimmed() == _name.view()) return;
        String           to(MemSpace::Psram);
        char             problem[200];
        const StringView path = _paths.view().trimmed();
        if (_actions.rename(path, name.view(), StringView(), 255, to, Span<char>(problem, sizeof(problem)))) return;
        if (problem[0] != '\0') _actions.tell("Rename", StringView(problem));
        return;
}

void PropertiesDialog::finished(void *arg, Dialog &dialog, int result) {
        (void)dialog;
        auto *self = static_cast<PropertiesDialog *>(arg);
        if (result == ResultOk) self->rename();
        // Destroyed after this event (not within it): the counting stopped and waited for then.
        for (PropertiesDialog *&d : openDialogs)
                if (d == self) d = nullptr;
        if (self->application().post(destroy, self).isError()) return; // left: the panel's memory
        return;
}

void PropertiesDialog::destroy(void *arg) {
        auto *self = static_cast<PropertiesDialog *>(arg);
        self->~PropertiesDialog();
        Memory::free(self);
        return;
}

BMDS_NAMESPACE_END
