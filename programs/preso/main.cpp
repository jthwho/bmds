/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * preso (devplan/preso.md): presentations written in Markdown. `preso DECK [SLIDE]` presents
 * a deck; `preso check DECK...` reads decks, lays out their slides, and reports what is wrong.
 * The platform is BMDS's on the board (PRESO_BMDS), the headless one elsewhere.
 */

#include <stdio.h>
#include <string.h>
#include "core/presenter.h"

#ifdef PRESO_BMDS
#include "bmds/bmdsplatform.h"
using SystemPlatform = preso::BmdsPlatform;
#else
#include "headless/headless.h"
using SystemPlatform = preso::HeadlessPlatform;
#endif

#ifndef PRESO_VERSION
#define PRESO_VERSION "dev"
#endif

namespace {

        void usage(FILE *out) {
                fputs("usage: preso DECK [SLIDE]          present a deck (from slide SLIDE)\n"
                      "       preso check [-v] DECK...   report what is wrong in decks\n"
                      "       preso theme [NAME]         list the built-in themes, or print one (a start for yours)\n"
                      "       preso --version\n",
                      out);
                return;
        }

        int check(int argc, char **argv) {
                SystemPlatform platform;
                bool           verbose = false;
                int            status = 0;
                int            decks = 0;
                for (int i = 2; i < argc; i++) {
                        if (strcmp(argv[i], "-v") == 0) {
                                verbose = true;
                                continue;
                        }
                        int s = preso::checkDeck(platform, argv[i], verbose, stdout);
                        if (s > status) status = s;
                        decks++;
                }
                if (decks == 0) {
                        usage(stderr);
                        return 2;
                }
                return status;
        }

        int present(const char *path, uint32_t slide) {
                SystemPlatform   platform;
                preso::Presenter presenter(platform, path, stdout);
                if (presenter.load() != preso::Error::Ok) return 2;
                preso::Error error = presenter.run(slide);
                if (error != preso::Error::Ok) {
                        fprintf(stderr, "preso: %s: %s\n", path, preso::errorName(error));
                        return 1;
                }
                return 0;
        }

} // namespace

int main(int argc, char **argv) {
        if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
                printf("preso %s\n", PRESO_VERSION);
                return 0;
        }
        if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
                usage(stdout);
                return 0;
        }
        if (argc >= 2 && strcmp(argv[1], "check") == 0) return check(argc, argv);
        if (argc >= 2 && strcmp(argv[1], "theme") == 0) {
                if (argc == 2) {
                        for (const char *const *n = preso::ThemeSet::builtInNames(); *n != nullptr; n++)
                                printf("%s\n", *n);
                        return 0;
                }
                const char *text = preso::ThemeSet::builtInText(preso::StringView(argv[2]));
                if (text == nullptr) {
                        fprintf(stderr, "preso: no built-in theme `%s` (preso theme lists them)\n", argv[2]);
                        return 2;
                }
                fputs(text, stdout);
                return 0;
        }
        if (argc == 2 || argc == 3) {
                int32_t slide = argc == 3 ? preso::StringView(argv[2]).toInt(-1) : 1;
                if (slide < 1) {
                        usage(stderr);
                        return 2;
                }
                return present(argv[1], uint32_t(slide));
        }
        usage(stderr);
        return 2;
}
