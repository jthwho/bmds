/**
 * @file      filehandle.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <new>
#include <bmds/filehandle.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

Result<FileHandle::Ptr> FileHandle::open(Stream &stream, uint8_t mode) {
        if (mode == 0 || (mode & ~ReadWrite) != 0) return makeError<Ptr>(Error::Invalid);
        void *block = Memory::alloc(MemSpace::Psram, sizeof(FileHandle));
        if (block == nullptr) return makeError<Ptr>(Error::NoMem);
        return makeResult(Ptr(new (block) FileHandle(stream, mode)));
}

void FileHandle::unref() {
        if (_refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        Stream *stream = _stream;
        uint8_t mode = _mode;
        this->~FileHandle();
        Memory::free(this);
        // Last: the stream may destroy itself here.
        stream->handleClosed(mode);
        return;
}

BMDS_NAMESPACE_END
