/**
 * @file      doom_bmds.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What Doom's platform files (devplan/games.md §19) share.
 */

#ifndef DOOM_BMDS_H
#define DOOM_BMDS_H

/**
 * @brief Opens the sound device and has the synth load its font (an empty song played), before
 *        Doom takes its zone: the font needs ~6 MB in one piece, which the zone would split.
 */
void doom_bmds_sound_early(void);

#endif /* DOOM_BMDS_H */
