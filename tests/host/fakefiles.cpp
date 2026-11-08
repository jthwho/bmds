/**
 * @file      fakefiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The file system calls on the host: the host's files, read only.
 */

#include "fakefiles.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

        int32_t fail() {
                return errno == ENOENT ? -BMDS_ENOENT : -BMDS_EIO;
        }

        int32_t fileOpen(const char *path, uint32_t flags) {
                if (flags != BMDS_O_READ) return -BMDS_EINVAL;
                const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
                return fd >= 0 ? fd : fail();
        }

        int32_t fileRead(int32_t fd, void *buf, uint32_t size) {
                const ssize_t n = ::read(fd, buf, size);
                return n >= 0 ? int32_t(n) : fail();
        }

        int32_t fileFstat(int32_t fd, bmds_stat_t *info) {
                struct stat st;
                if (::fstat(fd, &st) != 0) return fail();
                const uint32_t size = info->size;
                std::memset(info, 0, size);
                info->size = size;
                info->type = BMDS_TYPE_FILE;
                info->open_flags = BMDS_O_READ;
                info->file_size = uint64_t(st.st_size);
                return 0;
        }

        int32_t fileClose(int32_t fd) {
                return ::close(fd) == 0 ? 0 : fail();
        }

} // namespace

namespace FakeFiles {

        void install(bmds_sys_fn_t *slots) {
                slots[BMDS_SYS_OPEN] = reinterpret_cast<bmds_sys_fn_t>(fileOpen);
                slots[BMDS_SYS_READ] = reinterpret_cast<bmds_sys_fn_t>(fileRead);
                slots[BMDS_SYS_FSTAT] = reinterpret_cast<bmds_sys_fn_t>(fileFstat);
                slots[BMDS_SYS_CLOSE] = reinterpret_cast<bmds_sys_fn_t>(fileClose);
                return;
        }

} // namespace FakeFiles
