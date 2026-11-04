/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * preso (devplan/preso.md): presentations written in Markdown. `preso DECK [SLIDE]` presents
 * a deck; `preso check DECK...` reads decks, lays out their slides (running their scripts), and
 * reports what is wrong; `preso export DECK DIR` draws each step to a PNG; `preso --api` lists
 * what a deck's scripts have (tests/preso/api.lock).
 * The platform is BMDS's on the board (PRESO_BMDS), the headless one elsewhere.
 */

#include <stdio.h>
#include <string.h>
#include "core/presenter.h"
#include "script/runtime.h"

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
                      "       preso --trust DECK [SLIDE] the same, its scripts with files, the system and the "
                      "network,\n"
                      "                                  its demos run without asking\n"
                      "       preso --loop DECK [SLIDE]  kiosk mode: after the last slide the first again\n"
                      "       preso check [-v] DECK...   report what is wrong in decks\n"
                      "       preso export [--trust] [--posters] DECK DIR\n"
                      "                                  each step of each slide to a PNG in DIR; with --posters\n"
                      "                                  each script region's last frame too, as SCRIPT.png\n"
                      "       preso theme [NAME]         list the built-in themes, or print one (a start for yours)\n"
                      "       preso --api | --api-help   what a deck's scripts have, one a line (with help)\n"
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
                        int s = preso::checkDeck(platform, argv[i], verbose, stdout, preso::makeAngelScripts);
                        if (s > status) status = s;
                        decks++;
                }
                if (decks == 0) {
                        usage(stderr);
                        return 2;
                }
                return status;
        }

        int exportDeck(int argc, char **argv) {
                bool        trusted = false, posters = false;
                const char *paths[2] = {nullptr, nullptr};
                int         n = 0;
                for (int i = 2; i < argc; i++) {
                        if (strcmp(argv[i], "--trust") == 0) {
                                trusted = true;
                        } else if (strcmp(argv[i], "--posters") == 0) {
                                posters = true;
                        } else if (argv[i][0] == '-' || n == 2) {
                                n = 3;
                                break;
                        } else {
                                paths[n++] = argv[i];
                        }
                }
                if (n != 2) {
                        usage(stderr);
                        return 2;
                }
                SystemPlatform   platform;
                preso::Presenter presenter(platform, paths[0], stderr, preso::makeAngelScripts, trusted);
                if (presenter.load() != preso::Error::Ok) return 2;
                uint32_t     written = 0;
                preso::Error error = presenter.exportSlides(paths[1], posters, &written);
                if (error != preso::Error::Ok) {
                        fprintf(stderr, "preso: %s: %s\n", paths[1], preso::errorName(error));
                        return 2;
                }
                printf("%s: %u pictures\n", paths[0], unsigned(written));
                return presenter.problems() > 0 ? 1 : 0;
        }

        int present(const char *path, uint32_t slide, bool trusted, bool loop) {
                SystemPlatform   platform;
                preso::Presenter presenter(platform, path, stdout, preso::makeAngelScripts, trusted);
                if (presenter.load() != preso::Error::Ok) return 2;
                presenter.setLoop(loop);
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
        if (argc == 2 && (strcmp(argv[1], "--api") == 0 || strcmp(argv[1], "--api-help") == 0)) {
                preso::printDeckApi(strcmp(argv[1], "--api-help") == 0);
                return 0;
        }
        if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
                usage(stdout);
                return 0;
        }
        if (argc >= 2 && strcmp(argv[1], "check") == 0) return check(argc, argv);
        if (argc >= 2 && strcmp(argv[1], "export") == 0) return exportDeck(argc, argv);
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
        // Only the command line trusts a deck (devplan/preso.md §7.7).
        bool trusted = false, loop = false;
        int  first = 1;
        for (; first < argc; first++) {
                if (strcmp(argv[first], "--trust") == 0) {
                        trusted = true;
                } else if (strcmp(argv[first], "--loop") == 0) {
                        loop = true;
                } else {
                        break;
                }
        }
        if (argc - first == 1 || argc - first == 2) {
                int32_t slide = argc - first == 2 ? preso::StringView(argv[first + 1]).toInt(-1) : 1;
                if (slide < 1 || argv[first][0] == '-') {
                        usage(stderr);
                        return 2;
                }
                return present(argv[first], uint32_t(slide), trusted, loop);
        }
        usage(stderr);
        return 2;
}
