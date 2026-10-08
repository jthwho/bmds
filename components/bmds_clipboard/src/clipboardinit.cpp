/**
 * @file      clipboardinit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The seat's clipboard at boot, and `clip` in the shell and /bin (target only).
 * Its limit is the system configuration's `clipboard/limit`; clipboard.conf
 * holds the conversions (devplan/sysconf.md §8).
 */

#include <bmds/clipboard.h>
#include <bmds/clipcommand.h>
#include <bmds/log.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The configuration's size at most.
        constexpr size_t MaxConfigBytes = 8192;

        void reportLine(void *arg, uint32_t line, StringView text) {
                (void)arg;
                bmdsWarn("clipboard.conf:%u: not taken: %.*s", unsigned(line), int(text.size()), text.data());
                return;
        }

        BMDS_SYSCONF(clipboardLimit, "clipboard/limit", Config::Type::Int, "1048576",
                     "The bytes the clipboard keeps at most (copies over it are refused)", 1024, 64 * 1024 * 1024,
                     nullptr, nullptr)

        constinit StaticObject<SysConf::Watcher> limitWatcher;

        void applyLimit(void *) {
                const Result<int32_t> limit = SysConf::getInt("clipboard/limit");
                auto [clipboard, err] = Clipboard::clipboard(0);
                if (limit.isOk() && err.isOk()) clipboard->setLimit(size_t(limit.value()));
                return;
        }

        // Reads the configuration into @p out (nothing if there is none).
        Error readConfig(String &out) {
                Vfs::Context context;
                auto [handle, err] = Vfs::open(Clipboard::ConfigPath, Vfs::Read, context);
                if (err == Error::NotExist) return Error::Ok;
                if (err.isError()) return err;
                char buf[256];
                for (;;) {
                        auto [n, rerr] = handle->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                      WaitForever);
                        if (rerr == Error::EndOfFile) return Error::Ok;
                        if (rerr.isError()) return rerr;
                        if (out.size() + n > MaxConfigBytes) return Error::NoSpace;
                        const Error aerr = out.append(StringView(buf, n));
                        if (aerr.isError()) return aerr;
                }
        }

} // namespace

Error Clipboard::init() {
        auto [clipboard, err] = create(0);
        if (err.isError()) return err;
        String text(MemSpace::Psram);
        err = readConfig(text);
        if (err.isError()) bmdsWarn("Clipboard: %.*s: %s", int(ConfigPath.size()), ConfigPath.data(), err.desc());
        if (!text.isEmpty()) (void)clipboard->configure(text.view(), reportLine, nullptr);
        applyLimit(nullptr);
        limitWatcher.create();
        (void)limitWatcher->start("clipboard/limit", applyLimit, nullptr);
        return addFiles(0);
}

BMDS_SHELL_COMMAND(clip, ClipCommand::run, ClipCommand::Usage, ClipCommand::Help)

BMDS_NAMESPACE_END
