# The sequencer: songs as patterns, played through MIDI, with beat sync

Status: **planned** (2026-11-06, with the user). In the SDK (`libbmdsgame`, `bmds/seq.h`), so any
program can play music it carries as data and draw in time with it. First user: BMDS Demo #1
(`demo.md`).

## 1. Goal

- **An in-program sequencer** (the user's choice over a MIDI file): a song is patterns of steps
  arranged in sections, played live through a MIDI port (FM, the wavetable, or any port) and the
  game mixer's samples.
- **Beat sync** (the user's): a program asks where the song is at any moment, as heard, and which
  notes were heard between two moments, so a frame shown at time T draws what the music is doing
  at T. This is what demos are made of: the kick flashes the palette on the frame you hear it.
- **Live control:** input can change the music (mute a track, change a pattern at the next bar,
  move a filter, jump a section, play a note quantized to the beat).
- **Part of the SDK** (the user's, 2026-11-06), documented for others, with the kernel pieces it
  needs (§7).

## 2. Decisions (the user's, 2026-11-06)

- In-program sequencer, in the SDK, with beat sync.
- Plays through **MIDI ports** with scheduled output (`audio.md` §25): the system synth by default
  (`/dev/midi`), a named port (`wavetable`, `fm`, a USB synth) on request. Choosing the wavetable
  is choosing its port.
- **Samples too:** tracks may trigger the game mixer's sounds (custom drums, speech rendered by the
  speech engine) on their steps.

## 3. Facts that shape the design (checked 2026-11-06)

- **Scheduled MIDI:** a `bmds_midi_event_t` written with `time_ns` (`BMDS_CLOCK_MONOTONIC`) is sent
  at that time and the synth places it on its sample (worst 31-266 µs late on the board); up to 512
  unsent per fd, up to 60 s ahead, never before the fd's last scheduled; `BMDS_MIDI_CANCEL` drops
  what's queued. `midiout.c` already writes a file's events 40 ms ahead from a thread.
- **The system synths** (`fm`, `wavetable`) run at the device's period (240 frames, 5 ms). What
  they place at `time_ns` is heard later by their stream's queue and the device's delay. Nothing
  tells a program that latency today (§7, K2).
- **The synths honour** program change, volume (7), expression (11), pan (10), modulation (1),
  sustain (64), pitch bend, resets. **Neither honours a filter or brightness controller** (FM:
  `fmsynth.cpp`; the wavetable: TinySoundFont). Acid basslines are filter sweeps (§7, K3).
- **The game mixer** is a real-time client: each period comes with its `position` and `play_ns`
  (`bmds_audio_cycle_t`), so a sound can start on an exact frame.
- **The speech engine** gives 16 kHz mono samples from a speech fd (`speech.md`); a sentence takes
  ~0.25 s or more to start. Rendered ahead into a sound, it can be played on a beat.
- **Surfaces** say which frame a present will be shown in, not when (`3d.md` §9, K1).

## 4. The model

### 4a. Time

A song's own time is **steps**: `steps_per_beat` (4: sixteenths), beats per bar (4). The tempo
(BPM, fractional) can change at section boundaries (a tempo list); **swing** delays every odd step
by a fraction of a step. A song position is `(section, bar, beat, step, phase)`; with the tempo
list it maps exactly to seconds and back.

Playing, the song's step 0 is at a **start time** on the monotonic clock, **as heard**: every
event is scheduled at its heard time minus its output's latency (§5b). So the song clock *is* what
the listener hears, and the frame clock is what the viewer sees: both are monotonic times, and
sync is comparing them.

### 4b. Songs (`bmds_seq_song_t`): plain data

All caller structs begin with `uint32_t size`; all are plain arrays so a song can be static
rodata.

- **Tracks** (`bmds_seq_track_t`): an output (a MIDI channel on the song's port, or the mixer), a
  program (GM) and bank, volume, pan, a kind (`NOTES`, `DRUMS` on channel 10, `SAMPLES`), a
  name, and its patterns.
- **Patterns** (`bmds_seq_pattern_t`): a length in steps and its **steps**: note (or drum key or
  sample index), velocity, length in steps (or tied), flags (`ACCENT`, `SLIDE`: portamento into
  the next note, as a 303's; `CHANCE`: plays with a probability, for variation), and **lanes** of
  controller values per step (a filter sweep is a lane on controller 74).
- **The arrangement**: an order list of **sections** (`bmds_seq_section_t`): a length in bars, a
  repeat count, the pattern each track plays (or none), a tempo, a transposition, and a **cue**
  (a name and a number the program reads: "drop", "breakdown").
- A song is validated once at `bmds_seq_open()` (indices in range, lengths sane); playing never
  checks again.

### 4c. A text form (step 4)

A small text format for songs, so people write music without C and without rebuilding:

```text
song "BMDS Demo #1"  bpm 140  swing 0.08
track kick   drums
track acid   notes  program 39  channel 2
pattern kick.4    16  x...x...x...x...
pattern acid.a    16  c2 . c3 . d#2 - g2 c3 / a#2 .  lane 74 20 40 60 90 120 90 60 40
section intro  bars 8  kick kick.4
section drop   bars 16 kick kick.4 acid acid.a  cue drop
```

Parsed into the same structs; fuzzed. Also **SMF export** (`bmds_seq_export_smf()`): the song as
a format 1 MIDI file, to check it in other tools or play it with `play`.

## 5. Playing

### 5a. The player

`bmds_seq_open(const bmds_seq_config_t *config, const bmds_seq_song_t *song, bmds_seq_t **out)`:
the config names the MIDI port (a path, `NULL` for `/dev/midi`), the game mixer (`NULL`: no sample
tracks), a lookahead (default 60 ms), and the latency to assume if the port can't say. Then
`bmds_seq_play(seq, start_ns)` (0: now plus the lookahead), `pause`, `resume`, `stop` (all notes
off, the queue cancelled), `bmds_seq_close`.

One **thread** per sequencer (`SCHED_FIFO`, on the audio core as the mixer's threads are; PSRAM
stack) wakes every ~20 ms (a precise sleep, `processes.md` §3.12), and schedules every event whose
heard time falls before `now + lookahead`. Nothing allocates while playing; events go out in one
`write()` per wake. At 140 BPM, 8 tracks of sixteenths with controller lanes is ~300 events a
second: ~20 per wake, far under the 512 the fd holds.

### 5b. Latency

Each output's **latency** (from scheduled time to heard) is asked of the kernel at open (K2) and
again when the port's route changes; the mixer's comes from its stream's `delay_frames`. An event
heard at H is scheduled at `H − latency`. Two outputs with different latencies (the synth and the
mixer) still sound together.

### 5c. Live control (any thread)

Changes are **quantized**: `NOW`, the next `STEP`, `BEAT`, `BAR`, or `SECTION`. They take effect
on the first step that isn't scheduled yet at or after the quantum, so the lookahead (60 ms) is
the most a "now" waits; anything coarser is exact.

- `bmds_seq_mute(seq, track, muted, quantum)`, `solo`;
- `bmds_seq_set_pattern(seq, track, pattern, quantum)`;
- `bmds_seq_jump(seq, section, quantum)`, `loop_section(seq, on)` (stay in a section until let go);
- `bmds_seq_set_tempo(seq, bpm, quantum)`, `transpose`;
- `bmds_seq_controller(seq, track, number, value)`: sent now (a mouse moving a filter);
- `bmds_seq_note(seq, track, note, velocity, length_steps, quantum)`: a one-shot on the grid
  (keys as drum pads, in time).

Changes go through a small lock-free queue to the player's thread; the tempo map of what has been
scheduled is append-only, so queries never wait for the player.

## 6. Sync: what programs ask

All take a monotonic time, usually the frame's shown time (`bmds_game_frame_ns()`, K1):

- `bmds_seq_position(seq, time_ns, &pos)`: section, its cue, bar, beat, step, `beat_phase` and
  `bar_phase` (0..1: a smooth pulse is `1 − beat_phase`), `song_seconds`, `bpm`.
- `bmds_seq_hits(seq, from_ns, to_ns, track_mask, hits, max)`: the notes **heard** in a window
  (track, note, velocity, the exact heard time), from a ring of what was scheduled. A frame
  passes `(last frame's time, this frame's time]` and reacts to each hit once.
- `bmds_seq_last_hit(seq, time_ns, track, &hit)`: the latest hit at or before a time, for
  envelopes that decay (`flash = velocity × exp(−(t − hit.time) / τ)`) without keeping state.
- `bmds_seq_cues(seq, from_ns, to_ns, ...)`: sections entered in the window (a part changes).

`bmds_game_frame_ns()` and these calls are all a frame needs: no callbacks on the player's
thread, no locks on the drawing thread.

## 7. In the kernel

- **K1, frame times** (`3d.md` §9): `bmds_surface_info_t` gains `frame_ns` and `frame_period_ns`
  (ABI, appended); `libbmdsgame` adds `bmds_game_frame_ns()`.
- **K2, a port's latency** (ABI, appended): `midi_control(fd, BMDS_MIDI_LATENCY, 0)` (op 5)
  returns the microseconds from an event's scheduled time to its sound at the output, for the
  port's destination as routed now (the system synths: their period, stream queue and the device's
  delay; a hardware or network port: 0, the device's own being unknown). `-BMDS_ENOSYS` before it
  (old kernels), so the configured default applies.
- **K3, the synths' sound controllers** (no ABI change: MIDI's own meanings, GM2's sound
  controllers): 74 brightness and 71 resonance (the wavetable: TinySoundFont's per-voice low-pass
  offset from the preset's own cutoff and Q; FM: the modulators' levels, which is what brightness
  is in FM), 72 release and 73 attack (both engines' envelopes, scaled), 5 and 65 portamento
  (glides, for `SLIDE`). Measured: the voice loop's cost with a filter moving (the wavetable's
  filter is already computed per voice; FM's change is per note-on and per controller).
  Controllers the synths ignored before now act, which is what GM2 says they mean.
- **Samples on a frame:** `bmds_game_play_at(audio, sound, heard_ns, volume, pan, flags)` in the
  mixer (libbmdsgame, not the kernel): a voice that starts on the frame of that time.
- **Speech into a sound:** `bmds_game_sound_speak(audio, text, voice, flags, &sound)`: the speech
  fd's samples rendered into a mixer sound (resampled as loading is), on the caller's thread, so a
  program renders lines ahead and plays them on a beat. With markup (`BMDS_SPEAK_MARKUP`) for
  pitch and rate.

## 8. Budgets

- **Code:** ~10-14 KB (player, queries, control), ~6 KB for the text form; an archive, so only
  programs that use it pay.
- **Memory:** the player's thread (8 KB PSRAM stack), the hit ring (512 × 16 bytes = 8 KB), the
  control queue (64 entries); the song is the caller's.
- **CPU:** the player wakes 50 times a second for under 50 µs each (measured in step 1). The
  synth's cost is the synth's (FM 8.0 µs a voice-block).

## 9. Testing

- **Host:** the tempo list and swing to times and back (exact at section boundaries, monotonic),
  scheduling against a fake clock and a fake port (every event once, in order, at its time minus
  latency), quantized changes landing where they should, hits and positions for a window, the
  text form's parser (and fuzzing), SMF export read back by `smf.c`.
- **The board:** `progtest`'s MIDI sink measuring events' times against the song's; a beat-flash
  example (`sdk/examples/beat`: the screen flashes on every kick, the speaker clicks with it,
  checked by eye and by a camera's slow motion if it's ever in doubt); `seq` in the benches.

## 10. Steps

1. **K1 and K2**, the player with the C structs (notes and drums; no samples yet), the sync
   queries, `bmds_game_frame_ns()`; host tests; the `beat` example.
2. **Live control** (all of §5c), **K3** in both synths, `SLIDE`.
3. **Samples:** `bmds_game_play_at()`, `bmds_game_sound_speak()`, sample tracks.
4. **The text form** and SMF export, fuzzed; a `seq` command-line player (`seq FILE`, `seq -o
   song.mid FILE`) as an example.
5. **angel bindings.**

## 11. As built

(Nothing yet.)
