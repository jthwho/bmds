/**
 * @file      seq_internal.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The sequencer's insides (seq.c), for the host tests: a sequencer with no port and no thread, the
 * scheduler driven by hand, its events to a function.
 */

#ifndef BMDS_SEQ_INTERNAL_H
#define BMDS_SEQ_INTERNAL_H

#include <bmds/seq.h>

/** @brief An event as the scheduler makes it: when it is heard, and its bytes. */
typedef void (*bmds_seq_emit_t)(void *arg, int64_t heard_ns, const uint8_t *bytes, uint32_t length);

/** @brief Makes a sequencer for @p song with no port (the timeline, the scheduler, the hits). */
int bmds_seq_make(const bmds_seq_song_t *song, bmds_seq_t **out);

/** @brief Starts the scheduler at the song's first step, heard at @p start_ns, with @p latency_ns. */
void bmds_seq_start_at(bmds_seq_t *seq, int64_t start_ns, int64_t latency_ns);

/** @brief Emits, in time order, every event heard before @p horizon_ns not emitted yet. */
void bmds_seq_schedule(bmds_seq_t *seq, int64_t horizon_ns, bmds_seq_emit_t emit, void *arg);

#endif // BMDS_SEQ_INTERNAL_H
