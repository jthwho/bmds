/**
 * @file      startwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Preso from the desktop (devplan/preso.md §8b): the window `preso` opens without a deck, on
 * the GUI service through the SDK's calls (libbmdsgui), and the message box that tells a deck
 * that won't load when there's no terminal to tell it in.
 */

#ifndef PRESO_STARTWINDOW_H
#define PRESO_STARTWINDOW_H

#include <stdio.h>

namespace preso {

        /**
         * @brief Presents deck @p path, its problems written to @p messages.
         * @return 0, 1 (it failed while up) or 2 (it didn't load).
         */
        using PresentDeck = int (*)(const char *path, FILE *messages);

        /**
         * @brief Runs Preso's start window until it is closed: Open a Deck... (the file dialog),
         *        the examples installed beside @p program (argv[0]), Quit. A deck chosen is
         *        presented by @p present with the window closed, and a deck that won't load is
         *        told in a message box.
         * @return The exit status: 0, or 1 if there is no GUI service.
         */
        int runStartWindow(const char *program, PresentDeck present);

        /**
         * @brief Tells @p text in a message box titled @p title, an error, and waits until it is
         *        answered.
         * @return False if there is no GUI service to tell it.
         */
        bool tellInWindow(const char *title, const char *text);

} // namespace preso

#endif
