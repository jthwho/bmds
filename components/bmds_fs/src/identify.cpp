/**
 * @file      identify.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a file is: by its name, else its first bytes (devplan/filetypes.md T2).
 */

#include <bmds/fileformat.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace Vfs {

        Result<StringView> identify(StringView path, uint32_t flags, const Context &context) {
                if ((flags & IdentifyContent) == 0) {
                        const StringView byName = FileFormat::byName(path);
                        if (!byName.isEmpty()) return makeResult(byName);
                        if ((flags & IdentifyName) != 0) return makeResult(FileFormat::Binary);
                }
                auto [info, err] = stat(path, context);
                if (err.isError()) return makeError<StringView>(err);
                if (info.type == FileType::Directory) return makeResult(FileFormat::Directory);
                if (info.type == FileType::Device) return makeResult(FileFormat::Device);
                if (info.size == 0) return makeResult(FileFormat::Empty);
                auto [file, oerr] = open(path, Read, context);
                if (oerr.isError()) return makeError<StringView>(oerr);
                uint8_t head[FileFormat::HeadBytes];
                size_t  got = 0;
                while (got < sizeof(head)) {
                        auto [n, rerr] = file->read(WritableByteSpan(head + got, sizeof(head) - got), WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                        if (rerr.isError()) return makeError<StringView>(rerr);
                        got += n;
                }
                const bool whole = got < sizeof(head) || info.size == got;
                return makeResult(FileFormat::byContent(Span<const uint8_t>(head, got), whole));
        }

} // namespace Vfs

BMDS_NAMESPACE_END
