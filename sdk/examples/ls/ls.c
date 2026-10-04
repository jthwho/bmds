/**
 * @file      ls.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ls [-al] [path...]: lists directories (sorted) and files, with portable
 * POSIX calls only (opendir, readdir, stat, localtime), as a program
 * written for any system would. Names starting with '.' only with -a.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int long_format, all;

static int by_name(const void *a, const void *b) {
        return strcmp(*(char *const *)a, *(char *const *)b);
}

static void show(const char *dir, const char *name) {
        if (!long_format) {
                printf("%s\n", name);
                return;
        }
        char        path[1100];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (stat(path, &st) < 0) {
                printf("?????????? %10s %16s %s\n", "?", "?", name);
                return;
        }
        char       when[32] = "-";
        struct tm *tm = localtime(&st.st_mtime);
        if (st.st_mtime != 0 && tm != NULL) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", tm);
        printf("%c%s %10lld %16s %s%s\n",
               S_ISDIR(st.st_mode)   ? 'd'
               : S_ISCHR(st.st_mode) ? 'c'
                                     : '-',
               (st.st_mode & 0200) ? "rw-" : "r--", (long long)st.st_size, when, name, S_ISDIR(st.st_mode) ? "/" : "");
        return;
}

static int list(const char *path, int heading) {
        struct stat st;
        if (stat(path, &st) < 0) {
                fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
                return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
                show(".", path);
                return 0;
        }
        DIR *dir = opendir(path);
        if (dir == NULL) {
                fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
                return 1;
        }
        char         **names = NULL;
        size_t         count = 0, room = 0;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
                if (e->d_name[0] == '.' && !all) continue;
                if (count == room) {
                        room = room ? room * 2 : 64;
                        char **more = realloc(names, room * sizeof(*names));
                        if (more == NULL) break;
                        names = more;
                }
                names[count] = strdup(e->d_name);
                if (names[count] != NULL) ++count;
        }
        closedir(dir);
        qsort(names, count, sizeof(*names), by_name);
        if (heading) printf("%s:\n", path);
        for (size_t i = 0; i < count; ++i) {
                show(path, names[i]);
                free(names[i]);
        }
        free(names);
        return 0;
}

int main(int argc, char **argv) {
        int first = 1;
        while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
                for (const char *o = argv[first] + 1; *o != '\0'; ++o) {
                        if (*o == 'l') {
                                long_format = 1;
                        } else if (*o == 'a') {
                                all = 1;
                        } else {
                                fprintf(stderr, "usage: ls [-al] [path...]\n");
                                return 2;
                        }
                }
                ++first;
        }
        if (first == argc) return list(".", 0);
        int status = 0;
        for (int i = first; i < argc; ++i) {
                if (i > first) printf("\n");
                status |= list(argv[i], argc - first > 1);
        }
        return status;
}
