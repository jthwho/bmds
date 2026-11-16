/**
 * @file      tlsclient.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * tlsclient: connects to a TLS server, says who answered, then relays the terminal to it, or runs
 * a command with the connection as its stdin and stdout (devplan/network.md §35d). OpenSSL's
 * `s_client` job, through libtls (tls.h) on the kernel's TLS sockets.
 *
 *   tlsclient [-k] [-q] [-r] [-a ALPN] [-n NAME] [-C CAFILE] [-c CERT -K KEY] HOST [PORT]
 *   tlsclient [options] -e HOST PORT COMMAND [ARG...]
 *
 * -k  don't check the certificate (still says what was wrong with it)
 * -q  say nothing about the server
 * -r  send the terminal's lines with CRLF ends (HTTP, IRC, SMTP)
 * -a  protocols offered, comma separated (h2,http/1.1)
 * -n  the name the certificate must have, and SNI (default HOST): to connect by address
 * -C  more trusted roots (PEM)
 * -c, -K  a client certificate and its key
 * -e  COMMAND reads the server from its stdin and writes to it on its stdout: any plaintext program
 *     over TLS (the session is the socket's, so the child needs nothing of TLS)
 *
 * PORT is 443 unless given. The terminal's end (Ctrl-D) stops sending; the server's end stops it.
 */

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <tls.h>

extern char **environ;

static int usage(void) {
        fprintf(stderr, "usage: tlsclient [-k] [-q] [-r] [-a ALPN] [-n NAME] [-C CAFILE] [-c CERT -K KEY] HOST [PORT]\n"
                        "       tlsclient [options] -e HOST PORT COMMAND [ARG...]\n");
        return 2;
}

static int connect_to(const char *host, const char *port) {
        struct addrinfo  hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
        struct addrinfo *list = NULL;
        const int        g = getaddrinfo(host, port, &hints, &list);
        if (g != 0) {
                fprintf(stderr, "tlsclient: %s: %s\n", host, gai_strerror(g));
                return -1;
        }
        int fd = -1;
        int err = 0;
        for (struct addrinfo *a = list; a != NULL && fd < 0; a = a->ai_next) {
                fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
                        err = errno;
                        close(fd);
                        fd = -1;
                } else if (fd < 0) {
                        err = errno;
                }
        }
        freeaddrinfo(list);
        if (fd < 0) fprintf(stderr, "tlsclient: %s port %s: %s\n", host, port, strerror(err));
        return fd;
}

static void say_date(const char *what, time_t t) {
        struct tm tm;
        char      text[32];
        if (t < 0 || gmtime_r(&t, &tm) == NULL || strftime(text, sizeof(text), "%Y-%m-%d %H:%M UTC", &tm) == 0)
                snprintf(text, sizeof(text), "?");
        fprintf(stderr, "  %s %s\n", what, text);
        return;
}

static int write_all(struct tls *ctx, const char *data, size_t n) {
        while (n > 0) {
                const ssize_t w = tls_write(ctx, data, n);
                if (w < 0) {
                        fprintf(stderr, "tlsclient: %s\n", tls_error(ctx));
                        return -1;
                }
                data += w;
                n -= (size_t)w;
        }
        return 0;
}

/* The terminal to the server and the server to the terminal, until the server ends. */
static int relay(struct tls *ctx, int fd, int crlf) {
        char          buf[2048];
        struct pollfd p[2] = {{.fd = fd, .events = POLLIN}, {.fd = 0, .events = POLLIN}};
        int           sending = 1;
        for (;;) {
                if (poll(p, sending ? 2 : 1, -1) < 0) {
                        if (errno == EINTR) continue;
                        perror("tlsclient: poll");
                        return 1;
                }
                if (p[0].revents != 0) {
                        const ssize_t n = tls_read(ctx, buf, sizeof(buf));
                        if (n == 0) return 0;
                        if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) continue;
                        if (n < 0) {
                                fprintf(stderr, "tlsclient: %s\n", tls_error(ctx));
                                return 1;
                        }
                        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) return 1;
                        fflush(stdout);
                }
                if (sending && p[1].revents != 0) {
                        const ssize_t n = read(0, buf, sizeof(buf) / 2);
                        if (n <= 0) {
                                sending = 0; /* keep reading what the server says */
                                continue;
                        }
                        size_t start = 0;
                        for (ssize_t i = 0; i < n; ++i) {
                                if (!crlf || buf[i] != '\n') continue;
                                if (write_all(ctx, buf + start, (size_t)i - start) != 0 ||
                                    write_all(ctx, "\r\n", 2) != 0)
                                        return 1;
                                start = (size_t)i + 1;
                        }
                        if (write_all(ctx, buf + start, (size_t)n - start) != 0) return 1;
                }
        }
}

int main(int argc, char **argv) {
        int         insecure = 0, quiet = 0, crlf = 0, run = 0;
        const char *alpn = NULL, *name = NULL, *cafile = NULL, *cert = NULL, *key = NULL;
        int         opt;
        while ((opt = getopt(argc, argv, "+kqra:n:C:c:K:e")) != -1) {
                switch (opt) {
                        case 'k': insecure = 1; break;
                        case 'q': quiet = 1; break;
                        case 'r': crlf = 1; break;
                        case 'a': alpn = optarg; break;
                        case 'n': name = optarg; break;
                        case 'C': cafile = optarg; break;
                        case 'c': cert = optarg; break;
                        case 'K': key = optarg; break;
                        case 'e': run = 1; break;
                        default: return usage();
                }
        }
        const int left = argc - optind;
        if (left < 1 || (!run && left > 2) || (run && left < 3) || (cert == NULL) != (key == NULL)) return usage();
        const char *host = argv[optind];
        const char *port = left > 1 ? argv[optind + 1] : "443";
        signal(SIGPIPE, SIG_IGN);

        struct tls_config *config = tls_config_new();
        struct tls        *ctx = tls_client();
        if (config == NULL || ctx == NULL) {
                fprintf(stderr, "tlsclient: out of memory\n");
                return 1;
        }
        if (insecure) {
                tls_config_insecure_noverifycert(config);
                tls_config_insecure_noverifyname(config);
                tls_config_insecure_noverifytime(config);
        }
        if ((alpn != NULL && tls_config_set_alpn(config, alpn) != 0) ||
            (cafile != NULL && tls_config_set_ca_file(config, cafile) != 0) ||
            (cert != NULL && tls_config_set_keypair_file(config, cert, key) != 0)) {
                fprintf(stderr, "tlsclient: %s\n", tls_config_error(config));
                return 1;
        }
        if (tls_configure(ctx, config) != 0) {
                fprintf(stderr, "tlsclient: %s\n", tls_error(ctx));
                return 1;
        }
        const int fd = connect_to(host, port);
        if (fd < 0) return 1;
        if (tls_connect_socket(ctx, fd, name != NULL ? name : host) != 0 || tls_handshake(ctx) != 0) {
                fprintf(stderr, "tlsclient: %s\n", tls_error(ctx));
                return 1;
        }
        if (!quiet) {
                const char *chosen = tls_conn_alpn_selected(ctx);
                fprintf(stderr, "tlsclient: %s port %s: %s, %s%s%s%s\n", host, port, tls_conn_version(ctx),
                        tls_conn_cipher(ctx), chosen != NULL ? ", ALPN " : "", chosen != NULL ? chosen : "",
                        insecure ? " (not checked)" : "");
                fprintf(stderr, "  subject %s\n  issuer  %s\n", tls_peer_cert_subject(ctx), tls_peer_cert_issuer(ctx));
                say_date("from   ", tls_peer_cert_notbefore(ctx));
                say_date("until  ", tls_peer_cert_notafter(ctx));
                fprintf(stderr, "  %s\n", tls_peer_cert_hash(ctx));
        }
        int status;
        if (run) {
                /* The socket is the child's stdin and stdout: the kernel's session is the socket's. */
                posix_spawn_file_actions_t fa;
                posix_spawn_file_actions_init(&fa);
                posix_spawn_file_actions_adddup2(&fa, fd, 0);
                posix_spawn_file_actions_adddup2(&fa, fd, 1);
                pid_t     pid;
                const int r = posix_spawnp(&pid, argv[optind + 2], &fa, NULL, argv + optind + 2, environ);
                posix_spawn_file_actions_destroy(&fa);
                if (r != 0) {
                        fprintf(stderr, "tlsclient: %s: %s\n", argv[optind + 2], strerror(r));
                        return 1;
                }
                int st = 0;
                if (waitpid(pid, &st, 0) != pid) st = 1 << 8;
                status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        } else {
                status = relay(ctx, fd, crlf);
        }
        tls_close(ctx);
        close(fd);
        tls_free(ctx);
        tls_config_free(config);
        return status;
}
