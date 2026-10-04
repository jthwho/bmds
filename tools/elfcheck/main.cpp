/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-elfcheck: says whether files are BMDS programs the kernel will load
 * (the same checks as the loader), and what it would do with them.
 *
 *   bmds-elfcheck [-q] program.elf...
 *
 * Exit status: 0 if all are loadable, 1 if any isn't, 2 on usage errors.
 */

#include <cstdio>
#include <cstring>
#include <vector>
#include <bmds/programimage.h>

namespace ProgramImage = bmds::ProgramImage;

namespace {

        bool readFile(const char *path, std::vector<uint8_t> &out) {
                FILE *f = std::fopen(path, "rb");
                if (f == nullptr) return false;
                uint8_t buf[65536];
                size_t  n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
                const bool ok = std::ferror(f) == 0;
                std::fclose(f);
                return ok;
        }

} // namespace

int main(int argc, char **argv) {
        bool quiet = false;
        int  first = 1;
        if (argc > 1 && std::strcmp(argv[1], "-q") == 0) {
                quiet = true;
                first = 2;
        }
        if (first >= argc) {
                std::fprintf(stderr, "usage: bmds-elfcheck [-q] program.elf...\n");
                return 2;
        }
        int status = 0;
        for (int i = first; i < argc; ++i) {
                std::vector<uint8_t> bytes;
                if (!readFile(argv[i], bytes) || bytes.size() > UINT32_MAX) {
                        std::fprintf(stderr, "%s: cannot read\n", argv[i]);
                        status = 1;
                        continue;
                }
                ProgramImage::MemorySource source(bmds::ByteSpan(bytes.data(), bytes.size()));
                ProgramImage::Info         info;
                ProgramImage::Problem      problem;
                const bmds::Error          err =
                        ProgramImage::inspect(source, static_cast<uint32_t>(bytes.size()), info, problem);
                if (err.isError()) {
                        if (problem.hasValue) {
                                std::fprintf(stderr, "%s: %s (0x%x)\n", argv[i], problem.what, problem.value);
                        } else {
                                std::fprintf(stderr, "%s: %s\n", argv[i],
                                             problem.what != nullptr ? problem.what : err.desc());
                        }
                        status = 1;
                        continue;
                }
                if (!quiet) {
                        std::printf("%s: BMDS program, ABI %u: image %u bytes (%u in the file), entry +0x%x, "
                                    "stack %u%s, %u relocations",
                                    argv[i], info.abiVersion, info.imageSize, info.fileSize, info.entry, info.stackSize,
                                    info.stackSize == 0 ? " (default)" : "", info.relocations);
                        if (info.tlsSize != 0) std::printf(", %u bytes thread-local", info.tlsSize);
                        std::printf("\n");
                }
        }
        return status;
}
