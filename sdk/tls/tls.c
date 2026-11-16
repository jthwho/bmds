/**
 * @file      tls.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libtls.a (tls.h, devplan/network.md §35d): libtls's client calls over BMDS's TLS sockets. The
 * kernel does the handshake and the records (`tls_ctl`); this keeps the configuration, opens the
 * connection, says failures in words, and emulates libtls's separate "no verify" choices from the
 * reasons the kernel reports.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <bmds/errno.h>
#include <bmds/sys.h>
#include <tls.h>

/* The name tls_default_ca_cert_file() gives the system's roots: the kernel's own. */
#define SYSTEM_ROOTS "/etc/ssl/cert.pem"

/* tls_ctl's limits (sys.h's bmds_tls_config_t). */
#define MAX_ROOTS (256u * 1024u)
#define MAX_CERTIFICATE (16u * 1024u)

enum {
        STATE_NONE,
        STATE_STARTED,
        STATE_OPEN,
        STATE_FAILED,
        STATE_CLOSED
};

struct tls_config {
                int      refs;
                uint8_t *ca;
                size_t   ca_len;
                uint8_t *cert;
                size_t   cert_len;
                uint8_t *key;
                size_t   key_len;
                char     alpn[128];
                int      noverifycert;
                int      noverifyname;
                int      noverifytime;
                char     error[256];
                int      has_error;
};

struct tls {
                struct tls_config *config;
                int                fd;
                int                own_fd; /* tls_connect() opened it: tls_close() closes it */
                int                state;
                char               servername[256];
                bmds_tls_info_t    info;
                char               hash[7 + 64 + 1];
                char               error[256];
                int                has_error;
};

static void set_error(char *buf, int *has, const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, 256, fmt, ap);
        va_end(ap);
        *has = 1;
        return;
}

#define CTX_ERROR(ctx, ...) set_error((ctx)->error, &(ctx)->has_error, __VA_ARGS__)
#define CONFIG_ERROR(config, ...) set_error((config)->error, &(config)->has_error, __VA_ARGS__)

int tls_init(void) {
        return 0;
}

const char *tls_error(struct tls *ctx) {
        return ctx != NULL && ctx->has_error ? ctx->error : NULL;
}

const char *tls_config_error(struct tls_config *config) {
        return config != NULL && config->has_error ? config->error : NULL;
}

const char *tls_default_ca_cert_file(void) {
        return SYSTEM_ROOTS;
}

/* --- Configuration ---------------------------------------------------------------------------- */

struct tls_config *tls_config_new(void) {
        struct tls_config *config = calloc(1, sizeof(*config));
        if (config != NULL) config->refs = 1;
        return config;
}

void tls_config_free(struct tls_config *config) {
        if (config == NULL || --config->refs > 0) return;
        free(config->ca);
        free(config->cert);
        if (config->key != NULL) explicit_bzero(config->key, config->key_len);
        free(config->key);
        free(config);
        return;
}

uint8_t *tls_load_file(const char *file, size_t *len, char *password) {
        (void)password; /* encrypted keys aren't taken: the kernel refuses them */
        *len = 0;
        const int fd = open(file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return NULL;
        struct stat st;
        uint8_t    *buf = NULL;
        if (fstat(fd, &st) == 0 && st.st_size >= 0 && st.st_size <= (off_t)MAX_ROOTS) {
                buf = malloc((size_t)st.st_size + 1);
                size_t have = 0;
                while (buf != NULL && have < (size_t)st.st_size) {
                        const ssize_t n = read(fd, buf + have, (size_t)st.st_size - have);
                        if (n <= 0) {
                                free(buf);
                                buf = NULL;
                                break;
                        }
                        have += (size_t)n;
                }
                if (buf != NULL) {
                        buf[have] = '\0';
                        *len = have;
                }
        } else if (st.st_size > (off_t)MAX_ROOTS) {
                errno = EFBIG;
        }
        close(fd);
        return buf;
}

void tls_unload_file(uint8_t *buf, size_t len) {
        if (buf != NULL) explicit_bzero(buf, len);
        free(buf);
        return;
}

/* Replaces *@p into with a copy of @p data (at most @p limit bytes). */
static int set_mem(struct tls_config *config, uint8_t **into, size_t *into_len, const uint8_t *data, size_t len,
                   size_t limit, const char *what) {
        if (len > limit) {
                CONFIG_ERROR(config, "%s: %zu bytes, more than %zu", what, len, limit);
                return -1;
        }
        uint8_t *copy = NULL;
        if (len > 0) {
                copy = malloc(len);
                if (copy == NULL) {
                        CONFIG_ERROR(config, "%s: out of memory", what);
                        return -1;
                }
                memcpy(copy, data, len);
        }
        if (*into != NULL) explicit_bzero(*into, *into_len);
        free(*into);
        *into = copy;
        *into_len = len;
        return 0;
}

static int set_file(struct tls_config *config, uint8_t **into, size_t *into_len, const char *file, size_t limit,
                    const char *what) {
        size_t   len;
        uint8_t *data = tls_load_file(file, &len, NULL);
        if (data == NULL) {
                CONFIG_ERROR(config, "%s: %s: %s", what, file, strerror(errno));
                return -1;
        }
        const int r = set_mem(config, into, into_len, data, len, limit, what);
        tls_unload_file(data, len);
        return r;
}

int tls_config_set_ca_file(struct tls_config *config, const char *ca_file) {
        /* The system's roots are the kernel's: nothing to add. */
        if (ca_file == NULL || strcmp(ca_file, SYSTEM_ROOTS) == 0)
                return set_mem(config, &config->ca, &config->ca_len, NULL, 0, MAX_ROOTS, "CA file");
        return set_file(config, &config->ca, &config->ca_len, ca_file, MAX_ROOTS, "CA file");
}

int tls_config_set_ca_path(struct tls_config *config, const char *ca_path) {
        CONFIG_ERROR(config, "CA path %s: not offered (use a CA file)", ca_path != NULL ? ca_path : "");
        return -1;
}

int tls_config_set_ca_mem(struct tls_config *config, const uint8_t *ca, size_t len) {
        return set_mem(config, &config->ca, &config->ca_len, ca, len, MAX_ROOTS, "CA");
}

int tls_config_set_cert_file(struct tls_config *config, const char *cert_file) {
        return set_file(config, &config->cert, &config->cert_len, cert_file, MAX_CERTIFICATE, "certificate");
}

int tls_config_set_cert_mem(struct tls_config *config, const uint8_t *cert, size_t len) {
        return set_mem(config, &config->cert, &config->cert_len, cert, len, MAX_CERTIFICATE, "certificate");
}

int tls_config_set_key_file(struct tls_config *config, const char *key_file) {
        return set_file(config, &config->key, &config->key_len, key_file, MAX_CERTIFICATE, "key");
}

int tls_config_set_key_mem(struct tls_config *config, const uint8_t *key, size_t len) {
        return set_mem(config, &config->key, &config->key_len, key, len, MAX_CERTIFICATE, "key");
}

int tls_config_set_keypair_file(struct tls_config *config, const char *cert_file, const char *key_file) {
        if (tls_config_set_cert_file(config, cert_file) != 0) return -1;
        return tls_config_set_key_file(config, key_file);
}

int tls_config_set_keypair_mem(struct tls_config *config, const uint8_t *cert, size_t cert_len, const uint8_t *key,
                               size_t key_len) {
        if (tls_config_set_cert_mem(config, cert, cert_len) != 0) return -1;
        return tls_config_set_key_mem(config, key, key_len);
}

int tls_config_set_alpn(struct tls_config *config, const char *alpn) {
        if (alpn == NULL) alpn = "";
        if (strlen(alpn) >= sizeof(config->alpn)) {
                CONFIG_ERROR(config, "ALPN: longer than %zu bytes", sizeof(config->alpn) - 1);
                return -1;
        }
        strcpy(config->alpn, alpn);
        return 0;
}

int tls_config_set_ciphers(struct tls_config *config, const char *ciphers) {
        (void)config;
        (void)ciphers; /* the kernel's suites, AES-GCM and ChaCha20-Poly1305 with ECDHE, whatever is asked */
        return 0;
}

int tls_config_set_protocols(struct tls_config *config, uint32_t protocols) {
        if ((protocols & TLS_PROTOCOL_TLSv1_2) == 0) {
                CONFIG_ERROR(config, "only TLS 1.2 is offered");
                return -1;
        }
        return 0;
}

int tls_config_parse_protocols(uint32_t *protocols, const char *protostr) {
        uint32_t out = 0;
        char     copy[128];
        if (protostr == NULL || strlen(protostr) >= sizeof(copy)) return -1;
        strcpy(copy, protostr);
        char *save = NULL;
        for (char *word = strtok_r(copy, ",:", &save); word != NULL; word = strtok_r(NULL, ",:", &save)) {
                const int negate = *word == '!' || *word == '-';
                if (negate) ++word;
                uint32_t bits;
                if (strcasecmp(word, "all") == 0 || strcasecmp(word, "legacy") == 0)
                        bits = TLS_PROTOCOLS_ALL;
                else if (strcasecmp(word, "default") == 0 || strcasecmp(word, "secure") == 0)
                        bits = TLS_PROTOCOLS_DEFAULT;
                else if (strcasecmp(word, "tlsv1") == 0)
                        bits = TLS_PROTOCOL_TLSv1;
                else if (strcasecmp(word, "tlsv1.0") == 0)
                        bits = TLS_PROTOCOL_TLSv1_0;
                else if (strcasecmp(word, "tlsv1.1") == 0)
                        bits = TLS_PROTOCOL_TLSv1_1;
                else if (strcasecmp(word, "tlsv1.2") == 0)
                        bits = TLS_PROTOCOL_TLSv1_2;
                else if (strcasecmp(word, "tlsv1.3") == 0)
                        bits = TLS_PROTOCOL_TLSv1_3;
                else
                        return -1;
                out = negate ? out & ~bits : out | bits;
        }
        *protocols = out;
        return 0;
}

void tls_config_set_verify_depth(struct tls_config *config, int verify_depth) {
        (void)config;
        (void)verify_depth;
        return;
}

void tls_config_insecure_noverifycert(struct tls_config *config) {
        config->noverifycert = 1;
        return;
}

void tls_config_insecure_noverifyname(struct tls_config *config) {
        config->noverifyname = 1;
        return;
}

void tls_config_insecure_noverifytime(struct tls_config *config) {
        config->noverifytime = 1;
        return;
}

void tls_config_verify(struct tls_config *config) {
        config->noverifycert = config->noverifyname = config->noverifytime = 0;
        return;
}

/* --- Contexts --------------------------------------------------------------------------------- */

static struct tls *new_context(void) {
        struct tls *ctx = calloc(1, sizeof(*ctx));
        if (ctx != NULL) ctx->fd = -1;
        return ctx;
}

struct tls *tls_client(void) {
        return new_context();
}

struct tls *tls_server(void) {
        errno = ENOTSUP;
        return NULL;
}

int tls_configure(struct tls *ctx, struct tls_config *config) {
        if (config == NULL) {
                CTX_ERROR(ctx, "no configuration");
                return -1;
        }
        ++config->refs;
        tls_config_free(ctx->config);
        ctx->config = config;
        return 0;
}

void tls_reset(struct tls *ctx) {
        if (ctx->own_fd && ctx->fd >= 0) close(ctx->fd);
        tls_config_free(ctx->config);
        memset(ctx, 0, sizeof(*ctx));
        ctx->fd = -1;
        return;
}

void tls_free(struct tls *ctx) {
        if (ctx == NULL) return;
        tls_reset(ctx);
        free(ctx);
        return;
}

int tls_accept_socket(struct tls *ctx, struct tls **cctx, int s) {
        (void)cctx;
        (void)s;
        CTX_ERROR(ctx, "TLS servers aren't offered (clients only)");
        return -1;
}

int tls_accept_fds(struct tls *ctx, struct tls **cctx, int fd_read, int fd_write) {
        (void)fd_read;
        (void)fd_write;
        return tls_accept_socket(ctx, cctx, -1);
}

/* --- Connecting ------------------------------------------------------------------------------- */

int tls_connect_socket(struct tls *ctx, int s, const char *servername) {
        if (ctx->state != STATE_NONE) {
                CTX_ERROR(ctx, "already connected");
                return -1;
        }
        struct tls_config  empty = {.refs = 1};
        struct tls_config *c = ctx->config != NULL ? ctx->config : &empty;
        const int          relaxed = c->noverifycert || c->noverifyname || c->noverifytime;
        if (servername == NULL) servername = "";
        snprintf(ctx->servername, sizeof(ctx->servername), "%s", servername);
        bmds_tls_config_t k = {
                .size = sizeof(k),
                .flags = relaxed ? BMDS_TLS_NO_VERIFY : 0u,
                .server_name = servername[0] != '\0' ? servername : NULL,
                .ca = c->ca,
                .ca_size = (uint32_t)c->ca_len,
                .cert = c->cert,
                .cert_size = (uint32_t)c->cert_len,
                .key = c->key,
                .key_size = (uint32_t)c->key_len,
                .alpn = c->alpn[0] != '\0' ? c->alpn : NULL,
        };
        const int r = bmds_tls_ctl(s, BMDS_TLS_START, &k);
        if (r < 0) {
                switch (-r) {
                        case BMDS_ENOBUFS: CTX_ERROR(ctx, "too many TLS sockets (the setting net/tls/sessions)"); break;
                        case BMDS_EINVAL:
                                CTX_ERROR(ctx, servername[0] == '\0' && !relaxed
                                                       ? "a server name is needed to check its certificate"
                                                       : "a certificate, key or root doesn't parse (PEM or DER)");
                                break;
                        case BMDS_ENOSYS: CTX_ERROR(ctx, "this system has no TLS sockets"); break;
                        default: CTX_ERROR(ctx, "%s", strerror(-r)); break;
                }
                errno = -r;
                return -1;
        }
        ctx->fd = s;
        ctx->state = STATE_STARTED;
        return 0;
}

int tls_connect_fds(struct tls *ctx, int fd_read, int fd_write, const char *servername) {
        if (fd_read != fd_write) {
                CTX_ERROR(ctx, "one socket for both directions is needed");
                return -1;
        }
        return tls_connect_socket(ctx, fd_read, servername);
}

int tls_connect_servername(struct tls *ctx, const char *host, const char *port, const char *servername) {
        char name[256];
        if (host == NULL) {
                CTX_ERROR(ctx, "no host");
                return -1;
        }
        snprintf(name, sizeof(name), "%s", host);
        if (port == NULL) {
                char *colon = strrchr(name, ':');
                if (colon == NULL) {
                        CTX_ERROR(ctx, "no port");
                        return -1;
                }
                *colon = '\0';
                port = colon + 1;
        }
        struct addrinfo  hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
        struct addrinfo *list = NULL;
        const int        g = getaddrinfo(name, port, &hints, &list);
        if (g != 0) {
                CTX_ERROR(ctx, "%s: %s", name, gai_strerror(g));
                return -1;
        }
        int fd = -1;
        int err = 0;
        for (struct addrinfo *a = list; a != NULL && fd < 0; a = a->ai_next) {
                fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
                if (fd < 0) {
                        err = errno;
                        continue;
                }
                if (connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
                        err = errno;
                        close(fd);
                        fd = -1;
                }
        }
        freeaddrinfo(list);
        if (fd < 0) {
                CTX_ERROR(ctx, "%s port %s: %s", name, port, strerror(err));
                return -1;
        }
        if (tls_connect_socket(ctx, fd, servername != NULL ? servername : name) != 0) {
                close(fd);
                return -1;
        }
        ctx->own_fd = 1;
        return 0;
}

int tls_connect(struct tls *ctx, const char *host, const char *port) {
        return tls_connect_servername(ctx, host, port, NULL);
}

/* --- The session ------------------------------------------------------------------------------ */

static int refresh(struct tls *ctx) {
        memset(&ctx->info, 0, sizeof(ctx->info));
        ctx->info.size = sizeof(ctx->info);
        return bmds_tls_ctl(ctx->fd, BMDS_TLS_INFO, &ctx->info);
}

/* A refused certificate's reasons, in words. */
static const char *verify_words(uint32_t bits) {
        if (bits & BMDS_TLS_VERIFY_CLOCK) return "the system clock isn't set: a certificate's dates can't be checked";
        if (bits & BMDS_TLS_VERIFY_EXPIRED) return "its certificate has expired";
        if (bits & BMDS_TLS_VERIFY_NOT_YET_VALID) return "its certificate isn't valid yet";
        if (bits & BMDS_TLS_VERIFY_NAME) return "its certificate is for another name";
        if (bits & BMDS_TLS_VERIFY_UNTRUSTED) return "its certificate isn't from an authority this system trusts";
        return "its certificate was refused";
}

/* The session failed with @p err (positive): said in words. */
static int failed(struct tls *ctx, int err) {
        ctx->state = STATE_FAILED;
        if (refresh(ctx) == 0 && ctx->info.error != 0) err = ctx->info.error;
        const char *name = ctx->servername[0] != '\0' ? ctx->servername : "the server";
        switch (err) {
                case EKEYREJECTED: CTX_ERROR(ctx, "%s: %s", name, verify_words(ctx->info.verify)); break;
                case EPROTO: CTX_ERROR(ctx, "%s: the TLS handshake failed", name); break;
                case ETIMEDOUT: CTX_ERROR(ctx, "%s: the TLS handshake took too long", name); break;
                case EBADMSG: CTX_ERROR(ctx, "%s: a TLS record failed its check", name); break;
                default: CTX_ERROR(ctx, "%s: %s", name, strerror(err)); break;
        }
        errno = err;
        return -1;
}

/* What libtls's separate choices allow of the reasons the kernel found (NO_VERIFY was asked). */
static uint32_t allowed(const struct tls_config *c) {
        uint32_t mask = 0;
        if (c == NULL) return 0;
        if (c->noverifycert) mask |= BMDS_TLS_VERIFY_UNTRUSTED | BMDS_TLS_VERIFY_OTHER;
        if (c->noverifyname) mask |= BMDS_TLS_VERIFY_NAME;
        if (c->noverifytime) mask |= BMDS_TLS_VERIFY_EXPIRED | BMDS_TLS_VERIFY_NOT_YET_VALID | BMDS_TLS_VERIFY_CLOCK;
        return mask;
}

int tls_handshake(struct tls *ctx) {
        if (ctx->state == STATE_OPEN) return 0;
        if (ctx->state != STATE_STARTED) {
                if (ctx->state == STATE_NONE) CTX_ERROR(ctx, "not connected");
                return -1;
        }
        const int flags = fcntl(ctx->fd, F_GETFL);
        const int nonblock = flags >= 0 && (flags & O_NONBLOCK) != 0;
        uint32_t  dontwait = BMDS_TLS_DONTWAIT;
        const int r = bmds_tls_ctl(ctx->fd, BMDS_TLS_HANDSHAKE, nonblock ? &dontwait : NULL);
        if (r == -BMDS_EAGAIN) {
                /* Not blocking: its end makes the socket writable. Blocking: SO_RCVTIMEO passed. */
                if (nonblock) return TLS_WANT_POLLOUT;
                CTX_ERROR(ctx, "the TLS handshake didn't end within the socket's timeout");
                errno = EAGAIN;
                return -1;
        }
        if (r == -BMDS_EINTR) {
                /* A signal: its handler runs as a libc call returns (linux.c), so make one. The
                 * handshake goes on. */
                (void)getppid();
                CTX_ERROR(ctx, "interrupted");
                errno = EINTR;
                return -1;
        }
        if (r < 0) return failed(ctx, -r);
        if (refresh(ctx) != 0) return failed(ctx, EIO);
        const uint32_t refused = ctx->info.verify & ~allowed(ctx->config);
        if (refused != 0 && (ctx->config != NULL &&
                             (ctx->config->noverifycert || ctx->config->noverifyname || ctx->config->noverifytime))) {
                ctx->state = STATE_FAILED;
                CTX_ERROR(ctx, "%s: %s", ctx->servername[0] != '\0' ? ctx->servername : "the server",
                          verify_words(refused));
                errno = EKEYREJECTED;
                return -1;
        }
        ctx->state = STATE_OPEN;
        return 0;
}

/* Before a read or write: the handshake, as libtls does it first. 1 to go on, or what to return. */
static int ready(struct tls *ctx, ssize_t *out) {
        if (ctx->state == STATE_OPEN) return 1;
        if (ctx->state == STATE_FAILED || ctx->state == STATE_CLOSED) {
                if (!ctx->has_error) CTX_ERROR(ctx, "the connection is closed");
                *out = -1;
                return 0;
        }
        const int h = tls_handshake(ctx);
        if (h == 0) return 1;
        *out = h;
        return 0;
}

ssize_t tls_read(struct tls *ctx, void *buf, size_t buflen) {
        ssize_t r;
        if (!ready(ctx, &r)) return r;
        r = read(ctx->fd, buf, buflen);
        if (r >= 0) return r;
        if (errno == EAGAIN) return TLS_WANT_POLLIN;
        if (errno == EINTR) {
                CTX_ERROR(ctx, "interrupted");
                return -1;
        }
        return failed(ctx, errno);
}

ssize_t tls_write(struct tls *ctx, const void *buf, size_t buflen) {
        ssize_t r;
        if (!ready(ctx, &r)) return r;
        r = send(ctx->fd, buf, buflen, MSG_NOSIGNAL);
        if (r >= 0) return r;
        if (errno == EAGAIN) return TLS_WANT_POLLOUT;
        if (errno == EINTR) {
                CTX_ERROR(ctx, "interrupted");
                return -1;
        }
        return failed(ctx, errno);
}

int tls_close(struct tls *ctx) {
        if (ctx->fd >= 0 && (ctx->state == STATE_OPEN || ctx->state == STATE_STARTED))
                (void)shutdown(ctx->fd, SHUT_WR); /* close_notify, then the FIN */
        if (ctx->own_fd && ctx->fd >= 0) close(ctx->fd);
        ctx->own_fd = 0;
        ctx->fd = -1;
        ctx->state = STATE_CLOSED;
        return 0;
}

/* --- What the handshake told ------------------------------------------------------------------ */

static const bmds_tls_info_t *info_of(struct tls *ctx) {
        if (ctx->state != STATE_OPEN && ctx->state != STATE_FAILED) return NULL;
        if (ctx->fd >= 0) (void)refresh(ctx);
        return ctx->info.version != 0 || ctx->info.subject[0] != '\0' ? &ctx->info : NULL;
}

int tls_peer_cert_provided(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL && i->subject[0] != '\0';
}

/* @p name against @p pattern (a leading "*." matches one label). */
static int name_matches(const char *pattern, size_t length, const char *name) {
        if (length > 2 && pattern[0] == '*' && pattern[1] == '.') {
                const char *dot = strchr(name, '.');
                return dot != NULL && dot != name && strlen(dot) == length - 1 &&
                       strncasecmp(dot, pattern + 1, length - 1) == 0;
        }
        return strlen(name) == length && strncasecmp(pattern, name, length) == 0;
}

int tls_peer_cert_contains_name(struct tls *ctx, const char *name) {
        const bmds_tls_info_t *i = info_of(ctx);
        if (i == NULL || name == NULL) return 0;
        const char *cn = strstr(i->subject, "CN=");
        if (cn == NULL) return 0;
        cn += 3;
        const size_t length = strcspn(cn, ",");
        return name_matches(cn, length, name);
}

const char *tls_peer_cert_hash(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        if (i == NULL) return NULL;
        static const char hex[] = "0123456789abcdef";
        strcpy(ctx->hash, "SHA256:");
        for (int n = 0; n < 32; ++n) {
                ctx->hash[7 + 2 * n] = hex[i->peer_sha256[n] >> 4];
                ctx->hash[8 + 2 * n] = hex[i->peer_sha256[n] & 15];
        }
        ctx->hash[7 + 64] = '\0';
        return ctx->hash;
}

const char *tls_peer_cert_issuer(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL ? i->issuer : NULL;
}

const char *tls_peer_cert_subject(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL ? i->subject : NULL;
}

time_t tls_peer_cert_notbefore(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL ? (time_t)i->not_before : -1;
}

time_t tls_peer_cert_notafter(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL ? (time_t)i->not_after : -1;
}

const char *tls_conn_alpn_selected(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL && i->alpn[0] != '\0' ? i->alpn : NULL;
}

const char *tls_conn_cipher(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        return i != NULL && i->cipher_name[0] != '\0' ? i->cipher_name : NULL;
}

int tls_conn_cipher_strength(struct tls *ctx) {
        const char *name = tls_conn_cipher(ctx);
        if (name == NULL) return 0;
        if (strstr(name, "AES-256") != NULL || strstr(name, "CHACHA20") != NULL) return 256;
        if (strstr(name, "AES-128") != NULL) return 128;
        return 0;
}

const char *tls_conn_servername(struct tls *ctx) {
        return ctx->servername[0] != '\0' ? ctx->servername : NULL;
}

const char *tls_conn_version(struct tls *ctx) {
        const bmds_tls_info_t *i = info_of(ctx);
        if (i == NULL) return NULL;
        switch (i->version) {
                case 0x0303: return "TLSv1.2";
                case 0x0304: return "TLSv1.3";
                default: return "unknown";
        }
}
