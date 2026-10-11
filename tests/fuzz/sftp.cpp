/**
 * @file      sftp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for our SFTP server's protocol core (programs/sftp-server/sftp.c,
 * devplan/network.md §24): after an INIT, the input is requests, each a 2-byte length (big end
 * first) and that many bytes. The file calls are made up, answering by the path's hash (files,
 * directories, links, errors, long names). Each reply must be whole and fit; nothing may fault,
 * read out of bounds or leak.
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sftp.h"

namespace {

        uint32_t hash(const char *s) {
                uint32_t h = 2166136261u;
                for (; *s != '\0'; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
                return h;
        }

        int  openFiles = 0;
        int  openDirs = 0;
        int  dirLeft[8];
        bool dirOpen[8];

        int fOpen(void *, const char *path, int, uint32_t) {
                if (hash(path) % 7 == 0 || openFiles >= 40) return -ENOENT;
                return openFiles++;
        }
        int fClose(void *, int) {
                --openFiles;
                return 0;
        }
        long fPread(void *, int file, void *buf, uint32_t size, uint64_t offset) {
                if (offset > 5000) return 0;
                if (file % 5 == 4) return -EIO;
                const uint32_t n = size < 777 ? size : 777;
                std::memset(buf, 'd', n);
                return n;
        }
        long fPwrite(void *, int file, const void *, uint32_t size, uint64_t) {
                if (file % 6 == 5) return -ENOSPC;
                return size > 300 ? 300 : size; // short writes: the core goes on
        }
        void fillStat(uint32_t h, sftp_stat_t *st) {
                st->size = h;
                st->mode = (h % 3 == 0 ? 0040755u : h % 3 == 1 ? 0100644u : 0120777u);
                st->mtime = h;
                st->atime = h;
                st->nlink = 1;
                return;
        }
        int fFstat(void *, int file, sftp_stat_t *st) {
                fillStat(static_cast<uint32_t>(file) * 2654435761u, st);
                return 0;
        }
        int fFtruncate(void *, int, uint64_t) {
                return 0;
        }
        int fFsync(void *, int) {
                return 0;
        }
        int fStat(void *, const char *path, sftp_stat_t *st, int) {
                const uint32_t h = hash(path);
                if (h % 5 == 0) return -ENOENT;
                fillStat(h, st);
                return 0;
        }
        int fTruncate(void *, const char *path, uint64_t) {
                return hash(path) % 2 ? 0 : -EACCES;
        }
        int fUtimes(void *, const char *path, uint32_t, uint32_t) {
                return hash(path) % 3 ? 0 : -EROFS;
        }
        int fOpendir(void *, const char *path) {
                if (hash(path) % 4 == 0) return -ENOTDIR;
                for (int i = 0; i < 8; ++i) {
                        if (dirOpen[i]) continue;
                        dirOpen[i] = true;
                        dirLeft[i] = static_cast<int>(hash(path) % 300);
                        ++openDirs;
                        return i;
                }
                return -EMFILE;
        }
        int fReaddir(void *, int dir, sftp_dirent_t *e) {
                if (dir < 0 || dir >= 8 || !dirOpen[dir]) return -EBADF;
                if (dirLeft[dir] <= 0) return 0;
                --dirLeft[dir];
                // Some names as long as names get.
                const size_t n = dirLeft[dir] % 10 == 0 ? sizeof(e->name) - 1 : 1 + dirLeft[dir] % 40;
                std::memset(e->name, 'a' + dirLeft[dir] % 26, n);
                e->name[n] = '\0';
                e->have_st = dirLeft[dir] % 2;
                if (e->have_st) fillStat(static_cast<uint32_t>(dirLeft[dir]), &e->st);
                return 1;
        }
        int fClosedir(void *, int dir) {
                if (dir < 0 || dir >= 8 || !dirOpen[dir]) return -EBADF;
                dirOpen[dir] = false;
                --openDirs;
                return 0;
        }
        int fRemove(void *, const char *path) {
                return hash(path) % 2 ? 0 : -ENOENT;
        }
        int fMkdir(void *, const char *path, uint32_t) {
                return hash(path) % 2 ? 0 : -EEXIST;
        }
        int fRmdir(void *, const char *path) {
                return hash(path) % 2 ? 0 : -ENOTEMPTY;
        }
        int fRename(void *, const char *from, const char *, int) {
                return hash(from) % 2 ? 0 : -EXDEV;
        }
        int fReadlink(void *, const char *path, char *buf, uint32_t size) {
                if (hash(path) % 2) return -EINVAL;
                const uint32_t n = hash(path) % 3 == 0 ? size : 6; // a target as long as fits
                std::memset(buf, 't', n);
                return static_cast<int>(n);
        }
        int fSymlink(void *, const char *, const char *path) {
                return hash(path) % 2 ? 0 : -EEXIST;
        }
        int fGetcwd(void *, char *buf, uint32_t size) {
                std::snprintf(buf, size, "/sd0/p0/home");
                return 0;
        }
        int fStatvfs(void *, const char *path, sftp_statvfs_t *out) {
                if (hash(path) % 3 == 0) return -ENOSYS;
                out->bsize = 32768;
                out->blocks = hash(path);
                return 0;
        }

        const sftp_fs_t Fake = {fOpen,  fClose,    fPread,  fPwrite,   fFstat,   fFtruncate, fFsync,
                                fStat,  fTruncate, fUtimes, fOpendir,  fReaddir, fClosedir,  fRemove,
                                fMkdir, fRmdir,    fRename, fReadlink, fSymlink, fGetcwd,    fStatvfs};

        // Every reply in @p out whole: lengths that add up to what was written.
        void checkReplies(const sftp_out_t &out, size_t from) {
                size_t at = from;
                while (at < out.length) {
                        if (out.length - at < 5) abort();
                        const uint32_t n = uint32_t(out.data[at]) << 24 | uint32_t(out.data[at + 1]) << 16 |
                                           uint32_t(out.data[at + 2]) << 8 | out.data[at + 3];
                        if (n < 1 || out.length - at - 4 < n) abort();
                        at += 4 + n;
                }
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static uint8_t reply[SFTP_MAX_PACKET + 64];
        sftp_server_t  s;
        sftp_init(&s, &Fake, nullptr);
        static const uint8_t Init[] = {1, 0, 0, 0, 3};
        sftp_out_t           out = {reply, sizeof(reply), 0};
        if (sftp_request(&s, Init, sizeof(Init), &out) != 0) abort();
        checkReplies(out, 0);
        while (size >= 2) {
                const size_t n = size_t(data[0]) << 8 | data[1];
                data += 2;
                size -= 2;
                const size_t take = n < size ? n : size;
                out.length = 0;
                if (take > 0 && sftp_request(&s, data, static_cast<uint32_t>(take), &out) < 0) break;
                checkReplies(out, 0);
                data += take;
                size -= take;
        }
        sftp_finish(&s);
        if (openFiles != 0 || openDirs != 0) abort(); // every handle closed
        return 0;
}
