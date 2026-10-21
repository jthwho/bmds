/**
 * @file      findprogram.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program found as the shell finds it, for the services that start one (sshservice.cpp,
 * netmounts.cpp).
 */

#pragma once

#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace NetPrograms {

        /** @brief The shell's search path when `shell/path` is unset. */
        constexpr const char *DefaultPath = "/bin:/rom/bin:/apps:/sd0/p0/bin";

        /** @brief `shell/path`, or `DefaultPath`, into @p out. */
        inline void searchPath(String &out) {
                if (SysConf::get(StringView("shell/path"), out).isError() || out.isEmpty())
                        (void)out.assign(StringView(DefaultPath));
                return;
        }

        /** @brief @p name in the directories of the search path (the first that has it), into @p out. */
        inline bool find(StringView name, String &out) {
                String path(MemSpace::Psram);
                searchPath(path);
                StringView rest = path.view();
                while (!rest.isEmpty()) {
                        const size_t     colon = rest.indexOf(':');
                        const StringView dir = colon == StringView::NPos ? rest : rest.left(colon);
                        rest = colon == StringView::NPos ? StringView() : rest.mid(colon + 1);
                        if (dir.isEmpty()) continue;
                        out.clear();
                        if (out.appendf("%.*s/%.*s", int(dir.size()), dir.data(), int(name.size()), name.data())
                                    .isError())
                                return false;
                        const Result<FileInfo> info = Vfs::stat(out.view(), Vfs::Context{"/", nullptr});
                        if (info.isOk() && !info.value().isDirectory()) return true;
                }
                return false;
        }

} // namespace NetPrograms

BMDS_NAMESPACE_END
