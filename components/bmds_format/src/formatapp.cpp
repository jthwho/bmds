/**
 * @file      formatapp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /bin/format [DEVICE] (devplan/format.md §4): a card, what is on it, the filesystem to make, and
 * Format..., which asks once more and does what `mkfs /dev/sd0` does: a new partition table with
 * one partition, formatted and mounted. Without DEVICE, the first card nothing mounted from, else
 * the first card. Opened by the blank card's question and by Files. An ordinary GUI program on the
 * toolkit; the formatting runs on a thread of its own, told to the window through the loop.
 */

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/blockdevices.h>
#include <bmds/builtinprograms.h>
#include <bmds/cfg.h>
#include <bmds/combobox.h>
#include <bmds/dialog.h>
#include <bmds/directorywatch.h>
#include <bmds/fdtable.h>
#include <bmds/formlayout.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/memory.h>
#include <bmds/storage.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   MaxCards = 4;
        constexpr size_t   WorkerStackBytes = 8 * 1024;
        constexpr uint64_t ExFatFromBytes = 0x4000000ull * 512; // FatFs's Auto: exFAT from 32 GiB
        constexpr uint32_t RestartDelayMs = 1000;               // "Restarting..." seen first
        const Vfs::Context Root{"/", nullptr};

        struct Card {
                        char         name[BlockDevices::MaxName + 1] = {};
                        BlockDevice *device = nullptr;
        };

        // The cards: whole block devices that aren't the flash, the ROM or memory.
        size_t listCards(Card (&out)[MaxCards]) {
                BlockDevices::Entry entries[BlockDevices::MaxDevices];
                const size_t        n = BlockDevices::list(Span<BlockDevices::Entry>(entries));
                size_t              count = 0;
                for (size_t i = 0; i < n && count < MaxCards; ++i) {
                        BlockDevice *d = entries[i].device;
                        if (entries[i].parent != nullptr || d->flash() != nullptr || d->isMemory() ||
                            !d->mapped().isEmpty() || !d->isPresent())
                                continue;
                        std::memcpy(out[count].name, entries[i].name, sizeof(out[count].name));
                        out[count].device = d;
                        ++count;
                }
                return count;
        }

        // How many partitions of @p device are registered.
        size_t partitionsOf(const BlockDevice &device) {
                BlockDevices::Entry entries[BlockDevices::MaxDevices];
                const size_t        n = BlockDevices::list(Span<BlockDevices::Entry>(entries));
                size_t              count = 0;
                for (size_t i = 0; i < n; ++i) count += entries[i].parent == &device ? 1 : 0;
                return count;
        }

        // True if @p path is /NAME or below it (a card's mounts are /sd0/p0...).
        bool isOnCard(StringView path, StringView name) {
                if (path.size() < name.size() + 1 || path[0] != '/' || path.mid(1, name.size()) != name) return false;
                return path.size() == name.size() + 1 || path[name.size() + 1] == '/';
        }

        // @p bytes as "31.9 GB" (or MB under one).
        void sizeText(uint64_t bytes, char (&out)[16]) {
                if (bytes >= 1000000000ull) {
                        const uint64_t tenths = bytes / 100000000ull;
                        std::snprintf(out, sizeof(out), "%u.%u GB", unsigned(tenths / 10), unsigned(tenths % 10));
                } else {
                        std::snprintf(out, sizeof(out), "%u MB", unsigned(bytes / 1000000ull));
                }
                return;
        }

        const char *formatName(FsFormat format, uint64_t bytes) {
                switch (format) {
                        case FsFormat::Fat32: return "FAT32";
                        case FsFormat::ExFat: return "exFAT";
                        case FsFormat::Auto: break;
                }
                return bytes >= ExFatFromBytes ? "exFAT" : "FAT32";
        }

        // A paragraph in the column: as many lines as its text takes at the width it was given (the
        // first layout guesses the width; the second knows it), as the Welcome window's.
        class WrapText final : public TextBlock {
                public:
                        explicit WrapText(Widget *parent) : TextBlock(parent) {}

                        GuiSize sizeHint() const override {
                                const uint16_t width = geometry().width > 0 ? geometry().width : GuessedWidth;
                                return GuiSize{16, text().isEmpty() ? uint16_t(0) : heightForWidth(width)};
                        }

                private:
                        static constexpr uint16_t GuessedWidth = 520;
        };

        class FormatWindow final : public GuiWindow {
                public:
                        FormatWindow(GuiApplication &app, const Config &config)
                            : GuiWindow(app, config), _confirm(app, "Format", 460, 56) {}

                        ~FormatWindow() override {
                                _watch.stop();
                                _worker.join();
                                application().cancelPosts(this);
                        }

                        Error build(StringView device);

                protected:
                        void closedByManager() override {
                                if (!_busy) application().quit(0);
                                return;
                        }

                        // Back to it: a card may have come or gone (a blank one mounts nothing to be told of).
                        void activeChanged() override {
                                if (isActive() && !_busy) refresh();
                                return;
                        }

                private:
                        static void mountsChanged(void *arg);
                        static void refreshPosted(void *arg);
                        static void cardChosen(void *arg, ComboBox &box, int index);
                        static void clicked(void *arg, Button &button);
                        static void confirmed(void *arg, Dialog &dialog, int result);
                        static void workerMain(void *arg);
                        static void formatted(void *arg);
                        static void restart(void *arg);
                        void        refresh();
                        void        describe();
                        void        say(const char *fmt, ...) BMDS_PRINTF_FORMAT(2, 3);
                        Card       *current();

                        Dialog            _confirm;
                        Label            *_question = nullptr; // the confirmation's
                        Label            *_named = nullptr;
                        ComboBox         *_cards = nullptr;
                        ComboBox         *_formats = nullptr;
                        FormRow          *_cardRow = nullptr;
                        FormLayout       *_form = nullptr;
                        TextBlock        *_warning = nullptr;
                        TextBlock        *_status = nullptr;
                        Button           *_format = nullptr;
                        Button           *_restart = nullptr;
                        Button           *_close = nullptr;
                        DirectoryWatch    _watch;
                        Card              _list[MaxCards];
                        size_t            _count = 0;
                        char              _wanted[BlockDevices::MaxName + 1] = {}; // the card asked for, or chosen
                        std::atomic<bool> _refreshPosted{false};
                        // The worker's, read on the loop once it posted.
                        Thread   _worker;
                        Card     _target;
                        FsFormat _fs = FsFormat::Auto;
                        Error    _result;
                        size_t   _mounted = 0;
                        bool     _busy = false; // formatting: not closed
        };

        Error FormatWindow::build(StringView device) {
                if (device.startsWith("/dev/")) device = device.mid(5);
                // A partition's card (sd0p0: sd0).
                if (BlockDevice *d = BlockDevices::find(device); d != nullptr) {
                        BlockDevices::Entry entries[BlockDevices::MaxDevices];
                        const size_t        n = BlockDevices::list(Span<BlockDevices::Entry>(entries));
                        for (size_t i = 0; i < n; ++i)
                                if (entries[i].device == d && entries[i].parent != nullptr)
                                        (void)BlockDevices::nameOf(*entries[i].parent, _wanted);
                }
                if (_wanted[0] == '\0' && device.size() < sizeof(_wanted))
                        std::memcpy(_wanted, device.data(), device.size());

                auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
                if (column == nullptr) return Error::NoMem;
                setCentral(column);
                column->setMargins(12, 12, 12, 12);
                column->setSpacing(8);
                _form = Widget::create<FormLayout>(column);
                if (_form == nullptr) return Error::NoMem;
                _form->setSpacing(6);
                _cardRow = _form->addRow("&Card", " ");
                FormRow *fsRow = _form->addRow("Format &as", "FAT32 and exFAT: read by every computer and camera");
                if (_cardRow == nullptr || fsRow == nullptr) return Error::NoMem;
                _cards = Widget::create<ComboBox>(_cardRow->field());
                _formats = Widget::create<ComboBox>(fsRow->field());
                _warning = Widget::create<WrapText>(column);
                _status = Widget::create<WrapText>(column);
                auto *room = Widget::create<Label>(column); // what the column has to spare, above the buttons
                auto *buttons = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
                if (_cards == nullptr || _formats == nullptr || _warning == nullptr || _status == nullptr ||
                    room == nullptr || buttons == nullptr)
                        return Error::NoMem;
                room->setStretch(1);
                _cardRow->label()->setBuddy(_cards);
                fsRow->label()->setBuddy(_formats);
                _cards->setStretch(1);
                _cards->setChanged(cardChosen, this);
                _formats->setStretch(1);
                buttons->setSpacing(8);
                auto *gap = Widget::create<Label>(buttons);
                _restart = Widget::create<Button>(buttons, "&Restart Now");
                _format = Widget::create<Button>(buttons, "&Format...");
                _close = Widget::create<Button>(buttons, "&Close");
                if (gap == nullptr || _restart == nullptr || _format == nullptr || _close == nullptr)
                        return Error::NoMem;
                gap->setStretch(1);
                _restart->setVisible(false);
                _restart->setClicked(clicked, this);
                _format->setClicked(clicked, this);
                _close->setClicked(clicked, this);

                _question = Widget::create<Label>(_confirm.content());
                _named = Widget::create<Label>(_confirm.content());
                if (_question == nullptr || _named == nullptr) return Error::NoMem;
                _confirm.addButton("&Format", 1);
                Button *cancel = _confirm.addButton("&Cancel", 0);
                if (cancel == nullptr) return Error::NoMem;
                _confirm.setDefaultButton(cancel); // Enter doesn't erase a card
                _confirm.setFocusWidget(cancel);
                _confirm.setFinished(confirmed, this);

                _watch.setNotify(mountsChanged, this);
                (void)_watch.watch("/", Root);
                refresh();
                if (_count > 0 && _wanted[0] == '\0') {
                        // Not asked for one: the first that nothing mounted from (the question's), else the first.
                        int            blank = -1;
                        Vfs::MountInfo mounts[Vfs::MaxMounts];
                        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                        for (size_t i = 0; i < _count && blank < 0; ++i) {
                                bool any = false;
                                for (size_t m = 0; m < n; ++m)
                                        any = any || StringView(mounts[m].device).startsWith(StringView(_list[i].name));
                                if (!any) blank = int(i);
                        }
                        _cards->setCurrent(blank >= 0 ? blank : 0);
                        describe();
                }
                column->relayout();
                return Error::Ok;
        }

        // From the watch (any thread): told to the loop once until it ran.
        void FormatWindow::mountsChanged(void *arg) {
                auto *self = static_cast<FormatWindow *>(arg);
                if (!self->_refreshPosted.exchange(true, std::memory_order_acq_rel)) {
                        if (self->application().post(refreshPosted, self).isError())
                                self->_refreshPosted.store(false, std::memory_order_release);
                }
                return;
        }

        void FormatWindow::refreshPosted(void *arg) {
                auto *self = static_cast<FormatWindow *>(arg);
                self->_refreshPosted.store(false, std::memory_order_release);
                (void)self->_watch.takeChanges();
                (void)self->_watch.watch("/", Root);
                if (!self->_busy) self->refresh();
                return;
        }

        // The cards again, the one chosen kept (by name) while it is there.
        void FormatWindow::refresh() {
                _count = listCards(_list);
                _cards->clear();
                int chosen = -1;
                for (size_t i = 0; i < _count; ++i) {
                        char size[16], text[ComboBox::MaxText + 1];
                        sizeText(_list[i].device->sectorCount() * _list[i].device->sectorSize(), size);
                        std::snprintf(text, sizeof(text), "%s (%s)", _list[i].name, size);
                        (void)_cards->addItem(StringView(text), i);
                        if (StringView(_list[i].name) == StringView(_wanted)) chosen = int(i);
                }
                if (chosen < 0 && _count > 0 && _wanted[0] == '\0') chosen = 0;
                _cards->setCurrent(chosen);
                describe();
                return;
        }

        Card *FormatWindow::current() {
                const int i = _cards->current();
                return i >= 0 && size_t(i) < _count ? &_list[size_t(i)] : nullptr;
        }

        void FormatWindow::cardChosen(void *arg, ComboBox &, int index) {
                auto *self = static_cast<FormatWindow *>(arg);
                if (index >= 0 && size_t(index) < self->_count)
                        std::memcpy(self->_wanted, self->_list[index].name, sizeof(self->_wanted));
                self->describe();
                return;
        }

        // What is on the chosen card, the formats it can have, and what formatting it would take with it.
        void FormatWindow::describe() {
                Card *card = current();
                _format->setEnabled(card != nullptr && !_busy);
                const int fs = _formats->current();
                _formats->clear();
                if (card == nullptr) {
                        (void)_cardRow->setHelp(_wanted[0] != '\0' ? "That card isn't in its slot"
                                                                   : "No card is in a slot: put one in");
                        _warning->setText(StringView());
                        _formats->setEnabled(false);
                        _form->relayout();
                        static_cast<BoxLayout *>(_warning->parent())->relayout();
                        return;
                }
                _formats->setEnabled(!_busy);
                const StringView name(card->name);
                const uint64_t   bytes = card->device->sectorCount() * card->device->sectorSize();
                char             autoText[ComboBox::MaxText + 1];
                std::snprintf(autoText, sizeof(autoText), "Automatic (%s)", formatName(FsFormat::Auto, bytes));
                (void)_formats->addItem(StringView(autoText), uintptr_t(FsFormat::Auto));
                (void)_formats->addItem("FAT32", uintptr_t(FsFormat::Fat32));
                (void)_formats->addItem("exFAT", uintptr_t(FsFormat::ExFat));
                _formats->setCurrent(fs >= 0 ? fs : 0);

                char           help[TextBlock::MaxText + 1];
                size_t         at = 0;
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                for (size_t i = 0; i < n; ++i) {
                        if (!StringView(mounts[i].device).startsWith(name) || at >= sizeof(help) - 1) continue;
                        const Result<FsInfo> info = Vfs::statfs(StringView(mounts[i].path), Root);
                        if (info.error().isError()) continue;
                        char used[16], total[16];
                        sizeText(info.value().totalBytes - info.value().freeBytes, used);
                        sizeText(info.value().totalBytes, total);
                        at += size_t(std::snprintf(help + at, sizeof(help) - at, "%s%s at %s: %s used of %s",
                                                   at > 0 ? "; " : "", info.value().type, mounts[i].path, used, total));
                }
                if (at == 0) {
                        const size_t parts = partitionsOf(*card->device);
                        if (parts == 0)
                                std::snprintf(help, sizeof(help), "Blank: no partitions and no filesystem");
                        else
                                std::snprintf(help, sizeof(help), "%u partition%s BMDS can't read (not FAT or exFAT)",
                                              unsigned(parts), parts == 1 ? "" : "s");
                }
                (void)_cardRow->setHelp(StringView(help));

                // BMDS's own on it: /cfg (chosen at boot) and the home folder.
                String home(MemSpace::Psram);
                (void)SysConf::get(StringView("home"), home);
                const bool cfg = isOnCard(Cfg::leadsTo(), name);
                const bool homeOn = isOnCard(home.view(), name);
                _warning->setText(cfg && homeOn ? "BMDS's settings and your home folder are on this card: formatting "
                                                  "erases them too."
                                  : cfg         ? "BMDS's settings are on this card: formatting erases them too."
                                  : homeOn      ? "Your home folder is on this card: formatting erases it too."
                                                : "");
                _form->relayout();
                static_cast<BoxLayout *>(_warning->parent())->relayout();
                return;
        }

        void FormatWindow::say(const char *fmt, ...) {
                char    text[TextBlock::MaxText + 1];
                va_list args;
                va_start(args, fmt);
                std::vsnprintf(text, sizeof(text), fmt, args);
                va_end(args);
                _status->setText(StringView(text));
                static_cast<BoxLayout *>(_status->parent())->relayout();
                return;
        }

        void FormatWindow::clicked(void *arg, Button &button) {
                auto *self = static_cast<FormatWindow *>(arg);
                if (&button == self->_close) {
                        if (!self->_busy) self->application().quit(0);
                        return;
                }
                if (&button == self->_restart) {
                        self->say("Restarting...");
                        (void)self->application().startTimer(RestartDelayMs, false, restart, self);
                        return;
                }
                Card *card = self->current();
                if (card == nullptr) return;
                char size[16], text[Label::MaxText + 1];
                sizeText(card->device->sectorCount() * card->device->sectorSize(), size);
                std::snprintf(text, sizeof(text), "Erase everything on the %s card in %s?", size, card->name);
                self->_question->setText(StringView(text));
                const auto fs = static_cast<FsFormat>(self->_formats->itemData(size_t(self->_formats->current())));
                std::snprintf(text, sizeof(text), "It is formatted as %s, with one partition.",
                              formatName(fs, card->device->sectorCount() * card->device->sectorSize()));
                self->_named->setText(StringView(text));
                (void)self->_confirm.open();
                return;
        }

        void FormatWindow::confirmed(void *arg, Dialog &, int result) {
                auto *self = static_cast<FormatWindow *>(arg);
                Card *card = self->current();
                if (result != 1 || card == nullptr || self->_busy) return;
                self->_target = *card;
                self->_fs = static_cast<FsFormat>(self->_formats->itemData(size_t(self->_formats->current())));
                self->_busy = true;
                self->_format->setEnabled(false);
                self->_close->setEnabled(false);
                self->_cards->setEnabled(false);
                self->_formats->setEnabled(false);
                self->say("Formatting %s... (a few seconds; don't take the card out)", self->_target.name);
                self->_worker.join(); // the last one's, ended
                Thread::Config config;
                config.name = "format";
                config.stackBytes = WorkerStackBytes;
                config.stackSpace = MemSpace::Psram; // as the storage service's, which mounts cards
                if (self->_worker.start(config, workerMain, self).isError()) {
                        self->_result = Error::NoMem;
                        formatted(self);
                }
                return;
        }

        // The worker: what mkfs does to a whole card (devplan/format.md §4), then told to the loop.
        void FormatWindow::workerMain(void *arg) {
                auto        *self = static_cast<FormatWindow *>(arg);
                BlockDevice &device = *self->_target.device;
                self->_mounted = 0;
                Vfs::eject(device);
                self->_result = Vfs::format(device, self->_fs);
                if (self->_result.isOk()) {
                        const Result<size_t> mounted = Vfs::automount(device);
                        self->_result = mounted.error();
                        if (mounted.error().isOk()) self->_mounted = mounted.value();
                        Storage::cardFormatted(device);
                }
                (void)self->application().post(formatted, self);
                return;
        }

        void FormatWindow::formatted(void *arg) {
                auto *self = static_cast<FormatWindow *>(arg);
                self->_busy = false;
                self->_close->setEnabled(true);
                self->_cards->setEnabled(true);
                self->refresh();
                if (self->_result.isError()) {
                        self->say("Not formatted: %s", self->_result.desc());
                        return;
                }
                // Where it went, and whether /cfg's card folder is on it now (BMDS sets it up at boot).
                const StringView cardRoot = Cfg::CardPath.left(Cfg::CardPath.lastIndexOf('/'));
                const StringView name(self->_target.name);
                char             where[Vfs::MaxMountPath + 1] = {};
                bool             system = false;
                Vfs::MountInfo   mounts[Vfs::MaxMounts];
                const size_t     n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                for (size_t i = 0; i < n; ++i) {
                        if (!StringView(mounts[i].device).startsWith(name)) continue;
                        if (where[0] == '\0') std::memcpy(where, mounts[i].path, sizeof(where));
                        system = system || StringView(mounts[i].path) == cardRoot;
                }
                const Result<FsInfo> info = Vfs::statfs(StringView(where), Root);
                char                 free[16] = "?";
                if (info.error().isOk()) sizeText(info.value().freeBytes, free);
                if (where[0] == '\0') {
                        self->say("Formatted, but nothing mounted from it");
                } else if (system) {
                        self->say("Formatted: %s at %s, %s free. Restart BMDS to start using the card: its settings "
                                  "and your home folder go onto it, then it offers software to install.",
                                  info.error().isOk() ? info.value().type : "?", where, free);
                        self->_restart->setVisible(true);
                        static_cast<BoxLayout *>(self->_restart->parent())->relayout();
                } else {
                        self->say("Formatted: %s at %s, %s free.", info.error().isOk() ? info.value().type : "?", where,
                                  free);
                }
                return;
        }

        void FormatWindow::restart(void *) {
                (void)Vfs::sync();
                System::restart();
        }

        int formatMain(const BuiltinProgram::Context &context) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                const auto  complain = [err](const char *text) {
                        if (err != nullptr)
                                (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                                  WaitForever);
                };
                if (context.args.size() > 2) {
                        complain("usage: format [DEVICE]\n");
                        return 2;
                }
                const StringView device = context.args.size() == 2 ? StringView(context.args[1]) : StringView();
                void            *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
                if (memory == nullptr) return 1;
                auto *app = ::new (memory) GuiApplication();
                int   status = 1;
                if (app->connect(0).isError()) {
                        complain("format: the window system isn't running ('win start'); mkfs formats from the "
                                 "shell\n");
                } else {
                        GuiWindow::Config config;
                        config.rect = Rect{240, 160, 560, 330};
                        config.title = "Format";
                        void *wmem = Memory::alloc(MemSpace::Psram, sizeof(FormatWindow));
                        if (wmem != nullptr) {
                                auto *window = ::new (wmem) FormatWindow(*app, config);
                                Error e;
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        e = window->build(device);
                                        if (e.isOk()) e = window->show();
                                }
                                if (e.isOk()) {
                                        status = app->exec();
                                } else {
                                        complain("format: can't open its window\n");
                                }
                                {
                                        Mutex::Locker lock(app->lock(), WaitForever);
                                        window->~FormatWindow();
                                }
                                Memory::free(wmem);
                        }
                }
                app->~GuiApplication();
                Memory::free(memory);
                return status;
        }

} // namespace

BMDS_PROGRAM(format, formatMain, 12 * 1024)

BMDS_NAMESPACE_END
