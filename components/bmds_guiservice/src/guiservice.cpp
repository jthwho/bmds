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
#include <bmds/streampair.h>
#include <bmds/windowmanager.h>
#include "guiatoms.h"
#include "guiconnection.h"
#include "guiservicepriv.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constinit std::atomic<bool> used[GuiService::MaxConnections] = {};

        Result<FileHandle::Ptr> open(uint32_t display, const GuiOpener::Client &client) {
                auto [wm, werr] = WindowManager::manager(display);
                if (werr.isError() || !wm->isRunning()) return makeError<FileHandle::Ptr>(Error::NotSupported);
                size_t slot = GuiService::MaxConnections;
                for (size_t i = 0; i < GuiService::MaxConnections && slot == GuiService::MaxConnections; ++i) {
                        bool expected = false;
                        if (used[i].compare_exchange_strong(expected, true)) slot = i;
                }
                if (slot == GuiService::MaxConnections) return makeError<FileHandle::Ptr>(Error::Full);
                auto [ends, err] = StreamPair::create();
                if (err.isOk())
                        err = GuiConnection::start(std::move(ends.second), &ends.first->stream(), uint8_t(slot),
                                                   display, client.program);
                if (err.isError()) {
                        used[slot].store(false);
                        return makeError<FileHandle::Ptr>(err);
                }
                return makeResult(std::move(ends.first));
        }

} // namespace

void GuiServicePriv::released(uint8_t slot) {
        if (slot < GuiService::MaxConnections) used[slot].store(false);
        return;
}

Error GuiService::init() {
        const Error err = GuiAtoms::init();
        if (err.isError()) return err;
        GuiConnection::initRegistry();
        GuiOpener::set(open, GuiConnection::canvasSurface);
        return Error::Ok;
}

BMDS_NAMESPACE_END
