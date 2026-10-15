/**
 * @file      appinfo.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An application's folder, id and data folder (bmds/app.h, devplan/apps.md): installed as
 * apps/NAME.app with an app.desktop saying Exec=appinfo, it prints them and counts its runs in a
 * file in its data folder; run from anywhere else, it says it isn't an application.
 *
 *     appinfo
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <bmds/app.h>

int main(void) {
        if (bmds_app_id() == NULL) {
                printf("appinfo: not an application (no %s)\n", BMDS_APP_ID_VARIABLE);
                return 1;
        }
        char data[PATH_MAX];
        if (bmds_app_data_dir(data, sizeof(data)) != 0) {
                fprintf(stderr, "appinfo: data folder: %s\n", strerror(errno));
                return 1;
        }
        printf("folder: %s\nid:     %s\ndata:   %s\n", bmds_app_dir() != NULL ? bmds_app_dir() : "(none)",
               bmds_app_id(), data);
        /* What it writes goes in its data folder, never its own. */
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/runs", data) >= (int)sizeof(path)) return 1;
        unsigned runs = 0;
        FILE    *f = fopen(path, "r");
        if (f != NULL) {
                if (fscanf(f, "%u", &runs) != 1) runs = 0;
                fclose(f);
        }
        f = fopen(path, "w");
        if (f == NULL) {
                fprintf(stderr, "appinfo: %s: %s\n", path, strerror(errno));
                return 1;
        }
        fprintf(f, "%u\n", ++runs);
        fclose(f);
        printf("runs:   %u\n", runs);
        return 0;
}
