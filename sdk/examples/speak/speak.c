/**
 * @file      speak.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * speak TEXT...: says TEXT aloud with bmds_say, which returns once it has
 * been heard. speak -o FILE TEXT...: the samples instead, from a speech fd
 * (bmds_audio_speak: text written, 16 kHz mono 16-bit samples read), saved
 * as a WAV file.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

static char    text[4096];
static int16_t samples[2048];

/* Writes a 44-byte PCM WAV header for BYTES of 16-bit mono samples at RATE. */
static int write_header(int fd, uint32_t rate, uint32_t bytes) {
        uint8_t        h[44];
        const uint32_t v[] = {36 + bytes, 16, 1 | (1u << 16), rate, rate * 2, 2 | (16u << 16), bytes};
        const uint8_t  at[] = {4, 16, 20, 24, 28, 32, 40};
        memcpy(h, "RIFF....WAVEfmt ....................data....", sizeof(h)); // sizes filled below
        for (int i = 0; i < 7; ++i)
                for (int b = 0; b < 4; ++b) h[at[i] + b] = (uint8_t)(v[i] >> (8 * b));
        return pwrite(fd, h, sizeof(h), 0) == (ssize_t)sizeof(h) ? 0 : -1;
}

static int save(const char *path, const char *words, size_t length) {
        const bmds_speak_config_t config = {.size = sizeof(config)};
        const int32_t             speech = bmds_audio_speak(&config);
        if (speech < 0) {
                fprintf(stderr, "speak: %s\n", strerror((int)-speech));
                return 1;
        }
        bmds_speak_info_t info = {.size = sizeof(info)};
        bmds_audio_speak_info(speech, &info);
        const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0 || write_header(out, info.rate, 0) < 0 || lseek(out, 44, SEEK_SET) != 44) {
                fprintf(stderr, "speak: %s: %s\n", path, strerror(errno));
                return 1;
        }
        // The text and its NUL (said to the end), written as it fits; the samples read as made.
        size_t   at = 0;
        uint32_t bytes = 0;
        for (;;) {
                if (at <= length) {
                        const ssize_t n = write(speech, words + at, length + 1 - at);
                        if (n > 0)
                                at += (size_t)n;
                        else if (errno != EAGAIN)
                                break;
                }
                const ssize_t r = read(speech, samples, sizeof(samples));
                if (r < 0) break;
                if (r == 0 && at > length) break;
                if (write(out, samples, (size_t)r) != r) break;
                bytes += (uint32_t)r;
        }
        close(speech);
        const int ok = write_header(out, info.rate, bytes) == 0 && close(out) == 0;
        printf("%s: %s, %.1f s\n", path, info.voice, bytes / 2.0 / info.rate);
        return ok ? 0 : 1;
}

int main(int argc, char **argv) {
        int         first = 1;
        const char *path = NULL;
        if (argc > 2 && strcmp(argv[1], "-o") == 0) {
                path = argv[2];
                first = 3;
        }
        if (first >= argc) {
                fprintf(stderr, "usage: speak [-o FILE.wav] TEXT...\n");
                return 2;
        }
        size_t length = 0;
        for (int i = first; i < argc; ++i) {
                const size_t n = strlen(argv[i]);
                if (length + n + 2 > sizeof(text)) break;
                if (length != 0) text[length++] = ' ';
                memcpy(text + length, argv[i], n);
                length += n;
        }
        text[length] = 0;
        if (!bmds_sys_has(BMDS_SYS_AUDIO_SPEAK)) {
                fprintf(stderr, "speak: this kernel has no speech\n");
                return 1;
        }
        if (path != NULL) return save(path, text, length);
        const int32_t r = bmds_say(text);
        if (r < 0) fprintf(stderr, "speak: %s\n", strerror((int)-r));
        return r < 0 ? 1 : 0;
}
