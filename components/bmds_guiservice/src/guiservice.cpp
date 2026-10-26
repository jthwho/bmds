/**
 * @file      guiservice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The GUI service (devplan/guiprograms.md §8): registered with the loader at boot, it opens a
 * connection for each `gui_open` while the window system runs.
 */

#include <bmds/guiservice.h>

#include <atomic>
#include <bmds/guiopener.h>
#include <bmds/log.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/taskowner.h>
#include <bmds/streampair.h>
#include <bmds/windowmanager.h>
#include "guiatoms.h"
#include "guiconnection.h"
#include "guiservicepriv.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit std::atomic<bool> used[GuiService::MaxConnections] = {};
        constinit ProgramLauncher  *loader = nullptr;

        BMDS_SYSCONF(guiShellClients, "gui/shell-clients", Config::Type::Paths, "",
                     "The programs that may act as the shell through the GUI service (SHELL: panels, docks), "
                     "by their paths as run (; between)",
                     0, 1024, nullptr, nullptr)

        Result<FileHandle::Ptr> open(uint32_t display, const GuiOpener::Client &client) {
                auto [wm, werr] = WindowManager::manager(display);
                if (werr.isError() || !wm->isRunning()) return makeError<FileHandle::Ptr>(Error::NotSupported);
                size_t slot = GuiService::MaxConnections;
                for (size_t i = 0; i < GuiService::MaxConnections && slot == GuiService::MaxConnections; ++i) {
                        bool expected = false;
                        if (used[i].compare_exchange_strong(expected, true)) slot = i;
                }
                if (slot == GuiService::MaxConnections) return makeError<FileHandle::Ptr>(Error::Full);
                // The caller's session (this is its thread): the windows it opens end its launch feedback.
                TaskOwner    *owner = TaskOwner::current();
                const int32_t session = owner != nullptr ? owner->sessionId() : 0;
                auto [ends, err] = StreamPair::create();
                if (err.isOk())
                        err = GuiConnection::start(std::move(ends.second), &ends.first->stream(), uint8_t(slot),
                                                   display, client.program, session);
                if (err.isError()) {
                        used[slot].store(false);
                        return makeError<FileHandle::Ptr>(err);
                }
                return makeResult(std::move(ends.first));
        }

} // namespace

Error GuiServicePriv::openRemote(FileHandle::Ptr end) {
        auto [wm, werr] = WindowManager::manager(0);
        if (werr.isError() || !wm->isRunning()) return Error::NotSupported;
        size_t slot = GuiService::MaxConnections;
        for (size_t i = 0; i < GuiService::MaxConnections && slot == GuiService::MaxConnections; ++i) {
                bool expected = false;
                if (used[i].compare_exchange_strong(expected, true)) slot = i;
        }
        if (slot == GuiService::MaxConnections) return Error::Full;
        // No client end (no CANVAS-SURFACE), no program (no SHELL), no session: a remote program's.
        const Error err = GuiConnection::start(std::move(end), nullptr, uint8_t(slot), 0, StringView(), 0);
        if (err.isError()) used[slot].store(false);
        return err;
}

void GuiServicePriv::released(uint8_t slot) {
        if (slot < GuiService::MaxConnections) used[slot].store(false);
        return;
}

ProgramLauncher &GuiServicePriv::launcher() {
        return *loader;
}

bool GuiServicePriv::shellAllowed(StringView path) {
        String list(MemSpace::Psram);
        if (path.isEmpty() || SysConf::get(StringView(bmdsConf_guiShellClients.key), list).isError()) return false;
        StringView rest = list.view();
        while (!rest.isEmpty()) {
                const size_t     semi = rest.indexOf(';');
                const StringView one = (semi == StringView::NPos ? rest : rest.left(semi)).trimmed();
                if (one == path) return true;
                if (semi == StringView::NPos) break;
                rest = rest.mid(semi + 1);
        }
        return false;
}

Error GuiService::init(ProgramLauncher &launcher) {
        loader = &launcher;
        const Error err = GuiAtoms::init();
        if (err.isError()) return err;
        GuiConnection::initRegistry();
        GuiOpener::set(open, GuiConnection::canvasSurface);
        return Error::Ok;
}

BMDS_NAMESPACE_END
