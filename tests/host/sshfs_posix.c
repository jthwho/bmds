/**
 * @file      sshfs_posix.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Our sftp-server's calls over POSIX (programs/sftp-server/main.c), for the sshfs test
 * (sshfs.cpp): the host's files behind the server's protocol core, as on the board.
 */

#define main sftp_server_main
#include "../../programs/sftp-server/main.c"
#undef main

const sftp_fs_t *sshfs_test_posix_fs(void);

const sftp_fs_t *sshfs_test_posix_fs(void) {
        return &posix_fs;
}
