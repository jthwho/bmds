/**
 * @file      filetypes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a file is, and what opens it (devplan/filetypes.md).
 */

#include <doctest.h>

#include <string>
#include <bmds/desktopentry.h>
#include <bmds/fileformat.h>
#include <bmds/mimeapps.h>

using bmds::Span;
using bmds::StringView;
namespace FileFormat = bmds::FileFormat;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        std::string content(const std::string &bytes, bool whole = true) {
                return str(FileFormat::byContent(
                        Span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()), whole));
        }

} // namespace

TEST_CASE("FileFormat: by name") {
        CHECK(str(FileFormat::byName("notes.txt")) == "text/plain");
        CHECK(str(FileFormat::byName("/sd0/p0/Shot.PNG")) == "image/png");
        CHECK(str(FileFormat::byName("main.c")) == "text/x-csrc");
        CHECK(str(FileFormat::byName("Song.MID")) == "audio/midi");
        CHECK(str(FileFormat::byName("song.midi")) == "audio/midi");
        CHECK(str(FileFormat::byName("Album.M3U")) == "audio/x-mpegurl"); // a playlist, text-like
        CHECK(str(FileFormat::byName("album.m3u8")) == "audio/x-mpegurl");
        CHECK(FileFormat::isText("audio/x-mpegurl"));
        CHECK(str(FileFormat::byName("a.tar.gz")) == "application/gzip");
        CHECK(str(FileFormat::byName("terminal.desktop")) == "application/x-desktop");
        // Whole names, any case; a directory's dot isn't the name's.
        CHECK(str(FileFormat::byName("src/Makefile")) == "text/x-makefile");
        CHECK(str(FileFormat::byName("CMakeLists.txt")) == "text/x-cmake");
        CHECK(str(FileFormat::byName("a.d/README")) == "text/plain");
        CHECK(FileFormat::byName("a.tar/notes").isEmpty());
        // A dotfile's dot isn't an extension's; unknown and empty extensions are nothing.
        CHECK(FileFormat::byName(".profile").isEmpty());
        CHECK(str(FileFormat::byName(".hidden.sh")) == "application/x-shellscript");
        CHECK(FileFormat::byName("x.unknown").isEmpty());
        CHECK(FileFormat::byName("trailing.").isEmpty());
        CHECK(FileFormat::byName("").isEmpty());
}

TEST_CASE("FileFormat: by content") {
        CHECK(content(std::string("\x7f"
                                  "ELF\x01\x01\x01",
                                  7)) == "application/x-executable");
        CHECK(content(std::string("\x89PNG\r\n\x1a\n....", 12)) == "image/png");
        CHECK(content("\xff\xd8\xff\xe0") == "image/jpeg");
        CHECK(content("GIF89a..") == "image/gif");
        CHECK(content("%PDF-1.4\n") == "application/pdf");
        CHECK(content(std::string("PK\x03\x04", 4)) == "application/zip");
        CHECK(content(std::string("\x1f\x8b\x08\x00", 4)) == "application/gzip");
        CHECK(content(std::string("\xfd"
                                  "7zXZ\0\0",
                                  7)) == "application/x-xz");
        CHECK(content("BZh91AY") == "application/x-bzip2");
        CHECK(content("OggS....") == "audio/ogg");
        CHECK(content("fLaC....") == "audio/flac");
        CHECK(content("MThd\0\0\0\x06") == "audio/midi");
        CHECK(content("ID3\x03") == "audio/mpeg");
        {
                // MPEG audio without a tag: MPEG-2 layer III, 32 kb/s, 22,050 Hz, mono: 104-byte frames.
                std::string mp3(300, '\x55');
                mp3.replace(0, 4, "\xff\xf3\x40\xc0");
                mp3.replace(104, 4, "\xff\xf3\x40\xc0");
                CHECK(content(mp3) == "audio/mpeg");
                mp3[105] = '\xfb'; // another version where the next frame should be
                CHECK(content(mp3) == "application/octet-stream");
                CHECK(content(mp3.substr(0, 60)) == "audio/mpeg"); // the next header past the head
                CHECK(content(std::string("\xff\xf3\xf0\xc0", 4)) == "application/octet-stream"); // bitrate 15
        }
        CHECK(content(std::string("RIFF\x24\0\0\0WAVEfmt ", 16)) == "audio/x-wav");
        CHECK(content(std::string("RIFF\x24\0\0\0"
                                  "AVI LIST",
                                  16)) == "video/x-msvideo");
        CHECK(content(std::string("BM\x36\0\0\0\0\0\0\0\x36\0", 12)) == "image/bmp");
        CHECK(content("BMW is a car\n") == "text/plain"); // "BM" alone isn't a bitmap
        std::string tar(512, '\0');
        tar.replace(257, 5, "ustar");
        CHECK(content(tar) == "application/x-tar");
        CHECK(content("[Desktop Entry]\nName=x\n") == "application/x-desktop");
        CHECK(content("<?xml version=\"1.0\"?>") == "application/xml");
        CHECK(content("<!DOCTYPE HTML>\n<html>") == "text/html");
        // Scripts by their interpreter.
        CHECK(content("#!/bin/sh\necho hi\n") == "application/x-shellscript");
        CHECK(content("#!/usr/bin/env python3\n") == "text/x-python");
        // Text: UTF-8 and the usual controls; a NUL, another control or a bad byte is binary.
        CHECK(content("hello\tworld\r\n\x1b[1m\xc3\xa9\n") == "text/plain");
        CHECK(content(std::string("a\0b", 3)) == "application/octet-stream");
        CHECK(content("a\x01"
                      "b") == "application/octet-stream");
        CHECK(content("a\x7f") == "application/octet-stream");
        CHECK(content("caf\xe9 au lait") == "application/octet-stream"); // Latin-1
        CHECK(content("\xef\xbf\xbd written out") == "text/plain");
        // A character cut by the buffer's end is text, unless the buffer is the whole file.
        CHECK(content("caf\xc3", false) == "text/plain");
        CHECK(content("caf\xe2\x82", false) == "text/plain");
        CHECK(content("caf\xc3", true) == "application/octet-stream");
        CHECK(content("caf\xc3x", false) == "application/octet-stream");
        // Empty: the whole file, or nothing read.
        CHECK(content("", true) == "application/x-zerosize");
        CHECK(content("", false) == "application/octet-stream");
}

TEST_CASE("FileFormat: text-like types and icons") {
        CHECK(FileFormat::isText("text/plain"));
        CHECK(FileFormat::isText("text/x-csrc"));
        CHECK(FileFormat::isText("application/json"));
        CHECK(FileFormat::isText("application/x-shellscript"));
        CHECK(FileFormat::isText("application/x-zerosize"));
        CHECK_FALSE(FileFormat::isText("image/png"));
        CHECK_FALSE(FileFormat::isText("application/octet-stream"));
        CHECK_FALSE(FileFormat::isText("application/x-executable"));
        char name[32];
        CHECK(str(FileFormat::iconName("text/plain", Span<char>(name, sizeof(name)))) == "text-plain");
        CHECK(std::string(name) == "text-plain");
        char small[4];
        CHECK(FileFormat::iconName("text/plain", Span<char>(small, sizeof(small))).isEmpty());
        CHECK(str(FileFormat::genericIcon("text/x-csrc")) == "text-x-generic");
        CHECK(str(FileFormat::genericIcon("application/json")) == "text-x-generic");
        CHECK(str(FileFormat::genericIcon("image/png")) == "image-x-generic");
        CHECK(str(FileFormat::genericIcon("audio/ogg")) == "audio-x-generic");
        CHECK(str(FileFormat::genericIcon("video/mp4")) == "video-x-generic");
        CHECK(str(FileFormat::genericIcon("application/gzip")) == "package-x-generic");
        CHECK(str(FileFormat::genericIcon("application/x-executable")) == "application-x-executable");
        CHECK(str(FileFormat::genericIcon("inode/directory")) == "folder");
        CHECK(str(FileFormat::genericIcon("application/pdf")) == "unknown");
}

TEST_CASE("MimeApps: a type's applications, then its media's") {
        constexpr StringView Text = "# Which applications open files\n"
                                    "[Added Associations]\n"
                                    "text/plain=other.desktop\n"
                                    "[Default Applications]\n"
                                    "text/*=vi.desktop\n"
                                    "Text/Plain=edit.desktop;vi.desktop;\n"
                                    "image/png=view.desktop\n"
                                    "image/*x=bad.desktop\n";
        StringView           ids[4];
        size_t               n = bmds::MimeApps::lookup(Text, "text/plain", Span<StringView>(ids, 4));
        REQUIRE(n == 3);
        CHECK(str(ids[0]) == "edit.desktop");
        CHECK(str(ids[1]) == "vi.desktop");
        CHECK(str(ids[2]) == "vi.desktop");
        n = bmds::MimeApps::lookup(Text, "text/x-csrc", Span<StringView>(ids, 4));
        REQUIRE(n == 1);
        CHECK(str(ids[0]) == "vi.desktop");
        n = bmds::MimeApps::lookup(Text, "image/png", Span<StringView>(ids, 4));
        REQUIRE(n == 1);
        CHECK(str(ids[0]) == "view.desktop");
        CHECK(bmds::MimeApps::lookup(Text, "image/gif", Span<StringView>(ids, 4)) == 0);
        CHECK(bmds::MimeApps::lookup(Text, "audio/ogg", Span<StringView>(ids, 4)) == 0);
        // Room for one: the first.
        n = bmds::MimeApps::lookup(Text, "text/plain", Span<StringView>(ids, 1));
        REQUIRE(n == 1);
        CHECK(str(ids[0]) == "edit.desktop");
}

TEST_CASE("DesktopEntry: Exec with a file") {
        char       buffer[128];
        StringView args[8];
        const auto split = [&](const char *exec, const char *file) {
                auto [n, err] =
                        bmds::DesktopEntry::splitExec(exec, buffer, sizeof(buffer), Span<StringView>(args, 8), file);
                std::string all;
                if (err.isError()) return std::string("error");
                for (size_t i = 0; i < n; ++i) all += (i > 0 ? "|" : "") + str(args[i]);
                return all;
        };
        CHECK(split("vi %f", "/boot/a b.txt") == "vi|/boot/a b.txt");
        CHECK(split("vi %F", "x") == "vi|x");
        CHECK(split("view %u --", "x") == "view|x|--");
        CHECK(split("view %U", "x") == "view|x");
        CHECK(split("view --file=%f", "x") == "view|--file=x");
        // No field code for it: last. Other codes still dropped; %% a %.
        CHECK(split("vi", "/boot/x") == "vi|/boot/x");
        CHECK(split("vi %i %c 100%%", "x") == "vi|100%|x");
        // Without a file, as before.
        CHECK(split("vi %f", "") == "vi");
        // Too long for the buffer.
        std::string big(200, 'x');
        CHECK(split("vi %f", big.c_str()) == "error");
}
