/**
 * @file      programicons.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/builtinprograms.h>
#include <bmds/memory.h>
#include <bmds/programicons.h>
#include <bmds/programimage.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A file through the VFS, read where asked.
        class FileSource final : public ProgramImage::Source {
                public:
                        explicit FileSource(FileHandle &file) : _file(file) {}

                        Error readAt(uint32_t offset, WritableByteSpan out) override {
                                auto [pos, err] = _file.stream().seek(offset, Stream::SeekFrom::Start);
                                if (err.isError()) return err;
                                (void)pos;
                                size_t got = 0;
                                while (got < out.size()) {
                                        auto [n, rerr] = _file.read(out.mid(got), WaitForever);
                                        if (rerr.isError()) return rerr;
                                        if (n == 0) return Error::EndOfFile;
                                        got += n;
                                }
                                return Error::Ok;
                        }

                private:
                        FileHandle &_file;
        };

        // Better for @p side: that size, else the smallest bigger, else the biggest.
        bool better(uint16_t a, uint16_t b, uint16_t side) {
                if (a == side) return b != side;
                if (b == side) return false;
                if ((a > side) != (b > side)) return a > side;
                return a > side ? a < b : a > b;
        }

} // namespace

Error ProgramIcons::read(StringView path, const Vfs::Context &context, uint16_t side, MemSpace space, Image &out) {
        out = Image();
        if (const ProgramEntry *builtin = Vfs::builtinProgram(path, context); builtin != nullptr) {
                const ProgramIcon *best = nullptr;
                for (uint8_t i = 0; i < builtin->iconCount; ++i)
                        if (best == nullptr || better(builtin->icons[i].width, best->width, side))
                                best = &builtin->icons[i];
                if (best == nullptr) return Error::NotFound;
                out.png = static_cast<uint8_t *>(Memory::alloc(space, best->bytes));
                if (out.png == nullptr) return Error::NoMem;
                std::memcpy(out.png, best->png, best->bytes);
                out.bytes = best->bytes;
                out.width = best->width;
                out.height = best->height;
                return Error();
        }
        auto [info, serr] = Vfs::stat(path, context);
        if (serr.isError()) return Error::NotFound;
        if (info.type != FileType::File || info.size > ProgramImage::MaxImageBytes * 4u) return Error::NotFound;
        auto [file, err] = Vfs::open(path, Vfs::Read, context);
        if (err.isError()) return Error::NotFound;
        FileSource            source(*file);
        ProgramImage::IconRef ref;
        err = ProgramImage::findIcon(source, static_cast<uint32_t>(info.size), side, ref);
        if (err.isError()) return err;
        if (ref.length > MaxImageBytes) return Error::NotFound;
        out.png = static_cast<uint8_t *>(Memory::alloc(space, ref.length));
        if (out.png == nullptr) return Error::NoMem;
        err = source.readAt(ref.offset, WritableByteSpan(out.png, ref.length));
        if (err.isError()) {
                Memory::free(out.png);
                out = Image();
                return err;
        }
        out.bytes = ref.length;
        out.width = ref.width;
        out.height = ref.height;
        return Error();
}

BMDS_NAMESPACE_END
