/**
 * @file      main.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * sshfs: a directory on an SSH server mounted here (devplan/netfs.md step 3). Dropbear's client
 * runs the server's `sftp` subsystem on a pair of pipes; this program serves the mount
 * (fs_serve) from it, several requests at once (sshfs.c's workers). Mounted, it goes on in the background
 * (-f: in the foreground) until the mount is unmounted (umount) or the connection ends.
 */

#define _GNU_SOURCE /* setsid */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <bmds/sys.h>

#include "sshfs.h"

extern char **environ;

/* The client's options passed on, and ours. */
#define MAX_ARGS 32

static void usage(void) {
        fprintf(stderr,
                "usage: sshfs [-f] [-p PORT] [-i KEY] [-y] [-o OPTION] [-s COMMAND] [user@]host:[dir] MOUNTPOINT\n"
                "  -f         stay in the foreground\n"
                "  -p PORT    the server's port\n"
                "  -i KEY     an identity (key) file, as dbclient's -i\n"
                "  -y         accept an unknown host key (twice: don't check it), as dbclient's\n"
                "  -o OPTION  a dbclient option\n"
                "  -s COMMAND run COMMAND (sh -c), an SFTP server on its stdin and stdout, not\n"
                "             dbclient: a tunnel of your own, or a directory here (host: none)\n");
        return;
}

/* The pipes to the client. */
typedef struct link {
                int to;   /* its stdin */
                int from; /* its stdout */
} link_t;

static int send_all(void *ctx, const void *data, size_t size) {
        const link_t  *l = ctx;
        const uint8_t *p = data;
        while (size > 0) {
                const ssize_t n = write(l->to, p, size);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return -1;
                p += n;
                size -= (size_t)n;
        }
        return 0;
}

static int recv_all(void *ctx, void *data, size_t size) {
        const link_t *l = ctx;
        uint8_t      *p = data;
        while (size > 0) {
                const ssize_t n = read(l->from, p, size);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return -1;
                p += n;
                size -= (size_t)n;
        }
        return 0;
}

/* Starts dbclient with @p argv on new pipes: its id, or -1. */
static pid_t start_client(char **argv, link_t *l) {
        int to[2], from[2];
        if (pipe2(to, O_CLOEXEC) != 0) return -1;
        if (pipe2(from, O_CLOEXEC) != 0) {
                close(to[0]);
                close(to[1]);
                return -1;
        }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, to[0], 0);
        posix_spawn_file_actions_adddup2(&fa, from[1], 1);
        pid_t     pid = -1;
        const int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(to[0]);
        close(from[1]);
        if (r != 0) {
                close(to[1]);
                close(from[0]);
                errno = r;
                return -1;
        }
        l->to = to[1];
        l->from = from[0];
        return pid;
}

/* The workers and what they share. */
typedef struct serving {
                sshfs_t        *fs;
                int             server;
                link_t         *l;
                pthread_mutex_t lock;
                int             running;   /* workers still answering */
                int             unmounted; /* a worker's read ended: the mount went */
                sshfs_worker_t  workers[SSHFS_WORKERS];
                pthread_t       threads[SSHFS_WORKERS];
                uint8_t        *requests[SSHFS_WORKERS];
                uint8_t        *replies[SSHFS_WORKERS];
} serving_t;

typedef struct worker_arg {
                serving_t *s;
                int        index;
} worker_arg_t;

/* A worker: a request read, answered, its reply written, until the mount goes or the connection
 * does. Cancelled only while it waits for a request (the connection gone: main()). */
static void *work(void *arg) {
        const worker_arg_t *a = arg;
        serving_t          *s = a->s;
        const size_t        request_max = BMDS_FS_REQUEST_MAX(s->fs->max_data);
        uint8_t            *request = s->requests[a->index], *reply = s->replies[a->index];
        (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        for (;;) {
                (void)pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
                const ssize_t n = read(s->server, request, request_max);
                (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                        pthread_mutex_lock(&s->lock);
                        s->unmounted = 1;
                        pthread_mutex_unlock(&s->lock);
                        break;
                }
                const size_t m = sshfs_answer(&s->workers[a->index], request, (size_t)n, reply);
                if (m == 0) break; /* the connection broke */
                if (write(s->server, reply, m) != (ssize_t)m) break;
        }
        /* The last to go ends the client (its input closed), so the reader's read ends. */
        pthread_mutex_lock(&s->lock);
        if (--s->running == 0 && s->l->to >= 0) {
                close(s->l->to);
                s->l->to = -1;
        }
        pthread_mutex_unlock(&s->lock);
        return NULL;
}

/* Serves the mount until it is unmounted or the connection ends: the workers on threads of
 * their own, the replies read here (devplan/netfs.md step 5). */
static int serve(sshfs_t *fs, int server, link_t *l) {
        static serving_t    s;
        static worker_arg_t args[SSHFS_WORKERS];
        s.fs = fs;
        s.server = server;
        s.l = l;
        pthread_mutex_init(&s.lock, NULL);
        int started = 0, status = 0;
        for (; started < SSHFS_WORKERS; ++started) {
                s.requests[started] = malloc(BMDS_FS_REQUEST_MAX(fs->max_data));
                s.replies[started] = malloc(sshfs_reply_max(fs));
                if (s.requests[started] == NULL || s.replies[started] == NULL ||
                    sshfs_worker_init(fs, &s.workers[started]) != 0) {
                        free(s.requests[started]);
                        free(s.replies[started]);
                        break;
                }
        }
        if (started == 0) {
                fprintf(stderr, "sshfs: %s: out of memory\n", fs->mount);
                return 1;
        }
        /* The answers' paths are on the stack (4 KB of them): more than a thread's least. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 32 * 1024);
        int threads = 0;
        for (; threads < started; ++threads) {
                args[threads].s = &s;
                args[threads].index = threads;
                pthread_mutex_lock(&s.lock);
                s.running++;
                pthread_mutex_unlock(&s.lock);
                if (pthread_create(&s.threads[threads], &attr, work, &args[threads]) != 0) {
                        pthread_mutex_lock(&s.lock);
                        s.running--;
                        pthread_mutex_unlock(&s.lock);
                        break;
                }
        }
        pthread_attr_destroy(&attr);
        if (threads > 0) sshfs_read_replies(fs);
        /* The connection's end without an unmount: it broke. */
        pthread_mutex_lock(&s.lock);
        const int unmounted = s.unmounted;
        pthread_mutex_unlock(&s.lock);
        if (!unmounted) {
                fprintf(stderr, "sshfs: %s: the connection closed\n", fs->mount);
                status = 1;
                for (int i = 0; i < threads; ++i) (void)pthread_cancel(s.threads[i]);
        }
        for (int i = 0; i < threads; ++i) (void)pthread_join(s.threads[i], NULL);
        for (int i = 0; i < started; ++i) {
                sshfs_worker_finish(&s.workers[i]);
                free(s.requests[i]);
                free(s.replies[i]);
        }
        pthread_mutex_destroy(&s.lock);
        return status;
}

/* In the background: this program again (-f, -S FD), waited for until it says it is mounted. */
static int background(int argc, char **argv) {
        int p[2];
        if (pipe(p) != 0) {
                perror("sshfs: pipe");
                return 1;
        }
        char  fd[16];
        char *args[MAX_ARGS + 4];
        int   n = 0;
        snprintf(fd, sizeof(fd), "%d", p[1]);
        args[n++] = argv[0];
        args[n++] = "-f";
        args[n++] = "-S";
        args[n++] = fd;
        for (int i = 1; i < argc && n < MAX_ARGS + 3; ++i) args[n++] = argv[i];
        args[n] = NULL;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addclose(&fa, p[0]);
        pid_t     pid = -1;
        const int r = posix_spawnp(&pid, argv[0], &fa, NULL, args, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(p[1]);
        if (r != 0) {
                fprintf(stderr, "sshfs: %s: %s\n", argv[0], strerror(r));
                return 1;
        }
        char    c;
        ssize_t got;
        while ((got = read(p[0], &c, 1)) < 0 && errno == EINTR) {}
        close(p[0]);
        if (got == 1) return 0; /* mounted: it goes on */
        int st = 0;
        if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return 1;
        return WEXITSTATUS(st) != 0 ? WEXITSTATUS(st) : 1;
}

int main(int argc, char **argv) {
        int   foreground = 0, status_fd = -1, nargs = 0, opt;
        char *client[MAX_ARGS + 8];
        client[nargs++] = "dbclient";
        const char *command = NULL;
        while ((opt = getopt(argc, argv, "fp:i:yo:s:S:")) != -1) {
                if (nargs >= MAX_ARGS) {
                        fprintf(stderr, "sshfs: too many options\n");
                        return 2;
                }
                switch (opt) {
                        case 'f': foreground = 1; break;
                        case 's': command = optarg; break;
                        case 'S': status_fd = atoi(optarg); break;
                        case 'y': client[nargs++] = "-y"; break;
                        case 'p':
                        case 'i':
                        case 'o':
                                client[nargs++] = opt == 'p' ? "-p" : opt == 'i' ? "-i" : "-o";
                                client[nargs++] = optarg;
                                break;
                        default: usage(); return 2;
                }
        }
        if (argc - optind != 2) {
                usage();
                return 2;
        }
        if (!foreground) return background(argc, argv);

        /* [user@]host:[dir] */
        char *spec = argv[optind], *mountpoint = argv[optind + 1];
        char *colon = strchr(spec, ']'); /* past an address in brackets */
        colon = strchr(colon != NULL ? colon : spec, ':');
        if (colon == NULL || (colon == spec && command == NULL)) {
                fprintf(stderr, "sshfs: %s: not [user@]host:[dir]\n", spec);
                return 2;
        }
        *colon = 0;
        const char *dir = colon + 1;
        char        mount[BMDS_FS_PATH_MAX];
        if (mountpoint[0] == '/') {
                if (strlen(mountpoint) >= sizeof(mount)) {
                        fprintf(stderr, "sshfs: %s: too long\n", mountpoint);
                        return 2;
                }
                strcpy(mount, mountpoint);
        } else {
                char cwd[BMDS_FS_PATH_MAX];
                if (getcwd(cwd, sizeof(cwd)) == NULL) {
                        perror("sshfs: getcwd");
                        return 1;
                }
                const char  *base = strcmp(cwd, "/") == 0 ? "" : cwd;
                const size_t b = strlen(base), m = strlen(mountpoint);
                if (b + 1 + m >= sizeof(mount)) {
                        fprintf(stderr, "sshfs: %s: too long\n", mountpoint);
                        return 2;
                }
                memcpy(mount, base, b);
                mount[b] = '/';
                memcpy(mount + b + 1, mountpoint, m + 1);
        }
        /* The command given; else dbclient: no pty, the subsystem, a window that keeps a read in
         * flight. */
        char exec_command[BMDS_FS_PATH_MAX];
        if (command != NULL) {
                /* exec: the server in the shell's place, not a shell waiting for it. */
                if ((size_t)snprintf(exec_command, sizeof(exec_command), "exec %s", command) >= sizeof(exec_command)) {
                        fprintf(stderr, "sshfs: -s: too long\n");
                        return 2;
                }
                nargs = 0;
                client[nargs++] = "sh";
                client[nargs++] = "-c";
                client[nargs++] = exec_command;
        } else {
                client[nargs++] = "-T";
                client[nargs++] = "-W";
                client[nargs++] = "262144";
                client[nargs++] = "-s";
                client[nargs++] = spec;
                client[nargs++] = "sftp";
        }
        client[nargs] = NULL;

        /* A hang-up (the terminal it was started from gone) ends neither it nor the client;
         * a broken connection is an error, not a signal. */
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        link_t      l;
        const pid_t client_pid = start_client(client, &l);
        if (client_pid < 0) {
                fprintf(stderr, "sshfs: %s: %s\n", client[0], strerror(errno));
                return 1;
        }
        static sshfs_t   fs;
        const sshfs_io_t io = {send_all, recv_all, &l};
        char             why[160];
        int              status = 1, server = -1;
        if (sshfs_start(&fs, &io, dir, mount, why, sizeof(why)) != 0) {
                if (spec[0] != 0)
                        fprintf(stderr, "sshfs: %s: %s\n", spec, why);
                else
                        fprintf(stderr, "sshfs: %s\n", why);
        } else {
                const bmds_fs_serve_t c = {.size = sizeof(c),
                                           .flags = BMDS_O_CLOEXEC,
                                           .path = mount,
                                           .type = "sshfs",
                                           .max_data = fs.max_data};
                server = bmds_fs_serve(&c);
                if (server < 0) fprintf(stderr, "sshfs: %s: %s\n", mountpoint, strerror(-server));
        }
        if (server >= 0) {
                if (status_fd >= 0) {
                        /* Mounted: the one waiting goes; this one leaves the terminal's session. */
                        (void)write(status_fd, "1", 1);
                        close(status_fd);
                        status_fd = -1;
                        (void)setsid();
                }
                status = serve(&fs, server, &l);
                close(server); /* unmounted, if it wasn't */
        }
        if (l.to >= 0) close(l.to);
        close(l.from);
        int st;
        while (waitpid(client_pid, &st, 0) < 0 && errno == EINTR) {}
        sshfs_finish(&fs);
        return status;
}
