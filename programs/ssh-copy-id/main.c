/**
 * @file      main.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ssh-copy-id: this machine's public key put in a server's ~/.ssh/authorized_keys, so dbclient,
 * scp and sshfs log in there without a password (devplan/network.md §24). OpenSSH's is a shell
 * script around its own key files; this one takes Dropbear's (~/.ssh/id_dropbear, made with
 * dropbearkey if there is none yet) and runs the same kind of command on the server, through
 * dbclient (its password and host key prompts on the terminal).
 */

#define _GNU_SOURCE /* pipe2 */

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_ARGS 32
#define KEY_MAX 4096

/* What the server's command prints, so this one can tell which happened. */
#define MARK_ADDED "ssh-copy-id: added"
#define MARK_THERE "ssh-copy-id: there"

static void usage(void) {
        fprintf(stderr, "usage: ssh-copy-id [-n] [-i KEY] [-p PORT] [-o OPTION] [user@]host\n"
                        "  -i KEY     the key: a public one (.pub), or Dropbear's private one\n"
                        "             (default ~/.ssh/id_dropbear, made if there is none)\n"
                        "  -p PORT    the server's port\n"
                        "  -o OPTION  a dbclient option\n"
                        "  -n         only show the key that would be added\n");
        return;
}

/* Runs @p argv with its stdout into @p out (@p size bytes, NUL-terminated): its exit status, or -1. */
static int run(char *const argv[], char *out, size_t size, const char *input) {
        int in[2] = {-1, -1}, from[2];
        if (pipe2(from, O_CLOEXEC) != 0) return -1;
        if (input != NULL && pipe2(in, O_CLOEXEC) != 0) {
                close(from[0]);
                close(from[1]);
                return -1;
        }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        if (input != NULL) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, from[1], 1);
        pid_t     pid = -1;
        const int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(from[1]);
        if (input != NULL) close(in[0]);
        if (r != 0) {
                fprintf(stderr, "ssh-copy-id: %s: %s\n", argv[0], strerror(r));
                close(from[0]);
                if (input != NULL) close(in[1]);
                return -1;
        }
        if (input != NULL) {
                if (input[0] != 0 && write(in[1], input, strlen(input)) < 0) perror("ssh-copy-id: write");
                close(in[1]);
        }
        size_t n = 0;
        for (;;) {
                const ssize_t got = read(from[0], out + n, size - 1 - n);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0) break;
                n += (size_t)got;
                if (n == size - 1) {
                        char discard[256]; /* the rest, unread: not kept */
                        while (read(from[0], discard, sizeof(discard)) > 0) {}
                        break;
                }
        }
        out[n] = 0;
        close(from[0]);
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The line of @p text that is a public key ("ssh-ed25519 AAAA... comment"), into @p key. */
static int key_line(const char *text, char *key, size_t size) {
        for (const char *line = text; line != NULL && *line != 0;) {
                const char *end = strchr(line, '\n');
                size_t      n = end != NULL ? (size_t)(end - line) : strlen(line);
                while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) --n;
                if ((strncmp(line, "ssh-", 4) == 0 || strncmp(line, "ecdsa-", 6) == 0) && n < size) {
                        memcpy(key, line, n);
                        key[n] = 0;
                        return 0;
                }
                line = end != NULL ? end + 1 : NULL;
        }
        return -1;
}

/* A key goes into the server's command between double quotes: only what keys are made of. */
static int plain(const char *key) {
        for (const char *p = key; *p != 0; ++p) {
                const char c = *p;
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      strchr("+/=@._-: ", c) != NULL))
                        return 0;
        }
        return 1;
}

/* The public key of @p path (a .pub, or Dropbear's private key), made first if @p make and missing. */
static int public_key(const char *path, int make, char *key, size_t size) {
        static char  out[KEY_MAX];
        const size_t n = strlen(path);
        if (n > 4 && strcmp(path + n - 4, ".pub") == 0) {
                FILE *f = fopen(path, "r");
                if (f == NULL) {
                        fprintf(stderr, "ssh-copy-id: %s: %s\n", path, strerror(errno));
                        return -1;
                }
                const size_t got = fread(out, 1, sizeof(out) - 1, f);
                fclose(f);
                out[got] = 0;
        } else {
                struct stat st;
                if (stat(path, &st) != 0) {
                        if (!make) {
                                fprintf(stderr, "ssh-copy-id: %s: %s\n", path, strerror(errno));
                                return -1;
                        }
                        /* None yet: one made, as ssh-keygen would be asked to (~/.ssh first). */
                        char dir[1024];
                        snprintf(dir, sizeof(dir), "%s", path);
                        char *slash = strrchr(dir, '/');
                        if (slash != NULL && slash != dir) {
                                *slash = 0;
                                if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
                                        fprintf(stderr, "ssh-copy-id: %s: %s\n", dir, strerror(errno));
                                        return -1;
                                }
                        }
                        fprintf(stderr, "ssh-copy-id: no key yet: making %s\n", path);
                        char *const gen[] = {"dropbearkey", "-t", "ed25519", "-f", (char *)path, NULL};
                        if (run(gen, out, sizeof(out), NULL) != 0) {
                                fprintf(stderr, "ssh-copy-id: dropbearkey couldn't make %s\n", path);
                                return -1;
                        }
                }
                char *const show[] = {"dropbearkey", "-y", "-f", (char *)path, NULL};
                if (run(show, out, sizeof(out), NULL) != 0) {
                        fprintf(stderr, "ssh-copy-id: %s: not a key dropbearkey reads\n", path);
                        return -1;
                }
        }
        if (key_line(out, key, size) != 0) {
                fprintf(stderr, "ssh-copy-id: %s: no public key in it\n", path);
                return -1;
        }
        return 0;
}

int main(int argc, char **argv) {
        int   dry = 0, opt, nargs = 0;
        char *key_path = NULL;
        char *client[MAX_ARGS + 8];
        client[nargs++] = "dbclient";
        while ((opt = getopt(argc, argv, "ni:p:o:")) != -1) {
                if (nargs >= MAX_ARGS) {
                        fprintf(stderr, "ssh-copy-id: too many options\n");
                        return 2;
                }
                switch (opt) {
                        case 'n': dry = 1; break;
                        case 'i': key_path = optarg; break;
                        case 'p':
                        case 'o':
                                client[nargs++] = opt == 'p' ? "-p" : "-o";
                                client[nargs++] = optarg;
                                break;
                        default: usage(); return 2;
                }
        }
        if (argc - optind != 1) {
                usage();
                return 2;
        }
        char        def[1024];
        const char *home = getenv("HOME");
        if (key_path == NULL) {
                snprintf(def, sizeof(def), "%s/.ssh/id_dropbear", home != NULL && home[0] != 0 ? home : "/home");
                key_path = def;
        }
        static char key[KEY_MAX];
        if (public_key(key_path, key_path == def, key, sizeof(key)) != 0) return 1;
        if (!plain(key)) {
                fprintf(stderr, "ssh-copy-id: %s: the key has characters a key doesn't\n", key_path);
                return 1;
        }
        if (dry) {
                printf("would add to %s: %s\n", argv[optind], key);
                return 0;
        }
        /* On the server, in sh whatever the login shell: ~/.ssh made private, the key added once
         * (a last line without its newline given one first), saying which. */
        static char command[KEY_MAX * 2 + 1024];
        const int   n = snprintf(command, sizeof(command),
                                 "exec sh -c 'cd || exit 1; umask 077; mkdir -p .ssh || exit 1; "
                                   "f=.ssh/authorized_keys; k=\"%s\"; "
                                   "if [ -f \"$f\" ] && grep -qxF \"$k\" \"$f\"; then echo \"" MARK_THERE "\"; "
                                   "else { [ ! -s \"$f\" ] || [ -z \"$(tail -c1 \"$f\")\" ] || echo; "
                                   "echo \"$k\"; } >> \"$f\" && echo \"" MARK_ADDED "\"; fi'",
                                 key);
        if (n < 0 || (size_t)n >= sizeof(command)) return 1;
        client[nargs++] = "-T";
        client[nargs++] = argv[optind];
        client[nargs++] = command;
        client[nargs] = NULL;
        static char out[1024];
        const int   status = run(client, out, sizeof(out), "");
        if (strstr(out, MARK_THERE) != NULL) {
                printf("The key is already in %s's authorized_keys: nothing added.\n", argv[optind]);
                return 0;
        }
        if (status != 0 || strstr(out, MARK_ADDED) == NULL) {
                fprintf(stderr, "ssh-copy-id: %s: the key wasn't added (see above)\n", argv[optind]);
                return 1;
        }
        printf("Number of key(s) added: 1\n\nNow try logging into the machine, with: ssh %s\n", argv[optind]);
        return 0;
}
