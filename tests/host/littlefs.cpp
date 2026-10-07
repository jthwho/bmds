/**
 * @file      littlefs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * littlefs (thirdparty/littlefs) on a simulated NOR flash cut off at every
 * erase and program in turn: after each cut, the filesystem mounts and every
 * file holds either its last closed contents or the ones being written
 * (devplan/storage.md §13c).
 */

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include <lfs.h>

namespace {

        constexpr lfs_size_t BlockBytes = 4096;
        constexpr lfs_size_t Blocks = 64; // small, so blocks are reused and compacted often
        constexpr lfs_size_t CacheBytes = 256;
        constexpr lfs_size_t LookaheadBytes = 16;

        // A NOR flash: an erase sets a block to 0xFF, a program can only clear bits. The cut
        // operation (an erase or a program, counted from 0) is left half done (a program's
        // first bytes, an erase's garbage), and the flash is off after it until restored.
        struct NorFlash {
                        std::vector<uint8_t> bytes = std::vector<uint8_t>(size_t(BlockBytes) * Blocks, 0xff);
                        int64_t              cutAt = -1;
                        int64_t              ops = 0;
                        bool                 off = false;
                        size_t               overwrites = 0; // bytes programmed that weren't erased
                        std::mt19937         rng{1};

                        uint8_t *at(lfs_block_t block, lfs_off_t offset) {
                                return bytes.data() + size_t(block) * BlockBytes + offset;
                        }

                        bool cutNow() {
                                if (ops++ != cutAt) return false;
                                off = true;
                                return true;
                        }
        };

        NorFlash &flashOf(const lfs_config *c) {
                return *static_cast<NorFlash *>(c->context);
        }

        int flashRead(const lfs_config *c, lfs_block_t block, lfs_off_t offset, void *buffer, lfs_size_t size) {
                NorFlash &f = flashOf(c);
                if (f.off) return LFS_ERR_IO;
                std::memcpy(buffer, f.at(block, offset), size);
                return 0;
        }

        int flashProg(const lfs_config *c, lfs_block_t block, lfs_off_t offset, const void *buffer, lfs_size_t size) {
                NorFlash &f = flashOf(c);
                if (f.off) return LFS_ERR_IO;
                const auto *in = static_cast<const uint8_t *>(buffer);
                uint8_t    *out = f.at(block, offset);
                lfs_size_t  n = size;
                const bool  cut = f.cutNow();
                if (cut) n = f.rng() % size;
                for (lfs_size_t i = 0; i < n; ++i) {
                        if (out[i] != 0xff) ++f.overwrites;
                        out[i] &= in[i];
                }
                if (!cut) return 0;
                out[n] &= static_cast<uint8_t>(in[n] | f.rng()); // the byte being programmed: some bits
                return LFS_ERR_IO;
        }

        int flashErase(const lfs_config *c, lfs_block_t block) {
                NorFlash &f = flashOf(c);
                if (f.off) return LFS_ERR_IO;
                uint8_t *out = f.at(block, 0);
                if (!f.cutNow()) {
                        std::memset(out, 0xff, BlockBytes);
                        return 0;
                }
                for (lfs_size_t i = 0; i < BlockBytes; ++i) out[i] |= static_cast<uint8_t>(f.rng());
                return LFS_ERR_IO;
        }

        int flashSync(const lfs_config *c) {
                return flashOf(c).off ? LFS_ERR_IO : 0;
        }

        // littlefs's configuration and buffers (no heap: LFS_NO_MALLOC, as on the board).
        struct Fs {
                        NorFlash   flash;
                        uint8_t    readBuffer[CacheBytes];
                        uint8_t    progBuffer[CacheBytes];
                        uint8_t    lookahead[LookaheadBytes];
                        uint8_t    fileBuffer[CacheBytes];
                        lfs_config config{};
                        lfs_t      lfs{};

                        Fs() {
                                config.context = &flash;
                                config.read = flashRead;
                                config.prog = flashProg;
                                config.erase = flashErase;
                                config.sync = flashSync;
                                config.read_size = 16;
                                config.prog_size = 16;
                                config.block_size = BlockBytes;
                                config.block_count = Blocks;
                                config.block_cycles = 100; // blocks moved often: wear levelling exercised
                                config.cache_size = CacheBytes;
                                config.lookahead_size = LookaheadBytes;
                                config.read_buffer = readBuffer;
                                config.prog_buffer = progBuffer;
                                config.lookahead_buffer = lookahead;
                        }

                        int open(lfs_file_t &file, const char *path, int flags) {
                                lfs_file_config fc{};
                                fc.buffer = fileBuffer;
                                return lfs_file_opencfg(&lfs, &file, path, flags, &fc);
                        }
        };

        // The files: each version's contents follow from (file, version): a header, then bytes
        // from a generator seeded with both.
        constexpr int         Files = 8;
        constexpr const char *Names[Files] = {"f0", "f1", "f2", "f3", "f4", "f5", "d/g0", "d/g1"};
        constexpr int         Absent = -1;
        constexpr int         None = -2;
        constexpr int         Empty = -3; // made, nothing written yet
        constexpr size_t      HeaderBytes = 12;

        std::vector<uint8_t> contents(int file, int version, size_t size) {
                std::vector<uint8_t> out(size);
                const uint32_t       header[3] = {uint32_t(file), uint32_t(version), uint32_t(size)};
                std::memcpy(out.data(), header, HeaderBytes);
                std::mt19937 gen(uint32_t(file * 1000003 + version));
                for (size_t i = HeaderBytes; i < size; ++i) out[i] = static_cast<uint8_t>(gen());
                return out;
        }

        // What each file must hold: its last finished version (or Absent), or the one being
        // made when the flash was cut (None: nothing was). A file opened with O_CREAT exists
        // from then on, empty until its data is committed at close (as POSIX's creat()), so a
        // file being made by writing it may also be found Empty.
        struct Model {
                        int  committed[Files];
                        int  pending[Files];
                        bool mayBeEmpty[Files];
                        int  nextVersion = 0;

                        Model() {
                                for (int i = 0; i < Files; ++i) {
                                        committed[i] = Absent;
                                        pending[i] = None;
                                        mayBeEmpty[i] = false;
                                }
                        }
        };

        int writeFile(Fs &fs, const char *path, const std::vector<uint8_t> &data) {
                lfs_file_t file;
                int        err = fs.open(file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
                if (err < 0) return err;
                // In pieces, as a program writes.
                for (size_t done = 0; done < data.size() && err >= 0;) {
                        const size_t n = data.size() - done < 700 ? data.size() - done : 700;
                        err = int(lfs_file_write(&fs.lfs, &file, data.data() + done, lfs_size_t(n)));
                        done += n;
                }
                const int cerr = lfs_file_close(&fs.lfs, &file);
                return err < 0 ? err : cerr;
        }

        // One change to one file: rewritten, replaced by renaming a new file over it, or
        // removed. @return littlefs's result (an error once the flash is cut).
        int step(Fs &fs, Model &model, std::mt19937 &rng) {
                const int    f = int(rng() % Files);
                const int    kind = int(rng() % 10);
                const int    v = model.nextVersion++;
                const int    band = int(rng() % 10);
                const size_t size = band < 4   ? HeaderBytes + rng() % 90 // inline in its directory
                                    : band < 8 ? 100 + rng() % 1900       // one block
                                               : 2000 + rng() % 10000;    // a chain of blocks
                int          err;
                if (kind < 6) {
                        model.pending[f] = v;
                        model.mayBeEmpty[f] = model.committed[f] == Absent;
                        err = writeFile(fs, Names[f], contents(f, v, size));
                        if (err >= 0) model.committed[f] = v;
                } else if (kind < 9) {
                        err = writeFile(fs, "tmp", contents(f, v, size));
                        if (err < 0) return err;
                        model.pending[f] = v;
                        err = lfs_rename(&fs.lfs, "tmp", Names[f]);
                        if (err >= 0) model.committed[f] = v;
                } else {
                        model.pending[f] = Absent;
                        err = lfs_remove(&fs.lfs, Names[f]);
                        if (err == LFS_ERR_NOENT) err = 0;
                        if (err >= 0) model.committed[f] = Absent;
                }
                if (err >= 0) {
                        model.pending[f] = None;
                        model.mayBeEmpty[f] = false;
                }
                return err;
        }

        // The version file @p f holds, Absent, Empty, or None if its contents aren't any
        // version's.
        int versionOf(Fs &fs, int f) {
                lfs_file_t file;
                const int  err = fs.open(file, Names[f], LFS_O_RDONLY);
                if (err == LFS_ERR_NOENT) return Absent;
                if (err < 0) return None;
                std::vector<uint8_t> got(size_t(lfs_file_size(&fs.lfs, &file)));
                const lfs_ssize_t    n = lfs_file_read(&fs.lfs, &file, got.data(), lfs_size_t(got.size()));
                lfs_file_close(&fs.lfs, &file);
                if (n != lfs_ssize_t(got.size())) return None;
                if (got.empty()) return Empty;
                if (got.size() < HeaderBytes) return None;
                uint32_t header[3];
                std::memcpy(header, got.data(), HeaderBytes);
                if (header[0] != uint32_t(f) || header[2] != got.size()) return None;
                return got == contents(f, int(header[1]), got.size()) ? int(header[1]) : None;
        }

        // How the files being changed at a cut came out (both must be seen, or the cuts missed).
        struct Outcomes {
                        int kept = 0;   // the old version
                        int landed = 0; // the new one
        };

        // Every file holds what the model allows; the allowed version becomes the committed one.
        bool check(Fs &fs, Model &model, Outcomes &outcomes) {
                bool ok = true;
                for (int f = 0; f < Files; ++f) {
                        const int  v = versionOf(fs, f);
                        const bool allowed = v == model.committed[f] ||
                                             (model.pending[f] != None && v == model.pending[f]) ||
                                             (model.mayBeEmpty[f] && v == Empty);
                        if (!allowed) {
                                ok = false;
                                continue;
                        }
                        if (model.pending[f] != None && model.pending[f] != model.committed[f])
                                ++(v == model.pending[f] ? outcomes.landed : outcomes.kept);
                        // Empty: as a file of no version from here on (only rewritten or removed).
                        model.committed[f] = v;
                        model.pending[f] = None;
                        model.mayBeEmpty[f] = false;
                }
                return ok;
        }

        constexpr int SetupSteps = 40;
        constexpr int CutSteps = 30;
        constexpr int AfterSteps = 4;

} // namespace

TEST_CASE("littlefs keeps every file whole when the flash is cut at any erase or program") {
        Fs fs;
        REQUIRE(lfs_format(&fs.lfs, &fs.config) == 0);
        REQUIRE(lfs_mount(&fs.lfs, &fs.config) == 0);
        REQUIRE(lfs_mkdir(&fs.lfs, "d") == 0);
        Model        start;
        std::mt19937 setupRng(7);
        for (int i = 0; i < SetupSteps; ++i) REQUIRE(step(fs, start, setupRng) >= 0);
        REQUIRE(lfs_unmount(&fs.lfs) == 0);
        const std::vector<uint8_t> image = fs.flash.bytes;

        // The flash operations the cut steps make without a cut.
        int64_t total = 0;
        {
                Model        model = start;
                std::mt19937 rng(11);
                fs.flash.ops = 0;
                REQUIRE(lfs_mount(&fs.lfs, &fs.config) == 0);
                for (int i = 0; i < CutSteps; ++i) REQUIRE(step(fs, model, rng) >= 0);
                REQUIRE(lfs_unmount(&fs.lfs) == 0);
                total = fs.flash.ops;
        }
        REQUIRE(total > 100);

        int      failures = 0;
        Outcomes outcomes, unused;
        for (int64_t cut = 0; cut < total; ++cut) {
                fs.flash.bytes = image;
                fs.flash.ops = 0;
                fs.flash.cutAt = cut;
                fs.flash.off = false;
                fs.flash.rng.seed(uint32_t(cut));
                Model        model = start;
                std::mt19937 rng(11);
                REQUIRE(lfs_mount(&fs.lfs, &fs.config) == 0);
                for (int i = 0; i < CutSteps && step(fs, model, rng) >= 0; ++i) {}
                REQUIRE(fs.flash.off);
                // Power back: mount, check, carry on, check again after a remount.
                fs.flash.off = false;
                fs.flash.cutAt = -1;
                bool ok = lfs_mount(&fs.lfs, &fs.config) == 0;
                ok = ok && check(fs, model, outcomes);
                for (int i = 0; ok && i < AfterSteps; ++i) ok = step(fs, model, rng) >= 0;
                ok = ok && lfs_unmount(&fs.lfs) == 0;
                ok = ok && lfs_mount(&fs.lfs, &fs.config) == 0 && check(fs, model, unused);
                ok = ok && lfs_unmount(&fs.lfs) == 0;
                if (!ok && ++failures <= 5) MESSAGE("cut at flash operation " << cut << " of " << total);
        }
        CHECK(failures == 0);
        CHECK(outcomes.kept > 0);
        CHECK(outcomes.landed > 0);
        // littlefs never programs over bytes it hasn't erased (a NOR flash would mix them).
        CHECK(fs.flash.overwrites == 0);
}
