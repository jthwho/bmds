/**
 * @file      loader.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The program format (ProgramImage): what is accepted and refused, and
 * relocation, on hand-built ELF files and on the SDK's hello.elf.
 */

#include <doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <bmds/program.h>
#include <bmds/programimage.h>
#include <bmds/sys.h>

using bmds::Error;
namespace ProgramImage = bmds::ProgramImage;

namespace {

        void put16(std::vector<uint8_t> &b, size_t at, uint16_t v) {
                std::memcpy(b.data() + at, &v, 2);
        }
        void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
                std::memcpy(b.data() + at, &v, 4);
        }
        uint32_t get32(const uint8_t *p) {
                uint32_t v;
                std::memcpy(&v, p, 4);
                return v;
        }

        constexpr uint32_t R32 = 1, RHi20 = 26, RPcHi20 = 23, RLo12I = 27, RBranch = 16, R64 = 2;
        constexpr uint16_t ShnAbs = 0xfff1;

        // A small program built by hand: a 64-byte image (code and data), its
        // BMDS note, one symbol per section kind and relocations of .text.
        struct Builder {
                        uint16_t type = 2; // ET_EXEC
                        uint16_t machine = 243;
                        uint32_t flags = 0x3; // RVC, single-float
                        uint32_t entry = 0;
                        uint32_t vaddr = 0;
                        uint32_t memsz = 96;
                        bool     note = true;
                        uint32_t noteAbi = 1;
                        uint32_t noteStack = 0;
                        uint32_t noteFlags = 0;
                        bool     interp = false;
                        bool     dynamic = false;
                        bool     secondLoad = false;
                        bool     sections = true;
                        uint32_t relaType = 4; // SHT_RELA (9: SHT_REL)

                        struct Reloc {
                                        uint32_t offset, type, sym;
                        };
                        std::vector<Reloc> relocs;

                        // Image: words at 0..63; the test sets what they hold.
                        std::vector<uint32_t> words = std::vector<uint32_t>(16, 0x00000013); // nop

                        // Symbols: 1 = in .text (moves), 2 = absolute, 3 = undefined.
                        std::vector<uint8_t> build() const {
                                const size_t phnum = 1 + (note ? 1 : 0) + (interp ? 1 : 0) + (dynamic ? 1 : 0) +
                                                     (secondLoad ? 1 : 0);
                                const size_t         phoff = 52;
                                const size_t         imageOff = 0x100;
                                const size_t         noteOff = imageOff + 64;
                                const size_t         symOff = noteOff + 36;
                                const size_t         symCount = 4;
                                const size_t         relaOff = symOff + symCount * 16;
                                const size_t         shoff = relaOff + relocs.size() * 12;
                                const size_t         shnum = 4;
                                std::vector<uint8_t> b(shoff + shnum * 40, 0);
                                std::memcpy(b.data(),
                                            "\x7f"
                                            "ELF\x01\x01\x01",
                                            7);
                                put16(b, 16, type);
                                put16(b, 18, machine);
                                put32(b, 20, 1);
                                put32(b, 24, entry);
                                put32(b, 28, static_cast<uint32_t>(phoff));
                                put32(b, 32, sections ? static_cast<uint32_t>(shoff) : 0);
                                put32(b, 36, flags);
                                put16(b, 40, 52);
                                put16(b, 42, 32);
                                put16(b, 44, static_cast<uint16_t>(phnum));
                                put16(b, 46, 40);
                                put16(b, 48, sections ? static_cast<uint16_t>(shnum) : 0);
                                size_t ph = phoff;
                                auto   phdr = [&](uint32_t t, uint32_t off, uint32_t va, uint32_t fsz, uint32_t msz) {
                                        put32(b, ph, t);
                                        put32(b, ph + 4, off);
                                        put32(b, ph + 8, va);
                                        put32(b, ph + 12, va);
                                        put32(b, ph + 16, fsz);
                                        put32(b, ph + 20, msz);
                                        put32(b, ph + 24, 7);
                                        put32(b, ph + 28, 16);
                                        ph += 32;
                                };
                                phdr(1, imageOff, vaddr, 64, memsz);
                                if (secondLoad) phdr(1, imageOff, 0x1000, 64, 64);
                                if (note) phdr(4, noteOff, 0, 36, 36);
                                if (interp) phdr(3, noteOff, 0, 4, 4);
                                if (dynamic) phdr(2, noteOff, 0, 8, 8);
                                for (size_t i = 0; i < words.size(); ++i) put32(b, imageOff + i * 4, words[i]);
                                put32(b, noteOff, 5);
                                put32(b, noteOff + 4, 16);
                                put32(b, noteOff + 8, 1);
                                std::memcpy(b.data() + noteOff + 12, "BMDS", 5);
                                put32(b, noteOff + 20, 16);
                                put32(b, noteOff + 24, noteAbi);
                                put32(b, noteOff + 28, noteStack);
                                put32(b, noteOff + 32, noteFlags);
                                // Symbols (index 0 is null): 1 in .text, 2 absolute, 3 undefined.
                                put16(b, symOff + 16 + 14, 1);
                                put16(b, symOff + 32 + 14, ShnAbs);
                                put16(b, symOff + 48 + 14, 0);
                                for (size_t i = 0; i < relocs.size(); ++i) {
                                        put32(b, relaOff + i * 12, relocs[i].offset);
                                        put32(b, relaOff + i * 12 + 4, (relocs[i].sym << 8) | relocs[i].type);
                                }
                                // Sections: 0 null, 1 .text, 2 .symtab, 3 .rela.text.
                                auto shdr = [&](size_t i, uint32_t t, uint32_t fl, uint32_t addr, uint32_t off,
                                                uint32_t size, uint32_t link, uint32_t info, uint32_t entsize) {
                                        const size_t at = shoff + i * 40;
                                        put32(b, at + 4, t);
                                        put32(b, at + 8, fl);
                                        put32(b, at + 12, addr);
                                        put32(b, at + 16, off);
                                        put32(b, at + 20, size);
                                        put32(b, at + 24, link);
                                        put32(b, at + 28, info);
                                        put32(b, at + 36, entsize);
                                };
                                shdr(1, 1, 0x6, 0, static_cast<uint32_t>(imageOff), 64, 0, 0, 0);
                                shdr(2, 2, 0, 0, static_cast<uint32_t>(symOff), static_cast<uint32_t>(symCount * 16), 0,
                                     0, 16);
                                shdr(3, relaType, 0, 0, static_cast<uint32_t>(relaOff),
                                     static_cast<uint32_t>(relocs.size() * 12), 2, 1, 12);
                                return b;
                        }
        };

        struct Checked {
                        Error                 err;
                        ProgramImage::Info    info;
                        ProgramImage::Problem problem;
        };

        Checked check(const std::vector<uint8_t> &file) {
                Checked                    c;
                ProgramImage::MemorySource source(bmds::ByteSpan(file.data(), file.size()));
                c.err = ProgramImage::inspect(source, static_cast<uint32_t>(file.size()), c.info, c.problem);
                return c;
        }

        std::string why(const Checked &c) {
                return c.problem.what != nullptr ? c.problem.what : "";
        }

        // Loads @p file at @p base; the image (memsz bytes).
        std::vector<uint8_t> loadAt(const std::vector<uint8_t> &file, uint32_t base, Error &err) {
                Checked c = check(file);
                err = c.err;
                if (err.isError()) return {};
                std::vector<uint8_t>       image(c.info.imageSize, 0xAA);
                ProgramImage::MemorySource source(bmds::ByteSpan(file.data(), file.size()));
                ProgramImage::Problem      problem;
                err = ProgramImage::load(source, static_cast<uint32_t>(file.size()), c.info, image.data(), base,
                                         problem);
                return image;
        }

        // Reads @p file for placing (read()) and places it at @p base; the image.
        std::vector<uint8_t> placeAt(const std::vector<uint8_t> &file, uint32_t base, Error &err) {
                Checked c = check(file);
                err = c.err;
                if (err.isError()) return {};
                std::vector<uint8_t>             bytes(c.info.fileSize, 0x55);
                std::vector<ProgramImage::Patch> patches(c.info.relocations + 1, 0xFFFFFFFF);
                ProgramImage::MemorySource       source(bmds::ByteSpan(file.data(), file.size()));
                ProgramImage::Problem            problem;
                err = ProgramImage::read(source, static_cast<uint32_t>(file.size()), c.info, bytes.data(),
                                         patches.data(), problem);
                if (err.isError()) return {};
                CHECK(patches.back() == 0xFFFFFFFF); // no more than counted
                std::vector<uint8_t> image(c.info.imageSize, 0xAA);
                ProgramImage::place(c.info, bytes.data(), patches.data(), image.data(), base);
                return image;
        }

} // namespace

TEST_CASE("Program format: what is not a BMDS program") {
        Builder good;
        good.relocs = {{0, RBranch, 1}};
        CHECK(check(good.build()).err == Error::Ok);

        auto refused = [](Builder b, Error want, const char *what) {
                if (b.relocs.empty()) b.relocs = {{0, RBranch, 1}};
                const Checked c = check(b.build());
                CAPTURE(what);
                CHECK(c.err == want);
                CHECK(why(c).find(what) != std::string::npos);
        };
        {
                std::vector<uint8_t> text = {'#', '!', '/', 'b', 'i', 'n', '/', 's', 'h', '\n'};
                CHECK(check(text).err == Error::NotSupported);
                CHECK(why(check(text)) == "not an ELF file (too short)");
        }
        Builder b;
        b.machine = 62;
        refused(b, Error::NotSupported, "not a RISC-V program");
        b = Builder();
        b.type = 3;
        refused(b, Error::NotSupported, "ELF type is not EXEC");
        b = Builder();
        b.flags = 0x1; // soft float
        refused(b, Error::NotSupported, "wrong ABI");
        b = Builder();
        b.note = false;
        refused(b, Error::NotSupported, "no BMDS note");
        b = Builder();
        b.noteAbi = 99;
        refused(b, Error::NotSupported, "built for a newer BMDS");
        b = Builder();
        b.noteStack = BMDS_MAX_STACK_SIZE + 1;
        refused(b, Error::NotSupported, "too much stack");
        b = Builder();
        b.noteFlags = 2;
        refused(b, Error::NotSupported, "unknown program flags");
        b = Builder();
        b.interp = true;
        refused(b, Error::NotSupported, "dynamic linker");
        b = Builder();
        b.dynamic = true;
        refused(b, Error::NotSupported, "dynamically linked");
        b = Builder();
        b.secondLoad = true;
        refused(b, Error::NotSupported, "more than one PT_LOAD");
        b = Builder();
        b.vaddr = 0x1000;
        refused(b, Error::NotSupported, "not linked at address 0");
        b = Builder();
        b.entry = 64;
        refused(b, Error::Invalid, "entry point outside");
        b = Builder();
        b.sections = false;
        refused(b, Error::NotSupported, "no section headers");
        b = Builder();
        b.relaType = 9;
        refused(b, Error::NotSupported, "REL relocations");
        b = Builder();
        b.relocs = {{0, R64, 1}};
        refused(b, Error::NotSupported, "unsupported relocation type");
        b = Builder();
        b.relocs = {{62, R32, 1}};
        refused(b, Error::Invalid, "relocation outside its section");
        b = Builder();
        b.relocs = {{0, R32, 9}};
        refused(b, Error::Invalid, "bad symbol index");
        b = Builder();
        b.relocs = {{0, RPcHi20, 2}};
        b.words[0] = 0x00000517; // auipc a0, 0: PC-relative to an absolute symbol
        refused(b, Error::NotSupported, "PC-relative reference to an absolute symbol");
}

TEST_CASE("Program format: the display flag") {
        Builder b;
        b.noteFlags = BMDS_PROGRAM_DISPLAY;
        const Checked c = check(b.build());
        REQUIRE(c.err == Error::Ok);
        CHECK(c.info.flags == BMDS_PROGRAM_DISPLAY);
}

TEST_CASE("Program format: relocation") {
        Builder b;
        b.words[0] = 0x12345537; // lui a0, 0x12345     (HI20 to .text)
        b.words[1] = 0x67850513; // addi a0, a0, 0x678  (LO12_I: unchanged)
        b.words[2] = 0x00000020; // .word 0x20          (32 to .text)
        b.words[3] = 0x00000040; // .word 0x40          (32 to an absolute symbol)
        b.words[4] = 0x00000537; // lui a0, 0           (PCREL_HI20 to undefined, made lui)
        b.words[5] = 0x000005b7; // lui a1, 0           (HI20 to undefined: unchanged)
        b.relocs = {{0, RHi20, 1}, {4, RLo12I, 1}, {8, R32, 1}, {12, R32, 2}, {16, RPcHi20, 3}, {20, RHi20, 3}};
        const Checked c = check(b.build());
        REQUIRE(c.err == Error::Ok);
        CHECK(c.info.relocations == 2);
        CHECK(c.info.imageSize == 96);
        CHECK(c.info.fileSize == 64);
        CHECK(c.info.abiVersion == 1);
        CHECK(c.info.flags == 0);

        Error                      err;
        const std::vector<uint8_t> image = loadAt(b.build(), 0x48003000, err);
        REQUIRE(err == Error::Ok);
        CHECK(get32(&image[0]) == 0x12345537u + 0x48003000u);
        CHECK(get32(&image[4]) == 0x67850513u);
        CHECK(get32(&image[8]) == 0x48003020u);
        CHECK(get32(&image[12]) == 0x40u);
        CHECK(get32(&image[16]) == 0x00000537u);
        CHECK(get32(&image[20]) == 0x000005b7u);
        for (size_t i = 64; i < 96; ++i) CHECK(image[i] == 0); // bss
        // Read once and placed: the same image, at any base.
        for (uint32_t base : {0u, 0x2000u, 0x48003000u}) {
                CAPTURE(base);
                Error placed;
                CHECK(placeAt(b.build(), base, placed) == loadAt(b.build(), base, err));
                CHECK(placed == Error::Ok);
        }
        // The upper immediate wraps; the register and opcode stay.
        b.words[0] = 0xfffff537;
        CHECK(get32(&loadAt(b.build(), 0x2000, err)[0]) == 0x00001537u);
        CHECK(get32(&placeAt(b.build(), 0x2000, err)[0]) == 0x00001537u);
        // Not 4096-aligned: refused.
        (void)loadAt(b.build(), 0x48003010, err);
        CHECK(err == Error::Invalid);
}

#ifdef BMDS_SDK_EXAMPLES
namespace {

        std::vector<uint8_t> readFile(const std::string &path) {
                std::vector<uint8_t> file;
                FILE                *f = std::fopen(path.c_str(), "rb");
                REQUIRE_MESSAGE(f != nullptr, path);
                uint8_t buf[4096];
                size_t  n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
                std::fclose(f);
                return file;
        }

        uint32_t word(const std::vector<uint8_t> &f, size_t at) {
                REQUIRE(at + 4 <= f.size());
                return f[at] | f[at + 1] << 8 | f[at + 2] << 16 | uint32_t(f[at + 3]) << 24;
        }

        uint16_t half(const std::vector<uint8_t> &f, size_t at) {
                REQUIRE(at + 2 <= f.size());
                return static_cast<uint16_t>(f[at] | f[at + 1] << 8);
        }

} // namespace

// The startup code hands musl __tls_base as the TLS template's address; the
// linker computes thread-local offsets from the TLS segment's start. They
// must be the same address (they weren't, once, when a program had no
// initialized thread-locals: localtime's buffer then overwrote bmds_sys).
// musl itself has no thread-locals; progtest has both kinds.
TEST_CASE("Program format: the SDK's programs describe their TLS segment") {
        for (const std::string name : {"hello", "cat", "ls", "progtest"}) {
                CAPTURE(name);
                const std::vector<uint8_t> f = readFile(std::string(BMDS_SDK_EXAMPLES) + "/" + name + ".debug.elf");
                const uint32_t             phoff = word(f, 0x1C), shoff = word(f, 0x20);
                const uint16_t             phentsize = half(f, 0x2A), phnum = half(f, 0x2C);
                const uint16_t             shentsize = half(f, 0x2E), shnum = half(f, 0x30);
                uint32_t                   tls = 0, tlsSize = 0;
                for (uint16_t i = 0; i < phnum; ++i) {
                        const size_t ph = phoff + size_t(i) * phentsize;
                        if (word(f, ph) == 7) { // PT_TLS
                                tls = word(f, ph + 8);
                                tlsSize = word(f, ph + 20);
                        }
                }
                if (name == "progtest") CHECK(tlsSize > 0);
                if (tlsSize == 0) continue;
                bool found = false;
                for (uint16_t i = 0; i < shnum && !found; ++i) {
                        const size_t sh = shoff + size_t(i) * shentsize;
                        if (word(f, sh + 4) != 2) continue; // SHT_SYMTAB
                        const uint32_t off = word(f, sh + 16), size = word(f, sh + 20), entsize = word(f, sh + 36);
                        const size_t   strSh = shoff + size_t(word(f, sh + 24)) * shentsize;
                        const uint32_t strOff = word(f, strSh + 16);
                        for (uint32_t s = 0; s + entsize <= size; s += entsize) {
                                const char *symName = reinterpret_cast<const char *>(&f[strOff + word(f, off + s)]);
                                if (std::strcmp(symName, "__tls_base") != 0) continue;
                                CHECK(word(f, off + s + 4) == tls);
                                found = true;
                                break;
                        }
                }
                CHECK(found);
        }
}
#endif

#ifdef BMDS_SDK_HELLO
TEST_CASE("Program format: the SDK's hello.elf") {
        std::vector<uint8_t> file;
        {
                FILE *f = std::fopen(BMDS_SDK_HELLO, "rb");
                REQUIRE(f != nullptr);
                uint8_t buf[4096];
                size_t  n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
                std::fclose(f);
        }
        const Checked c = check(file);
        CAPTURE(why(c));
        REQUIRE(c.err == Error::Ok);
        CHECK(c.info.abiVersion == BMDS_ABI_VERSION);
        CHECK(c.info.relocations > 0);
        // musl keeps errno in its thread descriptor; the SDK's own is each thread's signal hint
        // and vfork state (linux.c), small enough for musl's built-in TLS (no mapping at start).
        CHECK(c.info.tlsSize > 0);
        CHECK(c.info.tlsSize <= 16);

        // Loaded at 0 it is the file's segment; loaded anywhere else, exactly
        // the patched words differ, by the distance (whole words, or upper
        // immediates).
        Error                      err;
        const std::vector<uint8_t> at0 = loadAt(file, 0, err);
        REQUIRE(err == Error::Ok);
        CHECK(std::memcmp(at0.data(), file.data() + c.info.fileOffset, c.info.fileSize) == 0);
        const uint32_t             b1 = 0x48010000, b2 = 0x48ABC000;
        const std::vector<uint8_t> i1 = loadAt(file, b1, err);
        REQUIRE(err == Error::Ok);
        const std::vector<uint8_t> i2 = loadAt(file, b2, err);
        REQUIRE(err == Error::Ok);
        // Each differing byte belongs to a patch starting at most 3 bytes
        // before it (a base's low 12 bits are zero, so a patch's first byte
        // never changes).
        auto patchAt = [&](size_t p) {
                if (p + 4 > i1.size()) return false;
                const uint32_t w0 = get32(&at0[p]), w1 = get32(&i1[p]), w2 = get32(&i2[p]);
                const bool     word = w1 - w0 == b1 && w2 - w0 == b2;
                const bool     upper = (w1 & 0xfff) == (w0 & 0xfff) && (w2 & 0xfff) == (w0 & 0xfff) &&
                                   ((w1 - w0) & 0xfffff000u) == b1 && ((w2 - w0) & 0xfffff000u) == b2;
                return word || upper;
        };
        size_t differing = 0;
        size_t consumed = 0; // bytes before this belong to patches already counted
        for (size_t at = 0; at < i1.size(); ++at) {
                if (i1[at] == i2[at] || at < consumed) continue;
                bool found = false;
                for (size_t p = at >= 3 ? at - 3 : 0; p <= at && !found; ++p) {
                        if (p < consumed || !patchAt(p)) continue;
                        found = true;
                        consumed = p + 4;
                        ++differing;
                }
                CAPTURE(at);
                CHECK(found);
                if (!found) break;
        }
        CHECK(differing == c.info.relocations);
        // Read once and placed: load()'s image at any base.
        for (uint32_t base : {0u, b1, b2}) {
                CAPTURE(base);
                Error placed;
                CHECK(placeAt(file, base, placed) == loadAt(file, base, err));
                CHECK(placed == Error::Ok);
        }
}
#endif

TEST_CASE("Program format: damaged files are refused, never crash") {
        // Random damage to a valid program (the SDK's hello.elf if built, else
        // a hand-built one): inspect() either refuses it or accepts it, and an
        // accepted one loads within its image. ASan/UBSan runs catch the rest.
        std::vector<uint8_t> original;
#ifdef BMDS_SDK_HELLO
        {
                FILE *f = std::fopen(BMDS_SDK_HELLO, "rb");
                REQUIRE(f != nullptr);
                uint8_t buf[4096];
                size_t  n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) original.insert(original.end(), buf, buf + n);
                std::fclose(f);
        }
#else
        Builder b;
        b.relocs = {{0, RHi20, 1}, {8, R32, 1}, {16, RPcHi20, 3}};
        original = b.build();
#endif
        uint32_t seed = 12345;
        auto     next = [&seed] {
                seed = seed * 1103515245u + 12345u;
                return seed >> 8;
        };
        int accepted = 0;
        for (int round = 0; round < 3000; ++round) {
                std::vector<uint8_t> file = original;
                const int            hits = 1 + static_cast<int>(next() % 4);
                for (int h = 0; h < hits; ++h) {
                        // Mostly the headers, where the structure is.
                        const size_t limit = next() % 2 ? 256 : file.size();
                        file[next() % limit] = static_cast<uint8_t>(next());
                }
                if (next() % 8 == 0) file.resize(next() % file.size()); // truncated
                Error                      err;
                const std::vector<uint8_t> image = loadAt(file, 0x48010000, err);
                if (err.isOk()) {
                        ++accepted;
                        // Read once and placed, an accepted file gives the same image.
                        CHECK(placeAt(file, 0x48010000, err) == image);
                }
        }
        CHECK(accepted > 0); // some damage lands where nothing checks (code, debug info)
}

// The compatibility corpus (tests/compat/v*): programs built with an older
// SDK, kept as built, must still pass the loader's check and relocate.
// (tools/bmds-compat runs them on a board.)
TEST_CASE("Program format: the compatibility corpus still loads") {
        size_t programs = 0;
        for (const auto &dir : std::filesystem::directory_iterator(BMDS_COMPAT_DIR)) {
                if (!dir.is_directory()) continue;
                for (const auto &entry : std::filesystem::directory_iterator(dir.path())) {
                        if (entry.path().extension() != ".elf") continue;
                        CAPTURE(entry.path().string());
                        std::vector<uint8_t> file;
                        FILE                *f = std::fopen(entry.path().c_str(), "rb");
                        REQUIRE(f != nullptr);
                        uint8_t buf[4096];
                        size_t  n;
                        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
                        std::fclose(f);
                        const Checked c = check(file);
                        CAPTURE(why(c));
                        REQUIRE(c.err == Error::Ok);
                        CHECK(c.info.abiVersion >= 1);
                        CHECK(c.info.abiVersion <= BMDS_ABI_VERSION);
                        Error err;
                        (void)loadAt(file, 0x48ABC000, err);
                        CHECK(err == Error::Ok);
                        ++programs;
                }
        }
        CHECK(programs >= 3);
}

namespace {

        constexpr uint32_t Page = ProgramImage::SharePageBytes;

        // place() of @p info at @p base, and placePages() with the shared pages left out: each
        // private page is place()'s bytes there (zeros past the image), each shared page the
        // entry's bytes as they are, which place()'s image holds at every base.
        void checkPages(const ProgramImage::Info &info, const std::vector<uint8_t> &bytes,
                        const std::vector<ProgramImage::Patch> &patches, uint32_t base) {
                CAPTURE(base);
                std::vector<uint8_t> image(info.imageSize, 0xAA);
                ProgramImage::place(info, bytes.data(), patches.data(), image.data(), base);
                const uint32_t                    count = ProgramImage::pageCount(info.imageSize);
                std::vector<std::vector<uint8_t>> storage(count);
                std::vector<uint8_t *>            pages(count, nullptr);
                for (uint32_t p = 0; p < count; ++p) {
                        if (ProgramImage::isSharedPage(info, p)) continue;
                        storage[p].assign(Page, 0xAA);
                        pages[p] = storage[p].data();
                }
                ProgramImage::placePages(info, bytes.data(), patches.data(), pages.data(), base);
                for (uint32_t p = 0; p < count; ++p) {
                        CAPTURE(p);
                        const uint32_t start = p * Page;
                        const uint32_t inImage = std::min(info.imageSize - start, Page);
                        if (pages[p] == nullptr) {
                                REQUIRE(start + Page <= bytes.size());
                                CHECK(std::memcmp(image.data() + start, bytes.data() + start, Page) == 0);
                                continue;
                        }
                        CHECK(std::memcmp(image.data() + start, pages[p], inImage) == 0);
                        CHECK(std::all_of(pages[p] + inImage, pages[p] + Page, [](uint8_t v) { return v == 0; }));
                }
        }

} // namespace

TEST_CASE("Program format: shared pages") {
        // Three pages and a bit: page 0 untouched, page 1 holds a lui straddling into page 2,
        // page 3 starts the writable data; the file ends inside page 3.
        ProgramImage::Info info;
        info.fileSize = 3 * Page + 100;
        info.imageSize = 3 * Page + 4000;
        info.writableOffset = 3 * Page;
        std::vector<uint8_t> bytes(info.fileSize);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 7 + 3);
        const uint32_t straddle = 2 * Page - 2; // RVC: a 4-byte instruction 2-aligned
        put32(bytes, straddle, 0x00000537);     // lui a0, 0
        put32(bytes, 3 * Page + 8, 0x1234);     // a pointer in .data
        std::vector<ProgramImage::Patch> patches = {straddle | ProgramImage::PatchUpper, 3 * Page + 8};
        info.relocations = uint32_t(patches.size());
        for (uint32_t at : {straddle, straddle + 3, 3 * Page + 8, 3 * Page + 11})
                info.patchedPages[at / Page / 32] |= 1u << (at / Page % 32);

        CHECK(ProgramImage::pageCount(info.imageSize) == 4);
        CHECK(ProgramImage::isSharedPage(info, 0));
        CHECK_FALSE(ProgramImage::isSharedPage(info, 1)); // the patch's first half
        CHECK_FALSE(ProgramImage::isSharedPage(info, 2)); // its second half
        CHECK_FALSE(ProgramImage::isSharedPage(info, 3)); // writable, and the file ends in it
        CHECK_FALSE(ProgramImage::isSharedPage(info, 4)); // past the image
        CHECK(ProgramImage::sharedPageCount(info) == 1);
        for (uint32_t base : {0u, 0x4A000000u, 0x4A130000u}) checkPages(info, bytes, patches, base);

        // The straddling lui got the base across the page boundary.
        std::vector<uint8_t> image(info.imageSize);
        ProgramImage::place(info, bytes.data(), patches.data(), image.data(), 0x4A130000u);
        CHECK(get32(image.data() + straddle) == (0x4A130000u | 0x537u));

        // Writable from page 1: only page 0 can be shared; nothing writable but the file ends
        // exactly on a page: every whole page of it.
        info.writableOffset = Page;
        CHECK(ProgramImage::sharedPageCount(info) == 1);
        info.writableOffset = info.imageSize;
        info.fileSize = 3 * Page;
        bytes.resize(info.fileSize);
        patches = {straddle | ProgramImage::PatchUpper};
        info.relocations = 1;
        std::memset(info.patchedPages, 0, sizeof(info.patchedPages));
        info.patchedPages[0] = (1u << 1) | (1u << 2);
        CHECK(ProgramImage::isSharedPage(info, 0));
        CHECK_FALSE(ProgramImage::isSharedPage(info, 3)); // the bss
        checkPages(info, bytes, patches, 0x4A000000u);

        // An image smaller than a page: nothing shared.
        ProgramImage::Info small;
        small.fileSize = 1000;
        small.imageSize = 2000;
        small.writableOffset = 2000;
        CHECK(ProgramImage::sharedPageCount(small) == 0);
}

TEST_CASE("Program format: real programs' shared pages are the same at every base") {
        std::vector<std::string> paths;
        for (const auto &dir : std::filesystem::directory_iterator(BMDS_COMPAT_DIR))
                if (dir.is_directory())
                        for (const auto &entry : std::filesystem::directory_iterator(dir.path()))
                                if (entry.path().extension() == ".elf") paths.push_back(entry.path().string());
#ifdef BMDS_SDK_HELLO
        paths.push_back(BMDS_SDK_HELLO);
#endif
        REQUIRE(paths.size() >= 3);
        for (const std::string &path : paths) {
                CAPTURE(path);
                std::vector<uint8_t> file;
                FILE                *f = std::fopen(path.c_str(), "rb");
                REQUIRE(f != nullptr);
                uint8_t buf[4096];
                size_t  n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
                std::fclose(f);
                const Checked c = check(file);
                REQUIRE(c.err == Error::Ok);
                CHECK(c.info.writableOffset > 0);
                CHECK(c.info.writableOffset <= c.info.imageSize);
                std::vector<uint8_t>             bytes(c.info.fileSize);
                std::vector<ProgramImage::Patch> patches(c.info.relocations);
                ProgramImage::MemorySource       source(bmds::ByteSpan(file.data(), file.size()));
                ProgramImage::Problem            problem;
                REQUIRE(ProgramImage::read(source, uint32_t(file.size()), c.info, bytes.data(), patches.data(),
                                           problem) == Error::Ok);
                // No patch lands in a shared page.
                for (const ProgramImage::Patch patch : patches) {
                        const uint32_t at = patch & ~ProgramImage::PatchUpper;
                        CHECK_FALSE(ProgramImage::isSharedPage(c.info, at / Page));
                        CHECK_FALSE(ProgramImage::isSharedPage(c.info, (at + 3) / Page));
                }
                for (uint32_t base : {0x4A000000u, 0x4A130000u}) checkPages(c.info, bytes, patches, base);
        }
}
