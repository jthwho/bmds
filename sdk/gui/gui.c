/**
 * @file      gui.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgui (bmds/gui.h, devplan/guiprograms.md §4a, §8): the GUI protocol's client. Portable
 * C99: only the transport is BMDS's (gui_open); a host build (BMDS_GUI_HOST) takes a connected
 * fd. Little-endian, every field 4-aligned, as bmds/guiprotocol.h's codec writes and reads.
 */

#include <bmds/gui.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef BMDS_GUI_HOST
#include <bmds/sys.h>
#endif

#define OP_HELLO 0u
#define OP_INTERN_NAMES 1u
#define OP_NAME_OF 2u
#define OP_DESCRIBE 3u
#define OP_CREATE 4u
#define OP_DESTROY 5u
#define OP_SET 6u
#define OP_GET 7u
#define OP_CALL 8u
#define OP_LISTEN 9u
#define OP_SYNC 12u
#define OP_PUT_IMAGE 10u
#define OP_PUT_PIXELS 11u
#define WANT_RESULT 1u

#define KIND_REPLY 1u
#define KIND_EVENT 2u
#define KIND_ERROR 3u

#define MAJOR 1u
#define MINOR 0u
#define MAX_MESSAGE (256u * 1024u)
#define FLUSH_AT (32u * 1024u) /* buffered requests sent once this many bytes wait */

typedef struct {
                char    *name;
                uint32_t atom;
} atom_entry_t;

typedef struct {
                uint8_t *data;
                uint32_t length;
} message_t;

struct bmds_gui {
                int               fd;
                int               broken;
                uint8_t          *out;
                size_t            out_length, out_capacity;
                size_t            request_start;
                uint8_t          *in;
                size_t            in_length, in_capacity;
                uint32_t          sequence; /* requests sent so far */
                uint32_t          id_base, id_mask, id_next;
                atom_entry_t     *atoms;
                size_t            atom_count, atom_capacity;
                message_t        *queue; /* events read while waiting for an answer */
                size_t            queue_head, queue_length, queue_capacity;
                uint8_t          *current; /* the message the caller's values point into */
                uint8_t          *setup;
                uint32_t          setup_length;
                bmds_gui_error_fn on_error;
                void             *error_arg;
                uint8_t          *scratch; /* bmds_gui_strings()'s */
                size_t            scratch_capacity;
};

/* ---- Writing */

static int reserve(uint8_t **buf, size_t *capacity, size_t needed) {
        if (needed <= *capacity) return 0;
        size_t n = *capacity != 0 ? *capacity : 1024;
        while (n < needed) n *= 2;
        uint8_t *grown = (uint8_t *)realloc(*buf, n);
        if (grown == NULL) {
                errno = ENOMEM;
                return -1;
        }
        *buf = grown;
        *capacity = n;
        return 0;
}

static int put(bmds_gui_t *g, const void *p, size_t n) {
        if (reserve(&g->out, &g->out_capacity, g->out_length + n) != 0) return -1;
        memcpy(g->out + g->out_length, p, n);
        g->out_length += n;
        return 0;
}

static int put16(bmds_gui_t *g, uint32_t v) {
        const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        return put(g, b, 2);
}

static int put32(bmds_gui_t *g, uint32_t v) {
        const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        return put(g, b, 4);
}

static int put_bytes(bmds_gui_t *g, const void *p, size_t n) {
        static const uint8_t zero[3] = {0, 0, 0};
        if (put(g, p, n) != 0) return -1;
        return put(g, zero, ((n + 3) & ~(size_t)3) - n);
}

static int put_string(bmds_gui_t *g, const char *s, size_t n) {
        if (put32(g, (uint32_t)n) != 0) return -1;
        return put_bytes(g, s, n);
}

static int put_element(bmds_gui_t *g, const bmds_gui_value_t *v) {
        switch (v->tag) {
                case BMDS_GUI_NONE: return 0;
                case BMDS_GUI_INT:
                case BMDS_GUI_BOOL:
                case BMDS_GUI_ATOM:
                case BMDS_GUI_ID: return put32(g, (uint32_t)v->number);
                case BMDS_GUI_LONG:
                        if (put32(g, (uint32_t)(uint64_t)v->number) != 0) return -1;
                        return put32(g, (uint32_t)((uint64_t)v->number >> 32));
                case BMDS_GUI_STRING: return put_string(g, v->text != NULL ? v->text : "", v->length);
                case BMDS_GUI_RECT:
                        if (put32(g, (uint32_t)v->x) != 0 || put32(g, (uint32_t)v->y) != 0 || put32(g, v->width) != 0)
                                return -1;
                        return put32(g, v->height);
                case BMDS_GUI_COLOR:
                        if (put32(g, (uint32_t)v->number & 0xffffffu) != 0) return -1;
                        return put32(g, v->role);
                case BMDS_GUI_LIST:
                        if (put32(g, v->elements) != 0 || put32(g, v->count) != 0) return -1;
                        return put_bytes(g, v->items, v->items_size);
                default: errno = EINVAL; return -1;
        }
}

static int put_value(bmds_gui_t *g, const bmds_gui_value_t *v) {
        if (put32(g, v->tag) != 0) return -1;
        return put_element(g, v);
}

static int begin(bmds_gui_t *g, uint32_t opcode, uint32_t flags) {
        if (g->broken) {
                errno = EPIPE;
                return -1;
        }
        g->request_start = g->out_length;
        if (put16(g, opcode) != 0 || put16(g, flags) != 0) return -1;
        return put32(g, 0);
}

/* The request's length set; it counts as sent (its sequence). */
static int end(bmds_gui_t *g) {
        const size_t   length = g->out_length - g->request_start;
        uint8_t *const p = g->out + g->request_start + 4;
        if (length > MAX_MESSAGE) {
                g->out_length = g->request_start;
                errno = E2BIG;
                return -1;
        }
        p[0] = (uint8_t)length;
        p[1] = (uint8_t)(length >> 8);
        p[2] = (uint8_t)(length >> 16);
        p[3] = (uint8_t)(length >> 24);
        ++g->sequence;
        if (g->out_length >= FLUSH_AT) return bmds_gui_flush(g);
        return 0;
}

int bmds_gui_flush(bmds_gui_t *g) {
        size_t done = 0;
        while (done < g->out_length) {
                const ssize_t n = write(g->fd, g->out + done, g->out_length - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                        g->broken = 1;
                        g->out_length = 0;
                        errno = EPIPE;
                        return -1;
                }
                done += (size_t)n;
        }
        g->out_length = 0;
        return 0;
}

/* ---- Reading */

typedef struct {
                const uint8_t *p;
                size_t         length, at;
                int            ok;
} reader_t;

static int fits(reader_t *r, size_t n) {
        if (!r->ok || n > r->length - r->at) {
                r->ok = 0;
                return 0;
        }
        return 1;
}

static uint32_t get32(reader_t *r) {
        if (!fits(r, 4)) return 0;
        const uint8_t *b = r->p + r->at;
        r->at += 4;
        return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static const uint8_t *get_bytes(reader_t *r, size_t n) {
        const size_t padded = (n + 3) & ~(size_t)3;
        if (!fits(r, padded)) return NULL;
        const uint8_t *p = r->p + r->at;
        r->at += padded;
        return p;
}

static int get_element(reader_t *r, uint32_t tag, bmds_gui_value_t *v) {
        memset(v, 0, sizeof(*v));
        v->tag = tag;
        switch (tag) {
                case BMDS_GUI_NONE: break;
                case BMDS_GUI_INT: v->number = (int32_t)get32(r); break;
                case BMDS_GUI_BOOL:
                case BMDS_GUI_ATOM:
                case BMDS_GUI_ID: v->number = get32(r); break;
                case BMDS_GUI_LONG: {
                        const uint64_t low = get32(r);
                        v->number = (int64_t)(low | (uint64_t)get32(r) << 32);
                        break;
                }
                case BMDS_GUI_STRING:
                        v->length = get32(r);
                        v->text = (const char *)get_bytes(r, v->length);
                        break;
                case BMDS_GUI_RECT:
                        v->x = (int32_t)get32(r);
                        v->y = (int32_t)get32(r);
                        v->width = get32(r);
                        v->height = get32(r);
                        break;
                case BMDS_GUI_COLOR:
                        v->number = get32(r);
                        v->role = get32(r);
                        break;
                case BMDS_GUI_LIST: {
                        v->elements = get32(r);
                        v->count = get32(r);
                        if (v->elements == BMDS_GUI_LIST || v->elements > BMDS_GUI_COLOR) {
                                r->ok = 0;
                                break;
                        }
                        const size_t     start = r->at;
                        bmds_gui_value_t e;
                        for (uint32_t i = 0; i < v->count && r->ok; ++i) get_element(r, v->elements, &e);
                        v->items = r->p + start;
                        v->items_size = (uint32_t)(r->at - start);
                        break;
                }
                default: r->ok = 0; break;
        }
        return r->ok ? 0 : -1;
}

static int get_value(reader_t *r, bmds_gui_value_t *v) {
        const uint32_t tag = get32(r);
        if (!r->ok || tag > BMDS_GUI_LIST) {
                r->ok = 0;
                return -1;
        }
        return get_element(r, tag, v);
}

/* The next whole message from the service, its bytes the caller's (free()), waiting at most
   timeout_ms (-1: for ever). 1: one; 0: none in time; -1: errno. */
static int read_message(bmds_gui_t *g, int timeout_ms, message_t *out) {
        for (;;) {
                if (g->in_length >= 8) {
                        const uint32_t length = (uint32_t)g->in[4] | (uint32_t)g->in[5] << 8 |
                                                (uint32_t)g->in[6] << 16 | (uint32_t)g->in[7] << 24;
                        if (length % 4 != 0 || length < 12 || length > MAX_MESSAGE) {
                                g->broken = 1;
                                errno = EPROTO;
                                return -1;
                        }
                        if (g->in_length >= length) {
                                out->data = (uint8_t *)malloc(length);
                                if (out->data == NULL) {
                                        errno = ENOMEM;
                                        return -1;
                                }
                                memcpy(out->data, g->in, length);
                                out->length = length;
                                memmove(g->in, g->in + length, g->in_length - length);
                                g->in_length -= length;
                                return 1;
                        }
                }
                if (g->broken) {
                        errno = EPIPE;
                        return -1;
                }
                if (timeout_ms >= 0) {
                        struct pollfd p = {g->fd, POLLIN, 0};
                        const int     r = poll(&p, 1, timeout_ms);
                        if (r < 0 && errno == EINTR) continue;
                        if (r < 0) return -1;
                        if (r == 0) return 0;
                }
                if (reserve(&g->in, &g->in_capacity, g->in_length + 4096) != 0) return -1;
                const ssize_t n = read(g->fd, g->in + g->in_length, g->in_capacity - g->in_length);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                        g->broken = 1;
                        errno = EPIPE;
                        return -1;
                }
                g->in_length += (size_t)n;
        }
}

static uint32_t kind_of(const message_t *m) {
        return (uint32_t)m->data[0] | (uint32_t)m->data[1] << 8;
}
static uint32_t code_of(const message_t *m) {
        return (uint32_t)m->data[2] | (uint32_t)m->data[3] << 8;
}
static uint32_t sequence_of(const message_t *m) {
        return (uint32_t)m->data[8] | (uint32_t)m->data[9] << 8 | (uint32_t)m->data[10] << 16 |
               (uint32_t)m->data[11] << 24;
}

static void default_error(void *arg, const bmds_gui_error_t *e) {
        (void)arg;
        fprintf(stderr, "gui: request %u (opcode %u): %s (%u)\n", (unsigned)e->sequence, (unsigned)e->opcode,
                bmds_gui_error_name(e->code), (unsigned)e->bad);
        return;
}

static void tell_error(bmds_gui_t *g, const message_t *m) {
        reader_t         r = {m->data + 12, m->length - 12, 0, 1};
        bmds_gui_error_t e;
        e.sequence = sequence_of(m);
        e.code = code_of(m);
        e.opcode = get32(&r);
        e.bad = get32(&r);
        (g->on_error != NULL ? g->on_error : default_error)(g->error_arg, &e);
        return;
}

static int enqueue(bmds_gui_t *g, message_t m) {
        if (g->queue_length == g->queue_capacity) {
                const size_t n = g->queue_capacity != 0 ? g->queue_capacity * 2 : 16;
                message_t   *grown = (message_t *)malloc(n * sizeof(message_t));
                if (grown == NULL) {
                        free(m.data);
                        errno = ENOMEM;
                        return -1;
                }
                for (size_t i = 0; i < g->queue_length; ++i)
                        grown[i] = g->queue[(g->queue_head + i) % g->queue_capacity];
                free(g->queue);
                g->queue = grown;
                g->queue_capacity = n;
                g->queue_head = 0;
        }
        g->queue[(g->queue_head + g->queue_length) % g->queue_capacity] = m;
        ++g->queue_length;
        return 0;
}

static void set_current(bmds_gui_t *g, uint8_t *data) {
        free(g->current);
        g->current = data;
        return;
}

/* Waits for the answer to the last request sent; events meanwhile are queued, other errors
   told. The answer becomes the current message. 0, or -1 (EINVAL: an error answered it). */
static int answer(bmds_gui_t *g, message_t *out) {
        const uint32_t wanted = g->sequence;
        if (bmds_gui_flush(g) != 0) return -1;
        for (;;) {
                message_t m;
                const int r = read_message(g, -1, &m);
                if (r < 0) return -1;
                const uint32_t kind = kind_of(&m);
                if (kind == KIND_EVENT) {
                        if (enqueue(g, m) != 0) return -1;
                        continue;
                }
                if (kind == KIND_ERROR) {
                        tell_error(g, &m);
                        const int mine = sequence_of(&m) == wanted;
                        free(m.data);
                        if (mine) {
                                errno = EINVAL;
                                return -1;
                        }
                        continue;
                }
                if (kind == KIND_REPLY && sequence_of(&m) == wanted) {
                        set_current(g, m.data);
                        *out = m;
                        return 0;
                }
                free(m.data); /* an answer to nothing asked: dropped */
        }
}

/* ---- Connecting */

bmds_gui_t *bmds_gui_connect_fd(int fd) {
        bmds_gui_t *g = (bmds_gui_t *)calloc(1, sizeof(bmds_gui_t));
        if (g == NULL) {
                close(fd);
                errno = ENOMEM;
                return NULL;
        }
        g->fd = fd;
        static const char magic[8] = {'B', 'M', 'D', 'S', 'G', 'U', 'I', '\0'};
        int               err = begin(g, OP_HELLO, 0);
        if (err == 0) err = put(g, magic, 8);
        if (err == 0) err = put16(g, MAJOR);
        if (err == 0) err = put16(g, MINOR);
        if (err == 0) err = put32(g, 0); /* authentication: none locally */
        if (err == 0) err = put32(g, 0);
        if (err == 0) {
                /* Hello is sequence 0: end() would count it. */
                const size_t length = g->out_length - g->request_start;
                uint8_t     *p = g->out + g->request_start + 4;
                p[0] = (uint8_t)length;
                p[1] = (uint8_t)(length >> 8);
                p[2] = (uint8_t)(length >> 16);
                p[3] = (uint8_t)(length >> 24);
        }
        message_t m = {NULL, 0};
        if (err == 0) err = answer(g, &m);
        if (err != 0) {
                const int e = errno == EINVAL ? EPROTO : errno;
                bmds_gui_close(g);
                errno = e;
                return NULL;
        }
        reader_t       r = {m.data + 12, m.length - 12, 0, 1};
        const uint32_t status = get32(&r);
        const uint32_t versions = get32(&r);
        g->id_base = get32(&r);
        g->id_mask = get32(&r);
        (void)get32(&r); /* the largest message: ours is the protocol's */
        if (!r.ok || status != 0 || (versions & 0xffffu) != MAJOR || g->id_mask == 0) {
                bmds_gui_close(g);
                errno = EPROTO;
                return NULL;
        }
        /* Kept for bmds_gui_display(); the current message is let go. */
        g->setup = m.data;
        g->setup_length = m.length;
        g->current = NULL;
        g->id_next = 1;
        return g;
}

bmds_gui_t *bmds_gui_connect(const char *display) {
        if (display == NULL) display = getenv("BMDS_GUI_DISPLAY");
        if (display == NULL || display[0] == '\0') display = "local";
#ifndef BMDS_GUI_HOST
        if (strcmp(display, "local") == 0) {
                const int32_t fd = bmds_gui_open(0, 0);
                if (fd < 0) {
                        errno = (int)-fd;
                        return NULL;
                }
                return bmds_gui_connect_fd((int)fd);
        }
#endif
        errno = ENOTSUP; /* other machines: devplan/guiprograms.md step 8 */
        return NULL;
}

void bmds_gui_close(bmds_gui_t *g) {
        if (g == NULL) return;
        if (!g->broken) (void)bmds_gui_flush(g);
        close(g->fd);
        for (size_t i = 0; i < g->atom_count; ++i) free(g->atoms[i].name);
        for (size_t i = 0; i < g->queue_length; ++i) free(g->queue[(g->queue_head + i) % g->queue_capacity].data);
        free(g->atoms);
        free(g->queue);
        free(g->out);
        free(g->in);
        free(g->current);
        free(g->setup);
        free(g->scratch);
        free(g);
        return;
}

int bmds_gui_fd(const bmds_gui_t *g) {
        return g->fd;
}

void bmds_gui_on_error(bmds_gui_t *g, bmds_gui_error_fn fn, void *arg) {
        g->on_error = fn;
        g->error_arg = arg;
        return;
}

const char *bmds_gui_error_name(uint32_t code) {
        static const char *const names[] = {"?",        "BadRequest", "BadLength",        "BadId",
                                            "BadName",  "BadClass",   "BadValue",         "BadAccess",
                                            "BadMatch", "NoMem",      "BadImplementation"};
        return code < sizeof(names) / sizeof(names[0]) ? names[code] : "?";
}

/* ---- Names */

static uint32_t cached(const bmds_gui_t *g, const char *name) {
        for (size_t i = 0; i < g->atom_count; ++i)
                if (strcmp(g->atoms[i].name, name) == 0) return g->atoms[i].atom;
        return 0;
}

static int remember(bmds_gui_t *g, const char *name, uint32_t atom) {
        if (g->atom_count == g->atom_capacity) {
                const size_t  n = g->atom_capacity != 0 ? g->atom_capacity * 2 : 32;
                atom_entry_t *grown = (atom_entry_t *)realloc(g->atoms, n * sizeof(atom_entry_t));
                if (grown == NULL) {
                        errno = ENOMEM;
                        return -1;
                }
                g->atoms = grown;
                g->atom_capacity = n;
        }
        const size_t length = strlen(name);
        char        *copy = (char *)malloc(length + 1);
        if (copy == NULL) {
                errno = ENOMEM;
                return -1;
        }
        memcpy(copy, name, length + 1);
        g->atoms[g->atom_count].name = copy;
        g->atoms[g->atom_count].atom = atom;
        ++g->atom_count;
        return 0;
}

int bmds_gui_atoms(bmds_gui_t *g, const char *const *names, uint32_t count, uint32_t *out) {
        uint32_t missing = 0;
        for (uint32_t i = 0; i < count; ++i) {
                out[i] = cached(g, names[i]);
                if (out[i] == 0) ++missing;
        }
        if (missing == 0) return 0;
        if (begin(g, OP_INTERN_NAMES, 0) != 0 || put32(g, missing) != 0) return -1;
        for (uint32_t i = 0; i < count; ++i)
                if (out[i] == 0 && put_string(g, names[i], strlen(names[i])) != 0) return -1;
        if (end(g) != 0) return -1;
        message_t m;
        if (answer(g, &m) != 0) return -1;
        reader_t r = {m.data + 12, m.length - 12, 0, 1};
        if (get32(&r) != missing) {
                errno = EPROTO;
                return -1;
        }
        for (uint32_t i = 0; i < count; ++i) {
                if (out[i] != 0) continue;
                out[i] = get32(&r);
                if (!r.ok || out[i] == 0) {
                        errno = EPROTO;
                        return -1;
                }
                if (remember(g, names[i], out[i]) != 0) return -1;
        }
        return 0;
}

uint32_t bmds_gui_atom(bmds_gui_t *g, const char *name) {
        uint32_t atom = 0;
        return bmds_gui_atoms(g, &name, 1, &atom) == 0 ? atom : 0;
}

const char *bmds_gui_atom_name(bmds_gui_t *g, uint32_t atom) {
        for (size_t i = 0; i < g->atom_count; ++i)
                if (g->atoms[i].atom == atom) return g->atoms[i].name;
        if (begin(g, OP_NAME_OF, 0) != 0 || put32(g, 1) != 0 || put32(g, atom) != 0 || end(g) != 0) return NULL;
        message_t m;
        if (answer(g, &m) != 0) return NULL;
        reader_t       r = {m.data + 12, m.length - 12, 0, 1};
        const uint32_t count = get32(&r);
        const uint32_t length = get32(&r);
        const char    *text = (const char *)get_bytes(&r, length);
        if (!r.ok || count != 1 || length == 0) {
                errno = EPROTO;
                return NULL;
        }
        char *name = (char *)malloc(length + 1);
        if (name == NULL) return NULL;
        memcpy(name, text, length);
        name[length] = '\0';
        const int kept = remember(g, name, atom);
        free(name);
        return kept == 0 ? g->atoms[g->atom_count - 1].name : NULL;
}

uint32_t bmds_gui_new_id(bmds_gui_t *g) {
        if (g->id_next > g->id_mask) return 0;
        return g->id_base | g->id_next++;
}

/* ---- Requests */

static int put_props(bmds_gui_t *g, const bmds_gui_prop_t *props, uint32_t count) {
        if (put32(g, count) != 0) return -1;
        for (uint32_t i = 0; i < count; ++i)
                if (put32(g, props[i].name) != 0 || put_value(g, &props[i].value) != 0) return -1;
        return 0;
}

int bmds_gui_create(bmds_gui_t *g, uint32_t id, uint32_t cls, uint32_t parent, const bmds_gui_prop_t *props,
                    uint32_t count) {
        if (begin(g, OP_CREATE, 0) != 0 || put32(g, id) != 0 || put32(g, cls) != 0 || put32(g, parent) != 0 ||
            put_props(g, props, count) != 0)
                return -1;
        return end(g);
}

int bmds_gui_destroy(bmds_gui_t *g, uint32_t id) {
        if (begin(g, OP_DESTROY, 0) != 0 || put32(g, id) != 0) return -1;
        return end(g);
}

int bmds_gui_set(bmds_gui_t *g, uint32_t id, const bmds_gui_prop_t *props, uint32_t count) {
        if (begin(g, OP_SET, 0) != 0 || put32(g, id) != 0 || put_props(g, props, count) != 0) return -1;
        return end(g);
}

int bmds_gui_get(bmds_gui_t *g, uint32_t id, const uint32_t *names, uint32_t count, bmds_gui_value_t *out) {
        if (begin(g, OP_GET, 0) != 0 || put32(g, id) != 0 || put32(g, count) != 0) return -1;
        for (uint32_t i = 0; i < count; ++i)
                if (put32(g, names[i]) != 0) return -1;
        if (end(g) != 0) return -1;
        message_t m;
        if (answer(g, &m) != 0) return -1;
        reader_t r = {m.data + 12, m.length - 12, 0, 1};
        if (get32(&r) != count) {
                errno = EPROTO;
                return -1;
        }
        for (uint32_t i = 0; i < count; ++i)
                if (get_value(&r, &out[i]) != 0) {
                        errno = EPROTO;
                        return -1;
                }
        return 0;
}

int bmds_gui_put_image(bmds_gui_t *g, uint32_t id, uint32_t width, uint32_t height, const uint32_t *argb) {
        /* In pieces under the largest message: 32768 pixels (128 KB) each. */
        const uint32_t total = width * height, piece = 32768;
        for (uint32_t at = 0; at < total || at == 0; at += piece) {
                const uint32_t n = total - at < piece ? total - at : piece;
                if (begin(g, OP_PUT_IMAGE, 0) != 0 || put32(g, id) != 0 || put32(g, width) != 0 ||
                    put32(g, height) != 0 || put32(g, 1) != 0 || put32(g, at) != 0 || put32(g, n) != 0)
                        return -1;
                /* The wire is little-endian, as the pixels are in memory here. */
                if (n != 0 && put(g, argb + at, (size_t)n * 4) != 0) return -1;
                if (end(g) != 0 || bmds_gui_flush(g) != 0) return -1; /* a piece at a time in memory */
                if (total == 0) break;
        }
        return 0;
}

/* One PutPixels of rows [row, row + rows) of the rectangle: run-length coded if that is smaller. */
static int put_pixel_rows(bmds_gui_t *g, uint32_t canvas, int32_t x, int32_t y, uint32_t width, uint32_t rows,
                          const uint16_t *pixels, uint32_t stride) {
        /* Count the runs first (row by row, runs crossing rows allowed). */
        uint32_t runs = 0, run = 0;
        uint16_t last = 0;
        for (uint32_t r = 0; r < rows; ++r)
                for (uint32_t c = 0; c < width; ++c) {
                        const uint16_t p = pixels[(size_t)r * stride + c];
                        if (run != 0 && p == last && run < 0xffff) {
                                ++run;
                        } else {
                                ++runs;
                                run = 1;
                                last = p;
                        }
                }
        const int      rle = (size_t)runs * 4 < (size_t)width * rows * 2;
        const uint32_t bytes = rle ? runs * 4 : width * rows * 2;
        if (begin(g, OP_PUT_PIXELS, 0) != 0 || put32(g, canvas) != 0 || put32(g, (uint32_t)x) != 0 ||
            put32(g, (uint32_t)y) != 0 || put32(g, width) != 0 || put32(g, rows) != 0 || put32(g, rle ? 2u : 1u) != 0 ||
            put32(g, bytes) != 0)
                return -1;
        if (rle) {
                run = 0;
                for (uint32_t r = 0; r < rows; ++r)
                        for (uint32_t c = 0; c < width; ++c) {
                                const uint16_t p = pixels[(size_t)r * stride + c];
                                if (run != 0 && p == last && run < 0xffff) {
                                        ++run;
                                        continue;
                                }
                                if (run != 0 && (put16(g, run) != 0 || put16(g, last) != 0)) return -1;
                                run = 1;
                                last = p;
                        }
                if (run != 0 && (put16(g, run) != 0 || put16(g, last) != 0)) return -1;
        } else {
                for (uint32_t r = 0; r < rows; ++r)
                        if (put(g, pixels + (size_t)r * stride, (size_t)width * 2) != 0) return -1;
        }
        static const uint8_t zero[3] = {0, 0, 0};
        if (put(g, zero, ((bytes + 3) & ~3u) - bytes) != 0) return -1;
        return end(g);
}

int bmds_gui_put_pixels(bmds_gui_t *g, uint32_t canvas, int32_t x, int32_t y, uint32_t width, uint32_t height,
                        const uint16_t *rgb565, uint32_t stride) {
        if (width == 0 || height == 0) return 0;
        /* In bands under the largest message (raw at most 128 KB a band). */
        uint32_t band = 65536 / width;
        if (band == 0) band = 1;
        for (uint32_t row = 0; row < height; row += band) {
                const uint32_t rows = height - row < band ? height - row : band;
                if (put_pixel_rows(g, canvas, x, y + (int32_t)row, width, rows, rgb565 + (size_t)row * stride,
                                   stride) != 0)
                        return -1;
                if (height > band && bmds_gui_flush(g) != 0) return -1;
        }
        return 0;
}

int bmds_gui_describe(bmds_gui_t *g, uint32_t cls, const uint8_t **body, uint32_t *length) {
        if (begin(g, OP_DESCRIBE, 0) != 0 || put32(g, cls) != 0 || end(g) != 0) return -1;
        message_t m;
        if (answer(g, &m) != 0) return -1;
        *body = m.data + 12;
        *length = m.length - 12;
        return 0;
}

int bmds_gui_call(bmds_gui_t *g, uint32_t id, uint32_t method, const bmds_gui_value_t *args, uint32_t count,
                  bmds_gui_value_t *result) {
        if (begin(g, OP_CALL, result != NULL ? WANT_RESULT : 0) != 0 || put32(g, id) != 0 || put32(g, method) != 0 ||
            put32(g, count) != 0)
                return -1;
        for (uint32_t i = 0; i < count; ++i)
                if (put_value(g, &args[i]) != 0) return -1;
        if (end(g) != 0) return -1;
        if (result == NULL) return 0;
        message_t m;
        if (answer(g, &m) != 0) return -1;
        reader_t r = {m.data + 12, m.length - 12, 0, 1};
        if (get_value(&r, result) != 0) {
                errno = EPROTO;
                return -1;
        }
        return 0;
}

int bmds_gui_listen(bmds_gui_t *g, uint32_t id, const uint32_t *events, uint32_t count, int on) {
        if (begin(g, OP_LISTEN, 0) != 0 || put32(g, id) != 0 || put32(g, on ? 1u : 0u) != 0 || put32(g, count) != 0)
                return -1;
        for (uint32_t i = 0; i < count; ++i)
                if (put32(g, events[i]) != 0) return -1;
        return end(g);
}

int bmds_gui_sync(bmds_gui_t *g) {
        if (begin(g, OP_SYNC, 0) != 0 || end(g) != 0) return -1;
        message_t m;
        return answer(g, &m);
}

/* ---- Events */

static int decode_event(bmds_gui_t *g, message_t m, bmds_gui_event_t *out) {
        set_current(g, m.data);
        reader_t r = {m.data + 12, m.length - 12, 0, 1};
        memset(out, 0, sizeof(*out));
        out->source = get32(&r);
        out->name = get32(&r);
        out->time_ms = get32(&r);
        const uint32_t count = get32(&r);
        for (uint32_t i = 0; i < count && r.ok; ++i) {
                bmds_gui_value_t v;
                if (get_value(&r, &v) != 0) break;
                if (out->count < BMDS_GUI_EVENT_VALUES) out->values[out->count++] = v;
        }
        if (!r.ok) {
                errno = EPROTO;
                return -1;
        }
        return 1;
}

int bmds_gui_next_event(bmds_gui_t *g, bmds_gui_event_t *out, int timeout_ms) {
        if (g->queue_length > 0) {
                const message_t m = g->queue[g->queue_head];
                g->queue_head = (g->queue_head + 1) % g->queue_capacity;
                --g->queue_length;
                return decode_event(g, m, out);
        }
        if (bmds_gui_flush(g) != 0) return -1;
        for (;;) {
                message_t m;
                const int r = read_message(g, timeout_ms, &m);
                if (r <= 0) return r;
                const uint32_t kind = kind_of(&m);
                if (kind == KIND_EVENT) return decode_event(g, m, out);
                if (kind == KIND_ERROR) tell_error(g, &m);
                free(m.data);
        }
}

/* ---- The setup's display, values */

int bmds_gui_display(const bmds_gui_t *g, const char *name, bmds_gui_value_t *out) {
        reader_t r = {g->setup + 12, g->setup_length - 12, 0, 1};
        for (int i = 0; i < 5; ++i) (void)get32(&r); /* status, versions, ids, mask, largest */
        const uint32_t extensions = get32(&r);
        for (uint32_t i = 0; i < extensions && r.ok; ++i) {
                const uint32_t n = get32(&r);
                (void)get_bytes(&r, n);
                (void)get32(&r);
                (void)get32(&r);
        }
        const uint32_t count = get32(&r);
        const size_t   wanted = strlen(name);
        for (uint32_t i = 0; i < count && r.ok; ++i) {
                const uint32_t   n = get32(&r);
                const char      *s = (const char *)get_bytes(&r, n);
                bmds_gui_value_t v;
                if (get_value(&r, &v) != 0) break;
                if (s != NULL && n == wanted && memcmp(s, name, n) == 0) {
                        *out = v;
                        return 0;
                }
        }
        errno = ENOENT;
        return -1;
}

int bmds_gui_list_item(const bmds_gui_value_t *list, uint32_t index, bmds_gui_value_t *out) {
        if (list->tag != BMDS_GUI_LIST || index >= list->count) {
                errno = ERANGE;
                return -1;
        }
        reader_t r = {(const uint8_t *)list->items, list->items_size, 0, 1};
        for (uint32_t i = 0; i <= index; ++i)
                if (get_element(&r, list->elements, out) != 0) {
                        errno = ERANGE;
                        return -1;
                }
        return 0;
}

bmds_gui_value_t bmds_gui_string(const char *text) {
        bmds_gui_value_t r;
        memset(&r, 0, sizeof(r));
        r.tag = BMDS_GUI_STRING;
        r.text = text != NULL ? text : "";
        r.length = (uint32_t)strlen(r.text);
        return r;
}

static bmds_gui_value_t number_value(uint32_t tag, int64_t number) {
        bmds_gui_value_t r;
        memset(&r, 0, sizeof(r));
        r.tag = tag;
        r.number = number;
        return r;
}

bmds_gui_value_t bmds_gui_int(int32_t v) {
        return number_value(BMDS_GUI_INT, v);
}

bmds_gui_value_t bmds_gui_bool(int v) {
        return number_value(BMDS_GUI_BOOL, v ? 1 : 0);
}

bmds_gui_value_t bmds_gui_atom_value(uint32_t atom) {
        return number_value(BMDS_GUI_ATOM, atom);
}

bmds_gui_value_t bmds_gui_id(uint32_t id) {
        return number_value(BMDS_GUI_ID, id);
}

bmds_gui_value_t bmds_gui_long(int64_t v) {
        return number_value(BMDS_GUI_LONG, v);
}

bmds_gui_value_t bmds_gui_rect(int32_t x, int32_t y, uint32_t width, uint32_t height) {
        bmds_gui_value_t r = number_value(BMDS_GUI_RECT, 0);
        r.x = x;
        r.y = y;
        r.width = width;
        r.height = height;
        return r;
}

bmds_gui_value_t bmds_gui_color(uint32_t rgb) {
        return number_value(BMDS_GUI_COLOR, rgb & 0xffffffu);
}

bmds_gui_value_t bmds_gui_strings(bmds_gui_t *g, const char *const *items, uint32_t count) {
        bmds_gui_value_t r = number_value(BMDS_GUI_LIST, 0);
        r.elements = BMDS_GUI_STRING;
        size_t bytes = 0;
        for (uint32_t i = 0; i < count; ++i) bytes += 4 + ((strlen(items[i]) + 3) & ~(size_t)3);
        if (reserve(&g->scratch, &g->scratch_capacity, bytes + 1) != 0) return number_value(BMDS_GUI_NONE, 0);
        size_t at = 0;
        for (uint32_t i = 0; i < count; ++i) {
                const size_t n = strlen(items[i]);
                g->scratch[at] = (uint8_t)n;
                g->scratch[at + 1] = (uint8_t)(n >> 8);
                g->scratch[at + 2] = (uint8_t)(n >> 16);
                g->scratch[at + 3] = (uint8_t)(n >> 24);
                memcpy(g->scratch + at + 4, items[i], n);
                memset(g->scratch + at + 4 + n, 0, ((n + 3) & ~(size_t)3) - n);
                at += 4 + ((n + 3) & ~(size_t)3);
        }
        r.count = count;
        r.items = g->scratch;
        r.items_size = (uint32_t)at;
        return r;
}

uint32_t bmds_gui_new(bmds_gui_t *g, const char *cls, uint32_t parent) {
        const uint32_t atom = bmds_gui_atom(g, cls);
        const uint32_t id = atom != 0 ? bmds_gui_new_id(g) : 0;
        if (id == 0) {
                if (atom != 0) errno = ENOSPC;
                return 0;
        }
        return bmds_gui_create(g, id, atom, parent, NULL, 0) == 0 ? id : 0;
}

int bmds_gui_set_named(bmds_gui_t *g, uint32_t id, const char *name, bmds_gui_value_t value) {
        const uint32_t        atom = bmds_gui_atom(g, name);
        const bmds_gui_prop_t prop = {atom, value};
        return atom != 0 ? bmds_gui_set(g, id, &prop, 1) : -1;
}

int bmds_gui_get_named(bmds_gui_t *g, uint32_t id, const char *name, bmds_gui_value_t *out) {
        const uint32_t atom = bmds_gui_atom(g, name);
        return atom != 0 ? bmds_gui_get(g, id, &atom, 1, out) : -1;
}

int bmds_gui_call_named(bmds_gui_t *g, uint32_t id, const char *name, const bmds_gui_value_t *args, uint32_t count,
                        bmds_gui_value_t *result) {
        const uint32_t atom = bmds_gui_atom(g, name);
        return atom != 0 ? bmds_gui_call(g, id, atom, args, count, result) : -1;
}

int bmds_gui_event_is(bmds_gui_t *g, const bmds_gui_event_t *e, const char *name) {
        const uint32_t atom = cached(g, name);
        return atom != 0 ? e->name == atom : e->name == bmds_gui_atom(g, name);
}
