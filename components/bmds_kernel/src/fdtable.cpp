/**
 * @file      fdtable.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <utility>
#include <bmds/fdtable.h>

BMDS_NAMESPACE_BEGIN

Error FdTable::set(int fd, FileHandle::Ptr handle) {
        if (!isValid(fd)) return Error::BadDescriptor;
        _fds[fd] = std::move(handle);
        return Error::Ok;
}

Result<int> FdTable::add(FileHandle::Ptr handle, int minFd) {
        if (!isValid(minFd)) return makeError<int>(Error::BadDescriptor);
        for (int fd = minFd; fd < MaxFds; ++fd) {
                if (!_fds[fd]) {
                        _fds[fd] = std::move(handle);
                        return makeResult(fd);
                }
        }
        return makeError<int>(Error::TooManyOpen);
}

Error FdTable::dup2(int from, int to) {
        if (!isValid(from) || !isValid(to) || !_fds[from]) return Error::BadDescriptor;
        if (from != to) _fds[to] = _fds[from];
        return Error::Ok;
}

Error FdTable::close(int fd) {
        if (!isValid(fd) || !_fds[fd]) return Error::BadDescriptor;
        _fds[fd].reset();
        return Error::Ok;
}

void FdTable::closeAll() {
        for (FileHandle::Ptr &fd : _fds) fd.reset();
        return;
}

BMDS_NAMESPACE_END
