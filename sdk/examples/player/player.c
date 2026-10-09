/**
 * @file      player.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * player FILE: a sound file (WAV, MP3, Ogg Vorbis, FLAC) on /dev/dsp. The
 * firmware decodes it (bmds_audio_decode): the decoded fd reads as PCM, so
 * playing is copying it to an audio fd set up from what it says it is.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

static char buf[16384];

int main(int argc, char **argv) {
        if (argc != 2) {
                fprintf(stderr, "usage: player FILE\n");
                return 2;
        }
        if (!bmds_sys_has(BMDS_SYS_AUDIO_DECODE)) {
                fprintf(stderr, "player: this kernel has no decoders\n");
                return 1;
        }
        const int file = open(argv[1], O_RDONLY);
        if (file < 0) {
                fprintf(stderr, "player: %s: %s\n", argv[1], strerror(errno));
                return 1;
        }
        // The decoder's own format: float for MP3 and Vorbis, the file's for WAV and FLAC.
        const bmds_decode_config_t config = {.size = sizeof(config), .format = BMDS_DECODE_NATIVE};
        const int32_t              pcm = bmds_audio_decode(file, &config);
        close(file); // the decoded fd keeps the file open
        if (pcm < 0) {
                fprintf(stderr, "player: %s: %s\n", argv[1], strerror((int)-pcm));
                return 1;
        }
        bmds_decode_info_t info = {.size = sizeof(info)};
        char               title[128] = "", artist[128] = "", key[32];
        bmds_audio_decode_info(pcm, &info);
        for (uint32_t i = 0; i < info.tags; ++i) {
                char value[128];
                if (bmds_audio_decode_tag(pcm, i, key, sizeof(key), value, sizeof(value)) < 0) continue;
                if (strcmp(key, "TITLE") == 0) strcpy(title, value);
                if (strcmp(key, "ARTIST") == 0) strcpy(artist, value);
        }
        printf("%s: %s, %u Hz, %u channel%s, %u kb/s", argv[1], info.name, (unsigned)info.rate, (unsigned)info.channels,
               info.channels == 1 ? "" : "s", (unsigned)(info.bitrate / 1000));
        if (info.frames != BMDS_DECODE_UNKNOWN_FRAMES)
                printf(", %s%.1f s", (info.flags & BMDS_DECODE_ESTIMATED) ? "about " : "",
                       (double)info.frames / info.rate);
        printf("\n");
        if (title[0] || artist[0]) printf("  %s%s%s\n", artist, title[0] && artist[0] ? " - " : "", title);

        const int dsp = open("/dev/dsp", O_WRONLY);
        if (dsp < 0) {
                fprintf(stderr, "player: /dev/dsp: %s\n", strerror(errno));
                return 1;
        }
        const bmds_audio_config_t out = {
                .size = sizeof(out), .format = info.format, .channels = info.channels, .rate = info.rate};
        const int32_t r = bmds_audio_config(dsp, &out, NULL);
        if (r < 0) {
                fprintf(stderr, "player: /dev/dsp: %s\n", strerror((int)-r));
                return 1;
        }
        for (;;) {
                const ssize_t n = read(pcm, buf, sizeof(buf));
                if (n < 0) {
                        fprintf(stderr, "player: %s: %s\n", argv[1], strerror(errno));
                        return 1;
                }
                if (n == 0) break;
                if (write(dsp, buf, (size_t)n) != n) {
                        fprintf(stderr, "player: /dev/dsp: %s\n", strerror(errno));
                        return 1;
                }
        }
        close(dsp); // waits until it has played
        return 0;
}
