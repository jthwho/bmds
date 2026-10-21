/**
 * @file      fscalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Filesystems served by programs (devplan/netfs.md step 2): slot 123,
 * `fs_serve`, over ServedFs (bmds/servedfs.h).
 */

#include <cstddef>
#include <cstring>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/servedfs.h>
#include <bmds/sys.h>
#include <bmds/sysconf.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::KernelCall;
using bmds::Process;
namespace ServedFs = bmds::ServedFs;

namespace {

        // The ABI's records are ServedFs's.
        static_assert(sizeof(bmds_fs_request_t) == sizeof(ServedFs::Request) &&
                              offsetof(bmds_fs_request_t, handle) == offsetof(ServedFs::Request, handle) &&
                              offsetof(bmds_fs_request_t, length) == offsetof(ServedFs::Request, length) &&
                              offsetof(bmds_fs_request_t, time_ns) == offsetof(ServedFs::Request, timeNs),
                      "bmds_fs_request_t is ServedFs::Request");
        static_assert(sizeof(bmds_fs_reply_t) == sizeof(ServedFs::Reply) &&
                              offsetof(bmds_fs_reply_t, count) == offsetof(ServedFs::Reply, count) &&
                              offsetof(bmds_fs_reply_t, handle) == offsetof(ServedFs::Reply, handle),
                      "bmds_fs_reply_t is ServedFs::Reply");
        static_assert(sizeof(bmds_fs_entry_t) == sizeof(ServedFs::Entry) &&
                              offsetof(bmds_fs_entry_t, modified_ns) == offsetof(ServedFs::Entry, modifiedNs) &&
                              offsetof(bmds_fs_entry_t, name_len) == offsetof(ServedFs::Entry, nameLength),
                      "bmds_fs_entry_t is ServedFs::Entry");
        static_assert(sizeof(bmds_fs_space_t) == sizeof(ServedFs::Info), "bmds_fs_space_t is ServedFs::Info");
        static_assert(BMDS_FS_STAT == ServedFs::Stat && BMDS_FS_SYMLINK == ServedFs::SymLink &&
                              BMDS_FS_O_READ == ServedFs::OpenRead && BMDS_FS_O_TRUNCATE == ServedFs::OpenTruncate &&
                              BMDS_FS_REPLACE == ServedFs::RenameReplace,
                      "the ABI's ops and flags are ServedFs's");
        static_assert(BMDS_FS_DATA_MIN == ServedFs::MinData && BMDS_FS_DATA_MAX == ServedFs::MaxData &&
                              BMDS_FS_DATA_DEFAULT == ServedFs::DefaultData &&
                              BMDS_FS_PATH_MAX == ServedFs::MaxPathBytes &&
                              BMDS_FS_REQUEST_MAX(4096u) == ServedFs::requestMax(4096),
                      "the ABI's limits are ServedFs's");

        // devplan/netfs.md: the keys are an interface once shipped.
        BMDS_SYSCONF(netfsTimeout, "netfs/timeout", bmds::Config::Type::Int, "30",
                     "Seconds a filesystem served by a program (sshfs) has to answer a request", 1, 600, nullptr,
                     nullptr)
        BMDS_SYSCONF(netfsAttr, "netfs/attr-ms", bmds::Config::Type::Int, "1000",
                     "Milliseconds a served filesystem's file attributes are kept (0: asked every time)", 0, 60000,
                     nullptr, nullptr)

        uint32_t setting(const char *key, int32_t fallback) {
                auto [v, err] = bmds::SysConf::getInt(bmds::StringView(key));
                return uint32_t(err.isOk() ? v : fallback);
        }

} // namespace

extern "C" {

int32_t bmds_k_fs_serve(const bmds_fs_serve_t *config) {
        KernelCall      call;
        bmds_fs_serve_t c;
        int32_t         r = bmds::detail::copyIn(config, c);
        if (r < 0) return r;
        if ((c.flags & ~BMDS_O_CLOEXEC) != 0 || c.path == nullptr || c.type == nullptr) return -BMDS_EINVAL;
        const size_t typeBytes = strnlen(c.type, ServedFs::MaxType);
        if (typeBytes == 0 || typeBytes >= ServedFs::MaxType) return -BMDS_EINVAL;
        bmds::String path;
        r = call->absolutePath(c.path, path);
        if (r < 0) return r;
        ServedFs::Config k;
        k.path = path.view();
        k.type = bmds::StringView(c.type, typeBytes);
        k.maxData = c.max_data != 0 ? c.max_data : ServedFs::DefaultData;
        k.timeoutMs = setting(bmdsConf_netfsTimeout.key, 30) * 1000;
        k.attrMs = setting(bmdsConf_netfsAttr.key, 1000);
        k.errorOf = Process::errorFor;
        auto [handle, err] = ServedFs::serve(k);
        if (err == Error::Full) return -BMDS_ENOSPC;
        if (err.isError()) return Process::errnoFor(err);
        return call->addHandle(std::move(handle), c.flags);
}

} // extern "C"
