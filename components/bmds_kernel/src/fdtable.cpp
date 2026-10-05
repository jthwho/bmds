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
        clearFlag(fd);
        return Error::Ok;
}

Result<int> FdTable::add(FileHandle::Ptr handle, int minFd) {
        if (!isValid(minFd)) return makeError<int>(Error::BadDescriptor);
        for (int fd = minFd; fd < MaxFds; ++fd) {
                if (!_fds[fd]) {
                        _fds[fd] = std::move(handle);
                        clearFlag(fd);
                        return makeResult(fd);
                }
        }
        return makeError<int>(Error::TooManyOpen);
}

Result<int> FdTable::dup(int fd, int minFd) {
        if (!isValid(fd) || !_fds[fd]) return makeError<int>(Error::BadDescriptor);
        return add(_fds[fd], minFd);
}

Error FdTable::dup2(int from, int to) {
        if (!isValid(from) || !isValid(to) || !_fds[from]) return Error::BadDescriptor;
        if (from != to) {
                _fds[to] = _fds[from];
                clearFlag(to);
        }
        return Error::Ok;
}

Error FdTable::close(int fd) {
        if (!isValid(fd) || !_fds[fd]) return Error::BadDescriptor;
        _fds[fd].reset();
        clearFlag(fd);
        return Error::Ok;
}

void FdTable::closeAll() {
        for (FileHandle::Ptr &fd : _fds) fd.reset();
        _closeOnExec = 0;
        return;
}

Error FdTable::setCloseOnExec(int fd, bool on) {
        if (!isOpen(fd)) return Error::BadDescriptor;
        if (on)
                _closeOnExec = static_cast<uint16_t>(_closeOnExec | (1u << fd));
        else
                clearFlag(fd);
        return Error::Ok;
}

void FdTable::closeOnExec() {
        for (int fd = 0; fd < MaxFds; ++fd) {
                if ((_closeOnExec & (1u << fd)) != 0) _fds[fd].reset();
        }
        _closeOnExec = 0;
        return;
}

BMDS_NAMESPACE_END
