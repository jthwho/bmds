/**
 * @file      tls.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libtls's API (OpenBSD's, as LibreSSL and libretls ship it), the client calls, over BMDS's TLS
 * sockets (`tls_ctl`, devplan/network.md §35): the kernel does the handshake and the records,
 * so this library (libtls.a, sdk/tls/) is a few KB. Programs written for libtls build as they are.
 *
 * What differs from OpenBSD's:
 * - Clients only: `tls_server()` returns NULL (errno `ENOTSUP`), `tls_accept_*()` fail.
 * - TLS 1.2 only: `tls_config_set_protocols()` without TLSv1.2 fails. The cipher suites are the
 *   kernel's (`tls_config_set_ciphers()` takes the usual names and changes nothing).
 * - `tls_connect_fds()` needs one socket for both directions; `tls_connect_cbs()` isn't offered.
 * - The system's roots are the kernel's (and the setting `net/tls/ca`): `tls_default_ca_cert_file()`
 *   names them, and setting it as the CA file adds nothing. Another file's roots are added to them.
 * - `tls_peer_cert_contains_name()` looks at the certificate's subject name only.
 * - `tls_conn_cipher()` gives mbedTLS's suite names (`TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256`).
 *
 * @par Thread Safety
 * As libtls: a `struct tls` is used by one thread at a time; separate ones are independent.
 */

#ifndef BMDS_TLS_H
#define BMDS_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TLS_API 20200120

#define TLS_PROTOCOL_TLSv1_0 (1 << 1)
#define TLS_PROTOCOL_TLSv1_1 (1 << 2)
#define TLS_PROTOCOL_TLSv1_2 (1 << 3)
#define TLS_PROTOCOL_TLSv1_3 (1 << 4)
#define TLS_PROTOCOL_TLSv1 (TLS_PROTOCOL_TLSv1_0 | TLS_PROTOCOL_TLSv1_1 | TLS_PROTOCOL_TLSv1_2 | TLS_PROTOCOL_TLSv1_3)
#define TLS_PROTOCOLS_ALL TLS_PROTOCOL_TLSv1
#define TLS_PROTOCOLS_DEFAULT (TLS_PROTOCOL_TLSv1_2 | TLS_PROTOCOL_TLSv1_3)

/** @brief What a call on a non-blocking socket waits for: poll for it, then call again. */
#define TLS_WANT_POLLIN -2
#define TLS_WANT_POLLOUT -3

struct tls;
struct tls_config;

/** @brief Nothing to set up: 0. */
int tls_init(void);

/** @brief The last error of @p ctx in words, or NULL. */
const char *tls_error(struct tls *ctx);
const char *tls_config_error(struct tls_config *config);

/** @brief The system's roots (the kernel's): setting it as the CA file adds nothing. */
const char *tls_default_ca_cert_file(void);

struct tls_config *tls_config_new(void);
void               tls_config_free(struct tls_config *config);

int  tls_config_set_ca_file(struct tls_config *config, const char *ca_file);
int  tls_config_set_ca_path(struct tls_config *config, const char *ca_path); /* fails: not offered */
int  tls_config_set_ca_mem(struct tls_config *config, const uint8_t *ca, size_t len);
int  tls_config_set_cert_file(struct tls_config *config, const char *cert_file);
int  tls_config_set_cert_mem(struct tls_config *config, const uint8_t *cert, size_t len);
int  tls_config_set_key_file(struct tls_config *config, const char *key_file);
int  tls_config_set_key_mem(struct tls_config *config, const uint8_t *key, size_t len);
int  tls_config_set_keypair_file(struct tls_config *config, const char *cert_file, const char *key_file);
int  tls_config_set_keypair_mem(struct tls_config *config, const uint8_t *cert, size_t cert_len, const uint8_t *key,
                                size_t key_len);
int  tls_config_set_alpn(struct tls_config *config, const char *alpn);
int  tls_config_set_ciphers(struct tls_config *config, const char *ciphers);
int  tls_config_set_protocols(struct tls_config *config, uint32_t protocols);
int  tls_config_parse_protocols(uint32_t *protocols, const char *protostr);
void tls_config_set_verify_depth(struct tls_config *config, int verify_depth);

void tls_config_insecure_noverifycert(struct tls_config *config);
void tls_config_insecure_noverifyname(struct tls_config *config);
void tls_config_insecure_noverifytime(struct tls_config *config);
void tls_config_verify(struct tls_config *config);

/** @brief Reads all of @p file (for the `_mem` calls); free with `tls_unload_file()`. */
uint8_t *tls_load_file(const char *file, size_t *len, char *password);
void     tls_unload_file(uint8_t *buf, size_t len);

struct tls *tls_client(void);
struct tls *tls_server(void); /* NULL, errno ENOTSUP: clients only */
int         tls_configure(struct tls *ctx, struct tls_config *config);
void        tls_reset(struct tls *ctx);
void        tls_free(struct tls *ctx);

int tls_accept_socket(struct tls *ctx, struct tls **cctx, int s);                  /* fails: clients only */
int tls_accept_fds(struct tls *ctx, struct tls **cctx, int fd_read, int fd_write); /* fails */

/** @brief Connects to @p host (@p port, or "host:port" with @p port NULL) and starts TLS. */
int tls_connect(struct tls *ctx, const char *host, const char *port);
int tls_connect_servername(struct tls *ctx, const char *host, const char *port, const char *servername);
/** @brief Starts TLS on a connected (or connecting) TCP socket @p s; the caller keeps and closes it. */
int tls_connect_socket(struct tls *ctx, int s, const char *servername);
int tls_connect_fds(struct tls *ctx, int fd_read, int fd_write, const char *servername);

/**
 * @brief Finishes the handshake (`tls_read()` and `tls_write()` also wait for it).
 * @return 0; -1 (`tls_error()`); `TLS_WANT_POLLOUT` on a non-blocking socket while it runs.
 */
int     tls_handshake(struct tls *ctx);
ssize_t tls_read(struct tls *ctx, void *buf, size_t buflen);
ssize_t tls_write(struct tls *ctx, const void *buf, size_t buflen);
/** @brief Sends close_notify; closes the socket if `tls_connect()` opened it. */
int tls_close(struct tls *ctx);

int         tls_peer_cert_provided(struct tls *ctx);
int         tls_peer_cert_contains_name(struct tls *ctx, const char *name);
const char *tls_peer_cert_hash(struct tls *ctx); /* "SHA256:<hex>" */
const char *tls_peer_cert_issuer(struct tls *ctx);
const char *tls_peer_cert_subject(struct tls *ctx);
time_t      tls_peer_cert_notbefore(struct tls *ctx);
time_t      tls_peer_cert_notafter(struct tls *ctx);
const char *tls_conn_alpn_selected(struct tls *ctx);
const char *tls_conn_cipher(struct tls *ctx);
int         tls_conn_cipher_strength(struct tls *ctx);
const char *tls_conn_servername(struct tls *ctx);
const char *tls_conn_version(struct tls *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BMDS_TLS_H */
