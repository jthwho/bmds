/**
 * @file      midimon.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * midimon [PORT] | midimon -c NAME: MIDI events as they arrive
 * (devplan/audio.md §21). PORT (midiN, N or a path; /dev/midi, what programs
 * play, `system` (§25), if none) is read; -c makes a port named NAME for
 * others to send to (`midi send`, `midi connect`), shows what reaches it and
 * passes it on from the port's source, a MIDI thru (`keytone -m NAME` plays it). Each line: the event's time in
 * seconds since boot, the gap since the one before, the port it came from,
 * its bytes and what it is. Ctrl+C stops and shows the counts.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/errno.h>
#include <bmds/sys.h>

static volatile sig_atomic_t quit;

static void on_signal(int sig) {
        (void)sig;
        quit = 1;
}

static int usage(void) {
        fprintf(stderr, "usage: midimon [PORT] | midimon -c NAME\n");
        return 2;
}

/* What a message is. */
static void describe(const bmds_midi_event_t *e, char *out, size_t size) {
        const unsigned s = e->data[0], ch = (s & 0x0f) + 1, d1 = e->data[1], d2 = e->data[2];
        out[0] = '\0';
        if (s < 0x80) return; /* a SysEx's middle */
        switch (s & 0xf0) {
                case 0x80: snprintf(out, size, "note off   ch %u  %u  %u", ch, d1, d2); return;
                case 0x90: snprintf(out, size, "note on    ch %u  %u  %u", ch, d1, d2); return;
                case 0xa0: snprintf(out, size, "key press  ch %u  %u  %u", ch, d1, d2); return;
                case 0xb0: snprintf(out, size, "control    ch %u  %u  %u", ch, d1, d2); return;
                case 0xc0: snprintf(out, size, "program    ch %u  %u", ch, d1); return;
                case 0xd0: snprintf(out, size, "pressure   ch %u  %u", ch, d1); return;
                case 0xe0: snprintf(out, size, "pitch bend ch %u  %d", ch, (int)(d1 | d2 << 7) - 8192); return;
                default: break;
        }
        switch (s) {
                case 0xf0: snprintf(out, size, "sysex"); return;
                case 0xf8: snprintf(out, size, "clock"); return;
                case 0xfa: snprintf(out, size, "start"); return;
                case 0xfb: snprintf(out, size, "continue"); return;
                case 0xfc: snprintf(out, size, "stop"); return;
                case 0xfe: snprintf(out, size, "active sensing"); return;
                case 0xff: snprintf(out, size, "reset"); return;
                default: return;
        }
}

int main(int argc, char **argv) {
        const char *create = NULL;
        const char *port = "/dev/midi";
        char        path[32];
        if (argc == 3 && strcmp(argv[1], "-c") == 0) {
                create = argv[2];
        } else if (argc == 2 && argv[1][0] != '-') {
                const char *n = strncmp(argv[1], "midi", 4) == 0 ? argv[1] + 4 : argv[1];
                if (argv[1][0] == '/') {
                        port = argv[1];
                } else {
                        snprintf(path, sizeof(path), "/dev/midi%s", n);
                        port = path;
                }
        } else if (argc != 1) {
                return usage();
        }
        if (!bmds_sys_has(BMDS_SYS_MIDI_CONFIG)) {
                fprintf(stderr, "midimon: this system has no MIDI\n");
                return 1;
        }
        const int fd = open(create != NULL ? "/dev/midinew" : port, create != NULL ? O_RDWR : O_RDONLY);
        if (fd < 0) {
                fprintf(stderr, "midimon: %s: %s\n", create != NULL ? "/dev/midinew" : port,
                        errno == ENOTSUP ? "the port has no source" : strerror(errno));
                return 1;
        }
        bmds_midi_config_t c = {.size = sizeof(c), .flags = BMDS_MIDI_EVENTS, .event_size = sizeof(bmds_midi_event_t)};
        bmds_midi_config_t got = {.size = sizeof(got)};
        if (create != NULL) {
                c.flags |= BMDS_MIDI_CREATE | BMDS_MIDI_SOURCE | BMDS_MIDI_DESTINATION;
                snprintf(c.name, sizeof(c.name), "%s", create);
        }
        const int32_t r = bmds_midi_config(fd, &c, &got);
        if (r < 0) {
                fprintf(stderr, "midimon: %s\n", r == -BMDS_ENOSPC ? "no MIDI port left" : strerror(-r));
                return 1;
        }
        printf("midimon: midi%u, %s%s; Ctrl+C stops\n", (unsigned)got.port, got.name,
               create != NULL ? " (made here: send to it)" : "");
        fflush(stdout);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        int64_t last = 0;
        while (!quit) {
                bmds_midi_event_t ev[16];
                const ssize_t     n = read(fd, ev, sizeof(ev));
                if (n == 0) {
                        printf("midimon: the port went away\n");
                        break;
                }
                if (n < 0) {
                        if (errno == EINTR) continue;
                        fprintf(stderr, "midimon: %s\n", strerror(errno));
                        break;
                }
                for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); ++i) {
                        const bmds_midi_event_t *e = &ev[i];
                        const int64_t            us = e->time_ns / 1000;
                        const int64_t            gap = last == 0 ? 0 : (e->time_ns - last) / 1000;
                        last = e->time_ns;
                        char hex[16] = "";
                        for (unsigned b = 0; b < e->length && b < 5; ++b)
                                snprintf(hex + 3 * b, sizeof(hex) - 3 * b, "%02x ", e->data[b]);
                        char what[40];
                        describe(e, what, sizeof(what));
                        char from[8] = "-";
                        if (e->port != 0xff) snprintf(from, sizeof(from), "midi%u", (unsigned)e->port);
                        printf("%5lld.%06lld  +%8lld us  %-6s  %-15s %s\n", (long long)(us / 1000000),
                               (long long)(us % 1000000), (long long)gap, from, hex, what);
                }
                fflush(stdout);
                if (create != NULL) {
                        /* Passed on: written events are now (time 0). */
                        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); ++i) ev[i].time_ns = 0;
                        if (write(fd, ev, (size_t)n) != n) fprintf(stderr, "midimon: thru: %s\n", strerror(errno));
                }
        }
        bmds_midi_status_t st = {.size = sizeof(st)};
        if (bmds_midi_status(fd, &st) == 0)
                printf("midimon: %llu events read, %u dropped (read too slowly)\n", (unsigned long long)st.fd_events,
                       (unsigned)st.fd_dropped);
        close(fd);
        return 0;
}
