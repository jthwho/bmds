/**
 * @file      monocypher.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Monocypher's core (thirdparty/monocypher, its own licence: keep its notice), for ed25519.cpp;
 * its Ed25519 is monocypher_ed25519.c (the two can't share a translation unit). Only what is
 * called is linked.
 */

#include <monocypher.c>
