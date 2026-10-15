/**
 * @file      app.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An application's folder and data folder (bmds/app.h, devplan/apps.md §3, §6).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <bmds/app.h>

const char *bmds_app_dir(void) {
        const char *dir = getenv(BMDS_APP_DIR_VARIABLE);
        return dir != NULL && dir[0] != '\0' ? dir : NULL;
}

const char *bmds_app_id(void) {
        const char *id = getenv(BMDS_APP_ID_VARIABLE);
        return id != NULL && id[0] != '\0' ? id : NULL;
}

int bmds_app_data_dir(char *buf, size_t size) {
        const char *id = bmds_app_id();
        if (id == NULL) {
                errno = ENOENT;
                return -1;
        }
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') home = "/home";
        const int n = snprintf(buf, size, "%s/%s/%s", home, BMDS_APP_DATA_PATH, id);
        if (n < 0 || (size_t)n >= size) {
                errno = ERANGE;
                return -1;
        }
        /* Each folder below HOME in turn (HOME itself is there). */
        for (char *p = buf + strlen(home) + 1; *p != '\0'; ++p) {
                if (*p != '/') continue;
                *p = '\0';
                const int made = mkdir(buf, 0755);
                *p = '/';
                if (made != 0 && errno != EEXIST) return -1;
        }
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        return 0;
}
