/**
 * @file      renamechecks.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Renames onto a taken name, the same on every volume (FAT, littlefs, tmpfs):
 * `Vfs::rename()`'s `Replace` and `NoReplace`.
 */

#pragma once

#include <doctest.h>

#include <string>
#include <bmds/vfs.h>

namespace RenameChecks {

        inline const bmds::Vfs::Context Root{"/", nullptr};

        inline bmds::StringView view(const std::string &s) {
                return bmds::StringView(s.data(), s.size());
        }

        inline void write(const std::string &path, const std::string &text) {
                namespace Vfs = bmds::Vfs;
                auto [h, err] = Vfs::open(view(path), Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                REQUIRE_MESSAGE(err.isOk(), path, ": ", err.name());
                REQUIRE(h->write(bmds::StringView(text.data(), text.size()), bmds::WaitForever).error().isOk());
        }

        inline std::string read(const std::string &path) {
                namespace Vfs = bmds::Vfs;
                auto [h, err] = Vfs::open(view(path), Vfs::Read, Root);
                if (err.isError()) return std::string("<") + err.name() + ">";
                char buf[64];
                auto [n, rerr] = h->read(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                         bmds::WaitForever);
                return rerr.isOk() ? std::string(buf, n) : std::string();
        }

        inline bmds::Error rename(const std::string &from, const std::string &to,
                                  bmds::Vfs::RenameMode mode = bmds::Vfs::RenameMode::Replace) {
                return bmds::Vfs::rename(view(from), view(to), mode, Root);
        }

        inline bool exists(const std::string &path) {
                return bmds::Vfs::stat(view(path), Root).isOk();
        }

        /**
         * Runs the checks in directory @p dir (absolute, made here, left empty).
         * @p openRemoves: an open file in the way is replaced (tmpfs), else `Busy`.
         */
        inline void run(const std::string &dir, bool openRemoves) {
                namespace Vfs = bmds::Vfs;
                using bmds::Error;
                using Mode = Vfs::RenameMode;
                REQUIRE(Vfs::mkdir(view(dir), Root).isOk());
                const std::string a = dir + "/a", b = dir + "/b";
                write(a, "A");
                write(b, "B");
                CHECK(rename(a, b, Mode::NoReplace) == Error::Exists);
                CHECK(read(b) == "B");
                // A file replaces a file; onto itself, nothing happens.
                CHECK(rename(a, b).isOk());
                CHECK(read(b) == "A");
                CHECK_FALSE(exists(a));
                CHECK(rename(b, b).isOk());
                CHECK(read(b) == "A");
                // A change of case renames (on FAT, the name taken is the file's own).
                write(dir + "/c", "C");
                CHECK(rename(dir + "/c", dir + "/C").isOk());
                CHECK(read(dir + "/C") == "C");
                {
                        auto [d, err] = Vfs::openDirectory(view(dir), Root);
                        REQUIRE(err.isOk());
                        bool upper = false, lower = false;
                        for (;;) {
                                auto [info, nerr] = d.next();
                                if (nerr.isError()) break;
                                upper = upper || std::string(info.name.cStr()) == "C";
                                lower = lower || std::string(info.name.cStr()) == "c";
                        }
                        CHECK(upper);
                        CHECK_FALSE(lower);
                }
                CHECK(Vfs::remove(view(dir + "/C"), Root).isOk());
                // Files and directories don't replace each other.
                const std::string d = dir + "/d", e = dir + "/e", f = dir + "/f", g = dir + "/g";
                REQUIRE(Vfs::mkdir(view(d), Root).isOk());
                CHECK(rename(b, d) == Error::IsDir);
                CHECK(rename(d, b) == Error::NotDir);
                CHECK(read(b) == "A");
                CHECK(exists(d));
                // A directory replaces an empty one, not one with something in it.
                REQUIRE(Vfs::mkdir(view(e), Root).isOk());
                write(d + "/inside", "I");
                CHECK(rename(d, e).isOk());
                CHECK_FALSE(exists(d));
                CHECK(read(e + "/inside") == "I");
                REQUIRE(Vfs::mkdir(view(g), Root).isOk());
                CHECK(rename(g, e) == Error::NotEmpty);
                CHECK(exists(g));
                CHECK(read(e + "/inside") == "I");
                // A file open where the new one goes.
                write(f, "F");
                {
                        auto [h, err] = Vfs::open(view(b), Vfs::Read, Root);
                        REQUIRE(err.isOk());
                        if (openRemoves) {
                                CHECK(rename(f, b).isOk());
                                char c = 0;
                                CHECK(h->read(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(&c), 1),
                                              bmds::WaitForever)
                                              .value() == 1);
                                CHECK(c == 'A');
                                CHECK(read(b) == "F");
                        } else {
                                CHECK(rename(f, b) == Error::Busy);
                                CHECK(read(f) == "F");
                                CHECK(read(b) == "A");
                        }
                }
                // Left empty.
                (void)Vfs::remove(view(b), Root);
                (void)Vfs::remove(view(f), Root);
                CHECK(Vfs::remove(view(e + "/inside"), Root).isOk());
                CHECK(Vfs::rmdir(view(e), Root).isOk());
                CHECK(Vfs::rmdir(view(g), Root).isOk());
                CHECK(Vfs::rmdir(view(dir), Root).isOk());
                return;
        }

} // namespace RenameChecks
