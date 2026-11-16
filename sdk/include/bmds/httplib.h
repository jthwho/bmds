/**
 * @file      httplib.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdshttp (devplan/network.md §35d): an HTTP/1.1 client for programs, `http:` and `https:`
 * (the kernel's TLS sockets, `bmds_tls_ctl`). Its parser is the kernel's own (bmds_core's
 * `Http`, built into the library), so a program and `pkg` read responses alike.
 *
 * @code
 * bmds_http_request_t req = {.size = sizeof(req), .url = "https://example.com/"};
 * bmds_http_t        *h;
 * if (bmds_http_open(&req, &h) == 0 && bmds_http_status(h) == 200) {
 *         char buf[1024];
 *         int32_t n;
 *         while ((n = bmds_http_read(h, buf, sizeof(buf))) > 0) fwrite(buf, 1, n, stdout);
 * } else {
 *         fprintf(stderr, "%s\n", bmds_http_error(h));
 * }
 * bmds_http_close(h);
 * @endcode
 *
 * One request a connection (`Connection: close`), blocking (`timeout_ms` bounds each wait).
 * Redirects are followed (at most `max_redirects`); a 303, or a 301 or 302 after `POST`, asks
 * again as `GET` without the body, as browsers do. The answer's body comes as it was sent: by
 * length, chunks undone, or to the connection's end.
 *
 * @par Thread Safety
 * A `bmds_http_t` is used by one thread at a time; separate ones are independent.
 */

#ifndef BMDS_HTTPLIB_H
#define BMDS_HTTPLIB_H

#include <stdint.h>
#include <bmds/sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An open request and its answer. */
typedef struct bmds_http bmds_http_t;

/** @brief `bmds_http_request_t::flags`. */
#define BMDS_HTTP_NO_REDIRECTS 1u /**< A redirect is the answer, not followed. */
#define BMDS_HTTP_INSECURE 2u     /**< `https:`'s certificate isn't checked (`BMDS_TLS_NO_VERIFY`). */

/** @brief Redirects followed when `max_redirects` is 0. */
#define BMDS_HTTP_DEFAULT_REDIRECTS 5u

/** @brief `timeout_ms` 0's wait: 30 s. */
#define BMDS_HTTP_DEFAULT_TIMEOUT_MS 30000

/** @brief A request (`bmds_http_open`). Read during the call only. */
typedef struct bmds_http_request_t {
                uint32_t                 size;          /**< `sizeof(bmds_http_request_t)`. */
                uint32_t                 flags;         /**< `BMDS_HTTP_*`; others `-BMDS_EINVAL`. */
                const char              *method;        /**< `"GET"`, `"POST"`, `"HEAD"`...; NULL: `GET`. */
                const char              *url;           /**< `http://` or `https://`. */
                const char              *headers;       /**< More header lines, each `"Name: value\r\n"`; NULL: none. */
                const void              *body;          /**< Sent with `Content-Length`; NULL: none. */
                uint32_t                 body_size;     /**< Its bytes. */
                const char              *user_agent;    /**< NULL: `"BMDS"`. */
                uint32_t                 max_redirects; /**< 0: `BMDS_HTTP_DEFAULT_REDIRECTS`. */
                int32_t                  timeout_ms;    /**< Each connect and wait; 0: 30 s; negative: forever. */
                uint64_t                 range_from;    /**< `Range: bytes=N-` (to resume); 0: all. */
                const bmds_tls_config_t *tls; /**< `https:`'s more roots, client certificate, ALPN; NULL: none. */
} bmds_http_request_t;

/**
 * @brief Sends @p request and reads its answer's head, following redirects.
 * @param out Set to the handle in every case but `-BMDS_ENOMEM` before it exists (NULL then):
 *        its `bmds_http_error()` says what went wrong; close it with `bmds_http_close()`.
 * @return 0 (any status: see `bmds_http_status()`); `-BMDS_EINVAL` (the struct, not an http: or
 *         https: URL, the method, a header line), `-BMDS_ENOENT` (no such host), the connection's
 *         errors (`-BMDS_ECONNREFUSED`, `-BMDS_ETIMEDOUT`...), TLS's (`-BMDS_EKEYREJECTED`: the
 *         certificate), `-BMDS_EPROTO` (not HTTP, too many redirects), `-BMDS_ENOMEM`.
 * @blocking
 */
int32_t bmds_http_open(const bmds_http_request_t *request, bmds_http_t **out);

/** @brief The answer's status (200, 404...); 0 if `bmds_http_open()` failed. */
int32_t bmds_http_status(const bmds_http_t *http);

/** @brief The status line's words ("OK", "Not Found"); "" if none. */
const char *bmds_http_reason(const bmds_http_t *http);

/** @brief The URL that answered (after redirects). */
const char *bmds_http_url(const bmds_http_t *http);

/** @brief The answer's header @p name (any case; the first of several), or NULL. Valid until close. */
const char *bmds_http_header(const bmds_http_t *http, const char *name);

/**
 * @brief The answer's header number @p index, in the order sent.
 * @return 0, or `-BMDS_ENOENT` past the last.
 */
int32_t bmds_http_header_at(const bmds_http_t *http, uint32_t index, const char **name, const char **value);

/** @brief The body's length (`Content-Length`), or -1 when not said (chunks, or to the end). */
int64_t bmds_http_length(const bmds_http_t *http);

/**
 * @brief Reads the body: up to @p size bytes into @p buf.
 * @return Bytes; 0 at its end; `-BMDS_EPROTO` (cut short, or not HTTP), `-BMDS_ETIMEDOUT`, the
 *         connection's errors.
 * @blocking
 */
int32_t bmds_http_read(bmds_http_t *http, void *buf, uint32_t size);

/** @brief `https:`'s session and its server's certificate. @return 0, or `-BMDS_EINVAL` (not https). */
int32_t bmds_http_tls_info(const bmds_http_t *http, bmds_tls_info_t *info);

/** @brief What went wrong, in words with the URL ("" if nothing). */
const char *bmds_http_error(const bmds_http_t *http);

/** @brief Ends the connection and frees @p http (NULL: nothing). */
void bmds_http_close(bmds_http_t *http);

#ifdef __cplusplus
}
#endif

#endif /* BMDS_HTTPLIB_H */
