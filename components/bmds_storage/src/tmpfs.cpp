/**
 * @file      tmpfs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/log.h>
#include <bmds/staticobject.h>
#include <bmds/sysconf.h>
#include <bmds/tmpfs.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(tmpSize, "tmp/size", Config::Type::Int, "16777216",
                     "The bytes /tmp may take (PSRAM, as used); 0 at boot for no /tmp", 0, 1024 * 1024 * 1024, nullptr,
                     nullptr)

        constinit StaticObject<SysConf::Watcher> sizeWatcher;

        uint64_t configuredSize() {
                const Result<int32_t> size = SysConf::getInt("tmp/size");
                return size.isOk() && size.value() > 0 ? uint64_t(size.value()) : 0;
        }

        void applySize(void *) {
                (void)Vfs::setTmpfsLimit(Vfs::TmpPath, configuredSize());
                return;
        }

} // namespace

Error TmpFs::init() {
        const uint64_t size = configuredSize();
        if (size == 0) return Error::Ok;
        Error err = Vfs::init();
        if (err.isOk()) err = Vfs::mountTmpfs(Vfs::TmpPath, size);
        if (err.isError()) {
                bmdsErr("TmpFs: %.*s: %s", int(Vfs::TmpPath.size()), Vfs::TmpPath.data(), err.desc());
                return err;
        }
        sizeWatcher.create();
        (void)sizeWatcher->start("tmp/size", applySize, nullptr);
        bmdsInfo("TmpFs: %.*s, %u KB", int(Vfs::TmpPath.size()), Vfs::TmpPath.data(), unsigned(size / 1024));
        return Error::Ok;
}

BMDS_NAMESPACE_END
