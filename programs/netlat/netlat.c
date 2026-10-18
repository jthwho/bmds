/**
 * @file      netlat.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * netlat: real-time network latency (devplan/network.md §11, §30i). Sends
 * AES67-sized datagrams at a fixed interval to a reflector
 * (tools/netlat-reflector), stamped in hardware both ways, and reports
 * what each part of the path took: the sender's wake, the departure against
 * an even grid, the round trip, and the delivery from the driver to the
 * program. With -l it loads the link with TCP both ways meanwhile; with -r
 * the sender and the receiver are real-time threads (SCHED_FIFO), the load not.
 *
 * Only public interfaces: musl's sockets with SO_TIMESTAMPING and the error
 * queue, BMDS_SO_FASTPATH, and pthreads' scheduling.
 *
 * `netlat --bench KEY=VALUE...` is `bench`'s net.latency (devplan/bench.md §12, netlat.bench):
 * the same run, its stages' percentiles as `@bench` lines (bmds/benchlib.h); no peer, skipped.
 */

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <bmds/benchlib.h>
#include <bmds/sys.h>

/* Linux's SOF_TIMESTAMPING_* (musl has no <linux/net_tstamp.h>). */
enum {
        TxHardware = 1 << 0,
        RxHardware = 1 << 2,
        RxSoftware = 1 << 3,
        ReportSoftware = 1 << 4,
        ReportRawHardware = 1 << 6,
        OptId = 1 << 7,
        OptTsOnly = 1 << 11,
};

#define MAGIC 0x6e6c6174u /* "nlat" */
#define MAX_PAYLOAD 1472

/* A datagram's start: the reflector sends it back as it came. */
struct probe {
                uint32_t magic;
                uint32_t sequence;
                int64_t  intended_ns; /* CLOCK_MONOTONIC */
};

/* What became of each datagram (0: never). */
struct record {
                int64_t intended_ns; /* CLOCK_MONOTONIC: when it was to go */
                int64_t woke_ns;     /* CLOCK_MONOTONIC: when the sender woke for it */
                int64_t departed_ns; /* the 1588 clock: its hardware departure */
                int64_t back_ns;     /* the 1588 clock: the reflection's hardware arrival */
                int64_t driver_ns;   /* CLOCK_REALTIME: the reflection given to its socket by the driver */
                int64_t taken_ns;    /* CLOCK_REALTIME: the reflection in this program's hands */
                uint8_t replies;
};

struct options {
                const char        *peer;
                struct sockaddr_in to;
                int                port;
                int                interval_us;
                int                size;
                int                seconds;
                int                dscp;
                int                fast;
                int                load;
                int                rt_priority; /* SCHED_FIFO's, 0: none */
};

static struct options opt = {.port = 5004, .interval_us = 1000, .size = 300, .seconds = 10, .dscp = 46, .fast = 1};
static struct record *records;
static uint32_t      *sequence_of; /* OPT_ID's count (a datagram sent) to its sequence number */
static uint32_t       sent_ids;    /* sequence_of's entries written (atomics) */
static uint32_t       count;
static volatile int   stopping;
static uint32_t       reordered, strays;
static int            sock = -1;
static int            load_socks[2] = {-1, -1}; /* shut down at the end: a stalled peer can't hold them */

static int64_t clock_ns(clockid_t c) {
        struct timespec t;
        clock_gettime(c, &t);
        return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int64_t stamp_ns(const struct timespec *t) {
        return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

/* SCM_TIMESTAMPING's three: software, (none), hardware. */
static int stamps_of(struct msghdr *m, int64_t *software, int64_t *hardware, uint32_t *id) {
        int found = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c != NULL; c = CMSG_NXTHDR(m, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING &&
                    c->cmsg_len >= CMSG_LEN(3 * sizeof(struct timespec))) {
                        struct timespec ts[3];
                        memcpy(ts, CMSG_DATA(c), sizeof(ts));
                        *software = stamp_ns(&ts[0]);
                        *hardware = stamp_ns(&ts[2]);
                        found = 1;
                }
                if (id != NULL && c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR &&
                    c->cmsg_len >= CMSG_LEN(16)) {
                        uint32_t e[4];
                        memcpy(e, CMSG_DATA(c), sizeof(e));
                        *id = e[3]; /* ee_data: OPT_ID's count */
                }
        }
        return found;
}

/* The reflections and the send stamps, as they come. */
static void *receiver(void *arg) {
        (void)arg;
        char     buf[MAX_PAYLOAD + 64];
        char     control[256];
        uint32_t highest = 0;
        int      any = 0;
        while (!stopping) {
                struct pollfd p = {.fd = sock, .events = POLLIN | POLLPRI};
                if (poll(&p, 1, 200) <= 0) continue;
                struct iovec  iov = {buf, sizeof(buf)};
                struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control};
                if (p.revents & (POLLPRI | POLLERR)) {
                        for (;;) {
                                m.msg_controllen = sizeof(control);
                                if (recvmsg(sock, &m, MSG_ERRQUEUE | MSG_DONTWAIT) < 0) break;
                                int64_t  software = 0, hardware = 0;
                                uint32_t id = UINT32_MAX;
                                if (stamps_of(&m, &software, &hardware, &id) &&
                                    id < __atomic_load_n(&sent_ids, __ATOMIC_ACQUIRE))
                                        records[sequence_of[id]].departed_ns = hardware;
                        }
                }
                if (!(p.revents & POLLIN)) continue;
                for (;;) {
                        m.msg_controllen = sizeof(control);
                        iov.iov_len = sizeof(buf);
                        const ssize_t n = recvmsg(sock, &m, MSG_DONTWAIT);
                        if (n < 0) break;
                        const int64_t taken = clock_ns(CLOCK_REALTIME);
                        struct probe  pr;
                        if (n < (ssize_t)sizeof(pr)) {
                                ++strays;
                                continue;
                        }
                        memcpy(&pr, buf, sizeof(pr));
                        if (pr.magic != MAGIC || pr.sequence >= count) {
                                ++strays;
                                continue;
                        }
                        struct record *r = &records[pr.sequence];
                        if (r->replies++ == 0) {
                                int64_t software = 0, hardware = 0;
                                (void)stamps_of(&m, &software, &hardware, NULL);
                                r->back_ns = hardware;
                                r->driver_ns = software;
                                r->taken_ns = taken;
                        }
                        if (any && pr.sequence < highest) ++reordered;
                        if (!any || pr.sequence > highest) highest = pr.sequence;
                        any = 1;
                }
        }
        return NULL;
}

/* TCP both ways through the reflector's sink (port + 1) and source (port + 2). */
static void *loader(void *arg) {
        const int          sending = arg != NULL;
        struct sockaddr_in a = opt.to;
        a.sin_port = htons((uint16_t)(opt.port + (sending ? 1 : 2)));
        const int t = socket(AF_INET, SOCK_STREAM, 0);
        if (t < 0 || connect(t, (struct sockaddr *)&a, sizeof(a)) != 0) {
                fprintf(stderr, "netlat: load: can't connect to port %d: %s\n", opt.port + (sending ? 1 : 2),
                        strerror(errno));
                if (t >= 0) close(t);
                return NULL;
        }
        __atomic_store_n(&load_socks[sending], t, __ATOMIC_RELEASE);
        static char blocks[2][16384];
        char       *block = blocks[sending];
        while (!stopping) {
                const ssize_t n =
                        sending ? send(t, block, sizeof(blocks[0]), MSG_NOSIGNAL) : read(t, block, sizeof(blocks[0]));
                if (n <= 0) break;
        }
        return NULL; /* main closes it */
}

static int compare(const void *a, const void *b) {
        const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
        return x < y ? -1 : x > y;
}

/* `--bench`: each stage's figures as @bench lines too. */
static int bench_mode;

/* min, the percentiles, max (µs) of @p n values in @p v (sorted here); as @p metric's
 * `_p50_us` and the rest under `--bench`. */
static void report(const char *what, const char *metric, int64_t *v, size_t n) {
        if (n == 0) {
                printf("  %-26s none\n", what);
                return;
        }
        qsort(v, n, sizeof(v[0]), compare);
        double mean = 0;
        for (size_t i = 0; i < n; ++i) mean += (double)v[i];
        mean /= (double)n;
#define AT(q) ((double)v[(size_t)((double)(n - 1) * (q))] / 1000.0)
        printf("  %-26s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n", what, (double)v[0] / 1000.0, mean / 1000.0, AT(0.5),
               AT(0.99), AT(0.999), (double)v[n - 1] / 1000.0);
        if (bench_mode) {
                static const struct {
                                const char *suffix;
                                double      q;
                } at[] = {{"p50", 0.5}, {"p99", 0.99}, {"p999", 0.999}};
                char name[64];
                for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); ++i) {
                        snprintf(name, sizeof(name), "%s_%s_us", metric, at[i].suffix);
                        bmds_bench_result(name, AT(at[i].q), "us", BMDS_BENCH_LOWER);
                }
                snprintf(name, sizeof(name), "%s_max_us", metric);
                bmds_bench_result(name, (double)v[n - 1] / 1000.0, "us", BMDS_BENCH_LOWER);
        }
#undef AT
        return;
}

/* The departures' distance from the even grid that fits them best (least squares on the
 * sequence numbers: the 1588 clock and the sender's run at their own rates), less the least. */
static size_t departures(int64_t *out) {
        double  sx = 0, sy = 0, sxx = 0, sxy = 0;
        size_t  n = 0;
        int64_t base = 0;
        for (uint32_t i = 0; i < count; ++i) {
                if (records[i].departed_ns == 0) continue;
                if (n == 0) base = records[i].departed_ns;
                const double x = i, y = (double)(records[i].departed_ns - base);
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
                ++n;
        }
        if (n < 2) return 0;
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        const double offset = (sy - slope * sx) / n;
        size_t       k = 0;
        int64_t      least = INT64_MAX;
        for (uint32_t i = 0; i < count; ++i) {
                if (records[i].departed_ns == 0) continue;
                out[k] = (int64_t)llround((double)(records[i].departed_ns - base) - (offset + slope * i));
                if (out[k] < least) least = out[k];
                ++k;
        }
        for (size_t i = 0; i < k; ++i) out[i] -= least;
        return k;
}

static int usage(void) {
        fprintf(stderr, "usage: netlat [-i US] [-s BYTES] [-t SECONDS] [-p PORT] [-d DSCP] [-r PRIO] [-n] [-l] PEER\n"
                        "  -i  the interval (us; 1000)       -s  the payload (bytes; 300)\n"
                        "  -t  how long (s; 10)              -p  the reflector's port (5004)\n"
                        "  -d  the DSCP (46, EF)             -n  not the fast path\n"
                        "  -l  load: TCP both ways through the reflector meanwhile\n"
                        "  -r  the sender and receiver real-time, SCHED_FIFO at PRIO (1-99)\n");
        return 2;
}

/* `--bench KEY=VALUE...`: the options from `bench`'s parameters. */
static int bench_options(int argc, char **argv) {
        bmds_bench_t b;
        bmds_bench_init(&b, argc, argv);
        bench_mode = 1;
        opt.peer = bmds_bench_param(&b, "peer", "");
        if (*opt.peer == '\0') {
                printf("no peer: give peer=ADDRESS, a wired host running tools/netlat-reflector\n");
                return BMDS_BENCH_SKIPPED;
        }
        opt.seconds = (int)bmds_bench_param_long(&b, "seconds", 5);
        opt.interval_us = (int)bmds_bench_param_long(&b, "interval", 1000);
        opt.size = (int)bmds_bench_param_long(&b, "size", 300);
        opt.port = (int)bmds_bench_param_long(&b, "port", 5004);
        opt.fast = bmds_bench_param_long(&b, "fast", 1) != 0;
        opt.load = bmds_bench_param_long(&b, "load", 0) != 0;
        opt.rt_priority = (int)bmds_bench_param_long(&b, "rt", 0);
        return 0;
}

int main(int argc, char **argv) {
        int c;
        if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
                const int err = bench_options(argc, argv);
                if (err != 0) return err;
                optind = argc - 1; /* the peer, below */
                argv[optind] = (char *)opt.peer;
        }
        while (!bench_mode && (c = getopt(argc, argv, "i:s:t:p:d:r:nl")) != -1) {
                switch (c) {
                        case 'i': opt.interval_us = atoi(optarg); break;
                        case 's': opt.size = atoi(optarg); break;
                        case 't': opt.seconds = atoi(optarg); break;
                        case 'p': opt.port = atoi(optarg); break;
                        case 'd': opt.dscp = atoi(optarg); break;
                        case 'n': opt.fast = 0; break;
                        case 'l': opt.load = 1; break;
                        case 'r': opt.rt_priority = atoi(optarg); break;
                        default: return usage();
                }
        }
        if (optind + 1 != argc || opt.interval_us < 100 || opt.seconds < 1 || opt.dscp < 0 || opt.dscp > 63 ||
            opt.size < (int)sizeof(struct probe) || opt.size > MAX_PAYLOAD || opt.port < 1 || opt.port > 65533 ||
            opt.rt_priority < 0 || opt.rt_priority > 99)
                return usage();
        opt.peer = argv[optind];
        opt.to.sin_family = AF_INET;
        opt.to.sin_port = htons((uint16_t)opt.port);
        if (inet_aton(opt.peer, &opt.to.sin_addr) == 0) {
                fprintf(stderr, "netlat: %s: not an IPv4 address\n", opt.peer);
                return 2;
        }
        count = (uint32_t)((int64_t)opt.seconds * 1000000 / opt.interval_us);
        records = calloc(count, sizeof(*records));
        sequence_of = calloc(count, sizeof(*sequence_of));
        int64_t *values = calloc(count, sizeof(*values));
        if (records == NULL || sequence_of == NULL || values == NULL) {
                fprintf(stderr, "netlat: no memory for %u records\n", (unsigned)count);
                return 1;
        }

        sock = socket(AF_INET, SOCK_DGRAM, 0);
        int v = opt.dscp << 2;
        if (sock < 0 || setsockopt(sock, IPPROTO_IP, IP_TOS, &v, sizeof(v)) != 0) {
                perror("netlat: socket");
                return 1;
        }
        v = TxHardware | RxHardware | RxSoftware | ReportSoftware | ReportRawHardware | OptId | OptTsOnly;
        if (setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPING, &v, sizeof(v)) != 0) {
                perror("netlat: SO_TIMESTAMPING");
                return 1;
        }
        v = 1;
        (void)setsockopt(sock, SOL_SOCKET, SO_SELECT_ERR_QUEUE, &v, sizeof(v));
        if (opt.fast) {
                bmds_fastpath_t f = {.size = sizeof(f), .packet_us = (uint32_t)opt.interval_us, .depth = 4};
                if (setsockopt(sock, BMDS_SOL_BMDS, BMDS_SO_FASTPATH, &f, sizeof(f)) != 0) {
                        perror("netlat: BMDS_SO_FASTPATH");
                        return 1;
                }
        }
        if (connect(sock, (struct sockaddr *)&opt.to, sizeof(opt.to)) != 0) {
                perror("netlat: connect");
                return 1;
        }

        /* The load first, at the normal priority; then the receiver and this thread real-time. */
        pthread_t rx, load[2];
        if (opt.load) {
                pthread_create(&load[0], NULL, loader, (void *)1);
                pthread_create(&load[1], NULL, loader, NULL);
        }
        pthread_attr_t     attr;
        struct sched_param sp = {.sched_priority = opt.rt_priority};
        pthread_attr_init(&attr);
        if (opt.rt_priority > 0) {
                pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
                pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
                pthread_attr_setschedparam(&attr, &sp);
        }
        int err = pthread_create(&rx, &attr, receiver, NULL);
        pthread_attr_destroy(&attr);
        if (err == 0 && opt.rt_priority > 0) err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (err != 0) {
                fprintf(stderr, "netlat: real-time threads: %s\n", strerror(err));
                return 1;
        }
        printf("netlat: %u datagrams of %d bytes every %d us to %s:%d, %s%s", (unsigned)count, opt.size,
               opt.interval_us, opt.peer, opt.port, opt.fast ? "fast path" : "through lwIP",
               opt.load ? ", TCP both ways meanwhile" : "");
        if (opt.rt_priority > 0) printf(", SCHED_FIFO %d", opt.rt_priority);
        printf("\n");

        /* The sender: at an even pace on the monotonic clock (absolute deadlines). */
        static char   payload[MAX_PAYLOAD];
        uint32_t      sent = 0, failed = 0;
        const int64_t start = clock_ns(CLOCK_MONOTONIC) + 100000000; /* 100 ms on: the threads ready */
        for (uint32_t i = 0; i < count; ++i) {
                const int64_t   due = start + (int64_t)i * opt.interval_us * 1000;
                struct timespec at = {.tv_sec = (time_t)(due / 1000000000), .tv_nsec = (long)(due % 1000000000)};
                while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL) == EINTR) {}
                records[i].intended_ns = due;
                records[i].woke_ns = clock_ns(CLOCK_MONOTONIC);
                struct probe pr = {.magic = MAGIC, .sequence = i, .intended_ns = due};
                memcpy(payload, &pr, sizeof(pr));
                /* Its number is known before it goes: its stamp may come before send() returns. A
                 * datagram not sent takes none, and the next one has it. */
                sequence_of[sent] = i;
                __atomic_store_n(&sent_ids, sent + 1, __ATOMIC_RELEASE);
                if (send(sock, payload, (size_t)opt.size, 0) == opt.size)
                        ++sent;
                else
                        ++failed;
        }
        /* The last reflections and stamps. */
        const struct timespec settle = {.tv_sec = 0, .tv_nsec = 300000000};
        nanosleep(&settle, NULL);
        stopping = 1;
        pthread_join(rx, NULL);
        if (opt.load) {
                for (int i = 0; i < 2; ++i) {
                        const int t = __atomic_load_n(&load_socks[i], __ATOMIC_ACQUIRE);
                        if (t >= 0) shutdown(t, SHUT_RDWR);
                }
                pthread_join(load[0], NULL);
                pthread_join(load[1], NULL);
                for (int i = 0; i < 2; ++i) {
                        if (load_socks[i] >= 0) close(load_socks[i]);
                }
        }
        close(sock);

        uint32_t back = 0, stamped = 0, duplicated = 0;
        for (uint32_t i = 0; i < count; ++i) {
                back += records[i].replies != 0;
                stamped += records[i].departed_ns != 0;
                duplicated += records[i].replies > 1;
        }
        printf("sent %u (%u failed), %u stamped; %u back, %u lost, %u reordered, %u duplicated, %u strays\n",
               (unsigned)sent, (unsigned)failed, (unsigned)stamped, (unsigned)back, (unsigned)(sent - back),
               (unsigned)reordered, (unsigned)duplicated, (unsigned)strays);
        printf("  %-26s %8s %8s %8s %8s %8s %8s  (us)\n", "", "min", "mean", "50%", "99%", "99.9%", "max");
        size_t n = 0;
        for (uint32_t i = 0; i < count; ++i) values[n++] = records[i].woke_ns - records[i].intended_ns;
        report("sender's wake, late by", "wake_late", values, n);
        n = departures(values);
        report("departure off the grid", "departure", values, n);
        n = 0;
        for (uint32_t i = 0; i < count; ++i) {
                if (records[i].departed_ns != 0 && records[i].back_ns != 0)
                        values[n++] = records[i].back_ns - records[i].departed_ns;
        }
        report("round trip (hardware)", "round_trip", values, n);
        n = 0;
        for (uint32_t i = 0; i < count; ++i) {
                if (records[i].driver_ns != 0) values[n++] = records[i].taken_ns - records[i].driver_ns;
        }
        report("driver to program", "delivery", values, n);
        free(values);
        free(sequence_of);
        free(records);
        if (bench_mode) {
                bmds_bench_result("lost", (double)(sent - back), "datagrams", BMDS_BENCH_LOWER);
                bmds_bench_result("failed", (double)failed, "datagrams", BMDS_BENCH_LOWER);
                return 0; /* losses are a figure, not a failure */
        }
        return sent == back && failed == 0 ? 0 : 1;
}
