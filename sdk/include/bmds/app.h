/**
 * @file      app.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An application's folder and its data (devplan/apps.md §3, §6): for a program installed as
 * `NAME.app` (its program, `app.desktop` and data in one folder), the loader sets
 * `BMDS_APP_DIR` to the folder and `BMDS_APP_ID` to the application's id; these read them, and
 * make the folder in the user's home where the application keeps what it writes (the folder
 * itself may be replaced by an update at any time, so it is never written).
 *
 * Library functions (the SDK's libbmds.a, on the C library): no system call. Not thread-safe
 * against `setenv()`, as `getenv()` isn't.
 */

#ifndef BMDS_APP_H
#define BMDS_APP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The environment variable naming the program's application folder. */
#define BMDS_APP_DIR_VARIABLE "BMDS_APP_DIR"

/** @brief The environment variable naming the program's application id. */
#define BMDS_APP_ID_VARIABLE "BMDS_APP_ID"

/** @brief Where an application's data is, below `$HOME`: then its id. */
#define BMDS_APP_DATA_PATH ".local/share"

/**
 * @brief Returns the program's application folder (`/sd0/p0/apps/blocks.app`), or NULL if the
 *        program isn't in one.
 */
const char *bmds_app_dir(void);

/** @brief Returns the program's application id (`blocks`), or NULL if it isn't an application. */
const char *bmds_app_id(void);

/**
 * @brief Writes the application's data folder, `$HOME/.local/share/ID` (`HOME` unset:
 *        `/home`), into @p buf (@p size bytes, NUL included), making it and the folders above
 *        it if they aren't there.
 * @return 0, or -1 with `errno`: `ENOENT` (not an application), `ERANGE` (@p buf too small),
 *         or what `mkdir()` failed with.
 */
int bmds_app_data_dir(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* BMDS_APP_H */
