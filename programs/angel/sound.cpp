/**
 * @file      sound.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Sound (devplan/scripting.md step 5): effects loaded from files (`Sound`) and mixed by
 * libbmdsgame, tones (`tone`, `beep`), music files (`playMusic`), QBasic's PLAY language
 * (`play("T120 O3 L8 CDEFGAB>C")`) on the system's MIDI synth, and speech (`say`).
 *
 * play() writes its notes to /dev/midi ahead, each with its time (the kernel sends them then),
 * and waits until they have played, as QBasic's PLAY did; "MB" in the string plays them in the
 * background, "MF" in the foreground again. The octave, length, tempo, style and instrument
 * carry over from one play() to the next, as in QBasic. Built for the host (no ANGEL_BMDS)
 * nothing sounds: play() checks its string, the rest does nothing or raises.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/game.h>
#include <bmds/sys.h>
#endif

namespace angel {

        namespace {

                int64_t nowNs() {
#ifdef ANGEL_BMDS
                        int64_t ns = 0;
                        (void)bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
                        return ns;
#else
                        struct timespec t;
                        clock_gettime(CLOCK_MONOTONIC, &t);
                        return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
#endif
                }

                void sleepNs(int64_t ns) {
                        if (ns <= 0) return;
                        struct timespec t = {time_t(ns / 1000000000), long(ns % 1000000000)};
                        while (nanosleep(&t, &t) != 0) {}
                        return;
                }

#ifdef ANGEL_BMDS
                float clampf(float v, float lo, float hi) {
                        return v < lo ? lo : v > hi ? hi : v;
                }
#endif

                // ---- PLAY: QBasic's music language

                // What carries over from one play() to the next.
                struct PlayState {
                                int  octave = 4;      // O: 0-6 (3 starts at middle C)
                                int  length = 4;      // L: a quarter note
                                int  tempo = 120;     // T: quarter notes a minute
                                int  style = 7;       // of 8: MN 7, ML 8, MS 6 (how long a note sounds)
                                int  instrument = 80; // I: General MIDI's square lead, the PC speaker's sound
                                int  velocity = 100;  // V
                                bool background = false;
                };
                PlayState playState;
                int64_t   playEndNs = 0; // when what was written ahead has played

                // A note as play() schedules it: its MIDI key (-1: a rest) and times.
                struct Note {
                                int     key;
                                int64_t startNs;
                                int64_t soundNs; // how long it sounds
                                int     instrument;
                                int     velocity;
                };

                // Parses @p text into notes from @p start (each to @p emit); false and @p error if bad.
                template <typename Emit>
                bool parsePlay(const char *text, PlayState &s, int64_t &t, Emit emit, char *error, size_t errorSize) {
                        size_t i = 0;
                        auto   skipSpaces = [&]() {
                                while (text[i] == ' ' || text[i] == '\t' || text[i] == ';') ++i;
                        };
                        auto number = [&](int &out) -> bool {
                                skipSpaces();
                                if (text[i] < '0' || text[i] > '9') return false;
                                long v = 0;
                                while (text[i] >= '0' && text[i] <= '9') {
                                        v = v * 10 + (text[i] - '0');
                                        if (v > 100000) v = 100000;
                                        ++i;
                                }
                                out = int(v);
                                return true;
                        };
                        auto fail = [&](const char *what, size_t at) {
                                snprintf(error, errorSize, "play: %s (at %zu in \"%s\")", what, at + 1, text);
                                return false;
                        };
                        // A note or pause of length @p length (1-64) with its dots.
                        auto duration = [&](int length) -> int64_t {
                                double d = 240.0e9 / (double(s.tempo) * length);
                                double part = d;
                                skipSpaces();
                                while (text[i] == '.') {
                                        part /= 2;
                                        d += part;
                                        ++i;
                                        skipSpaces();
                                }
                                return int64_t(d);
                        };
                        static constexpr int semitone[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
                        for (;;) {
                                skipSpaces();
                                const size_t at = i;
                                const char   c = char(text[i] >= 'a' && text[i] <= 'z' ? text[i] - 32 : text[i]);
                                if (c == 0) return true;
                                ++i;
                                if (c >= 'A' && c <= 'G') {
                                        int key = 12 * (s.octave + 2) + semitone[c - 'A'];
                                        skipSpaces();
                                        if (text[i] == '#' || text[i] == '+') {
                                                ++key;
                                                ++i;
                                        } else if (text[i] == '-') {
                                                --key;
                                                ++i;
                                        }
                                        int length = s.length;
                                        if (number(length) && (length < 1 || length > 64))
                                                return fail("a note's length is 1 to 64", at);
                                        const int64_t d = duration(length);
                                        emit(Note{key, t, d * s.style / 8, s.instrument, s.velocity});
                                        t += d;
                                } else if (c == 'N') {
                                        int n;
                                        if (!number(n) || n > 84) return fail("N needs a note 0 (a rest) to 84", at);
                                        const int64_t d = duration(s.length);
                                        if (n > 0) emit(Note{23 + n, t, d * s.style / 8, s.instrument, s.velocity});
                                        t += d;
                                } else if (c == 'P' || c == 'R') {
                                        int length = s.length;
                                        if (number(length) && (length < 1 || length > 64))
                                                return fail("a pause's length is 1 to 64", at);
                                        t += duration(length);
                                } else if (c == 'O') {
                                        int o;
                                        if (!number(o) || o > 6) return fail("O needs an octave 0 to 6", at);
                                        s.octave = o;
                                } else if (c == '>') {
                                        if (s.octave < 6) ++s.octave;
                                } else if (c == '<') {
                                        if (s.octave > 0) --s.octave;
                                } else if (c == 'L') {
                                        int l;
                                        if (!number(l) || l < 1 || l > 64) return fail("L needs a length 1 to 64", at);
                                        s.length = l;
                                } else if (c == 'T') {
                                        int tempo;
                                        if (!number(tempo) || tempo < 32 || tempo > 255)
                                                return fail("T needs a tempo 32 to 255", at);
                                        s.tempo = tempo;
                                } else if (c == 'I') {
                                        int instrument;
                                        if (!number(instrument) || instrument > 127)
                                                return fail("I needs an instrument 0 to 127 (General MIDI's)", at);
                                        s.instrument = instrument;
                                } else if (c == 'V') {
                                        int v;
                                        if (!number(v) || v > 127) return fail("V needs a volume 0 to 127", at);
                                        s.velocity = v;
                                } else if (c == 'M') {
                                        const char m = char(text[i] >= 'a' && text[i] <= 'z' ? text[i] - 32 : text[i]);
                                        if (m == 'N')
                                                s.style = 7;
                                        else if (m == 'L')
                                                s.style = 8;
                                        else if (m == 'S')
                                                s.style = 6;
                                        else if (m == 'F')
                                                s.background = false;
                                        else if (m == 'B')
                                                s.background = true;
                                        else
                                                return fail("M is followed by N, L, S, F or B", at);
                                        ++i;
                                } else {
                                        char what[48];
                                        snprintf(what, sizeof(what), "'%c' isn't a PLAY command", c);
                                        return fail(what, at);
                                }
                        }
                }

#ifdef ANGEL_BMDS
                int midiFd = -1;

                bool openMidi() {
                        if (midiFd >= 0) return true;
                        const int fd = open("/dev/midi", O_WRONLY | O_CLOEXEC);
                        if (fd < 0) {
                                raise("play: no MIDI synth (/dev/midi: %s)", strerror(errno));
                                return false;
                        }
                        bmds_midi_config_t c{};
                        c.size = sizeof(c);
                        c.flags = BMDS_MIDI_EVENTS;
                        c.event_size = sizeof(bmds_midi_event_t);
                        if (bmds_midi_config(fd, &c, nullptr) < 0) {
                                close(fd);
                                raise("play: /dev/midi doesn't take timed events");
                                return false;
                        }
                        midiFd = fd;
                        return true;
                }

                // One message at @p when, waiting while the fd's queue (512 events) is full.
                void sendMidi(int64_t when, uint8_t a, uint8_t b, uint8_t c, uint8_t length) {
                        bmds_midi_event_t e{};
                        e.time_ns = when;
                        e.length = length;
                        e.data[0] = a;
                        e.data[1] = b;
                        e.data[2] = c;
                        for (;;) {
                                const ssize_t n = write(midiFd, &e, sizeof(e));
                                if (n == ssize_t(sizeof(e)) || (n < 0 && errno != EAGAIN)) return;
                                sleepNs(10000000);
                        }
                }

                // ---- The mixer (effects, tones, music files)

                bmds_game_audio_t *mixer = nullptr;

                bmds_game_audio_t *audio() {
                        if (mixer != nullptr) return mixer;
                        const int r = bmds_game_audio_open(nullptr, &mixer);
                        if (r != 0) {
                                mixer = nullptr;
                                raise("No sound: %s", strerror(-r));
                        }
                        return mixer;
                }
#endif

#ifdef ANGEL_BMDS
                int sentInstrument = -1; // the synth's channel 1 as last set
#endif

                void play(asIScriptGeneric *gen) {
                        const String *text = static_cast<const String *>(gen->GetArgAddress(0));
                        const int64_t now = nowNs();
                        // From now, or after what an earlier play() put in the background.
                        int64_t   t = playEndNs > now ? playEndNs : now + 20000000;
                        PlayState parsing = playState;
                        char      error[160];
#ifdef ANGEL_BMDS
                        if (!openMidi()) return;
#endif
                        // Checked whole first: a bad string plays nothing.
                        int64_t dry = t;
                        if (!parsePlay(text->cStr(), parsing, dry, [](const Note &) {}, error, sizeof(error))) {
                                raise("%s", error);
                                return;
                        }
                        parsing = playState;
                        parsePlay(
                                text->cStr(), parsing, t,
                                [&](const Note &n) {
                                        if (n.key < 0 || n.key > 127) return;
#ifdef ANGEL_BMDS
                                        if (n.instrument != sentInstrument) {
                                                sendMidi(n.startNs, 0xc0, uint8_t(n.instrument), 0, 2);
                                                sentInstrument = n.instrument;
                                        }
                                        sendMidi(n.startNs, 0x90, uint8_t(n.key), uint8_t(n.velocity), 3);
                                        sendMidi(n.startNs + n.soundNs, 0x80, uint8_t(n.key), 0, 3);
#endif
                                },
                                error, sizeof(error));
                        playState = parsing;
                        playEndNs = t;
#ifdef ANGEL_BMDS
                        if (!parsing.background) {
                                fflush(stdout);
                                sleepNs(t - nowNs());
                        }
#endif
                }

                // ---- Live notes: on and off as the script says, on a channel of their own (2),
                // so play()'s music on channel 1 goes on beside them.

                constexpr uint8_t LiveChannel = 1;

                int checkedNote(asIScriptGeneric *gen) {
                        const int note = int(gen->GetArgDWord(0));
                        if (note < 0 || note > 127) {
                                raise("A note is 0 to 127 (middle C is 60), not %d", note);
                                return -1;
                        }
                        return note;
                }
                void noteOn(asIScriptGeneric *gen) {
                        const int note = checkedNote(gen);
                        const int v = int(gen->GetArgDWord(1));
                        if (note < 0) return;
#ifdef ANGEL_BMDS
                        if (!openMidi()) return;
                        sendMidi(0, 0x90 | LiveChannel, uint8_t(note), uint8_t(v < 1 ? 1 : v > 127 ? 127 : v), 3);
#else
                        (void)v;
#endif
                }
                void noteOff(asIScriptGeneric *gen) {
                        const int note = checkedNote(gen);
                        if (note < 0) return;
#ifdef ANGEL_BMDS
                        if (!openMidi()) return;
                        sendMidi(0, 0x80 | LiveChannel, uint8_t(note), 0, 3);
#endif
                }
                void instrument(asIScriptGeneric *gen) {
                        const int program = int(gen->GetArgDWord(0));
                        if (program < 0 || program > 127) {
                                raise("An instrument is 0 to 127 (General MIDI's), not %d", program);
                                return;
                        }
#ifdef ANGEL_BMDS
                        if (!openMidi()) return;
                        sendMidi(0, 0xc0 | LiveChannel, uint8_t(program), 0, 2);
#endif
                }
                void allNotesOff(asIScriptGeneric *) {
#ifdef ANGEL_BMDS
                        if (!openMidi()) return;
                        sendMidi(0, 0xb0 | LiveChannel, 123, 0, 3);
#endif
                }

                // ---- Sound: an effect loaded from a file

                class Sound {
                        public:
                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                }
#ifdef ANGEL_BMDS
                                ~Sound() {
                                        stop();
                                        bmds_game_sound_free(&sound);
                                }
                                void stop() {
                                        for (int32_t &v : _voices)
                                                if (v >= 0) {
                                                        if (mixer != nullptr) bmds_game_voice_stop(mixer, v);
                                                        v = -1;
                                                }
                                }
                                bool playing() {
                                        for (int32_t &v : _voices)
                                                if (v >= 0 && mixer != nullptr && bmds_game_voice_is_playing(mixer, v))
                                                        return true;
                                        return false;
                                }
                                void started(int32_t voice) {
                                        for (int32_t &v : _voices)
                                                if (v < 0 || mixer == nullptr ||
                                                    !bmds_game_voice_is_playing(mixer, v)) {
                                                        v = voice;
                                                        return;
                                                }
                                }
                                bmds_game_sound_t sound{};
#endif

                        private:
                                int _refs = 1;
#ifdef ANGEL_BMDS
                                int32_t _voices[BMDS_GAME_VOICES] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                                                     -1, -1, -1, -1, -1, -1, -1, -1};
#endif
                };

                Sound *self(asIScriptGeneric *gen) {
                        return static_cast<Sound *>(gen->GetObject());
                }

                void soundLoad(asIScriptGeneric *gen) {
                        const String *path = static_cast<const String *>(gen->GetArgAddress(0));
#ifdef ANGEL_BMDS
                        bmds_game_audio_t *a = audio();
                        if (a == nullptr) return;
                        Sound    *s = new Sound();
                        const int r = bmds_game_sound_load(a, path->cStr(), &s->sound);
                        if (r != 0) {
                                delete s;
                                raise("Can't load '%s': %s", path->cStr(), strerror(-r));
                                return;
                        }
                        gen->SetReturnAddress(s);
#else
                        raise("Loading sounds (%s) needs BMDS", path->cStr());
#endif
                }
                void soundAddRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }
                void soundRelease(asIScriptGeneric *gen) {
                        self(gen)->release();
                }
                void soundPlay(asIScriptGeneric *gen, bool loop) {
#ifdef ANGEL_BMDS
                        Sound      *s = self(gen);
                        const float volume = clampf(gen->GetArgFloat(0), 0, 1),
                                    pan = clampf(gen->GetArgFloat(1), -1, 1);
                        const int32_t v = bmds_game_play(mixer, &s->sound, volume, pan, loop ? BMDS_GAME_PLAY_LOOP : 0);
                        if (v >= 0) s->started(v);
#else
                        (void)gen;
                        (void)loop;
#endif
                }
                void soundPlayOnce(asIScriptGeneric *gen) {
                        soundPlay(gen, false);
                }
                void soundLoop(asIScriptGeneric *gen) {
                        soundPlay(gen, true);
                }
                void soundStop(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        self(gen)->stop();
#else
                        (void)gen;
#endif
                }
                void soundPlaying(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        gen->SetReturnByte(self(gen)->playing());
#else
                        gen->SetReturnByte(0);
#endif
                }
                void soundLength(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        const Sound *s = self(gen);
                        gen->SetReturnFloat(
                                mixer != nullptr ? float(s->sound.frames) / float(bmds_game_audio_rate(mixer)) : 0.0f);
#else
                        gen->SetReturnFloat(0);
#endif
                }

                // ---- Tones

                // A square wave (the PC speaker's) of @p hz for @p ms at @p volume, waited for.
                void playTone(float hz, uint32_t ms, float volume) {
#ifdef ANGEL_BMDS
                        bmds_game_audio_t *a = audio();
                        if (a == nullptr || ms == 0) return;
                        if (ms > 60000) ms = 60000;
                        const uint32_t rate = bmds_game_audio_rate(a);
                        const uint32_t frames = uint32_t(uint64_t(rate) * ms / 1000);
                        int16_t *samples = static_cast<int16_t *>(malloc(sizeof(int16_t) * (frames != 0 ? frames : 1)));
                        if (samples == nullptr) {
                                raise("Out of memory for a tone");
                                return;
                        }
                        // A quarter of full scale at volume 1, the ends faded over 3 ms against clicks.
                        const float    amplitude = 8192.0f * clampf(volume, 0, 1);
                        const uint32_t fade = rate * 3 / 1000;
                        const float    period = hz > 0 ? float(rate) / hz : 0;
                        for (uint32_t i = 0; i < frames; ++i) {
                                float v = period > 0 ? (fmodf(float(i), period) < period / 2 ? amplitude : -amplitude)
                                                     : 0;
                                if (i < fade) v *= float(i) / float(fade);
                                if (frames - i < fade) v *= float(frames - i) / float(fade);
                                samples[i] = int16_t(v);
                        }
                        bmds_game_sound_t tone = {samples, frames, 1};
                        const int32_t     voice = bmds_game_play(a, &tone, 1, 0, 0);
                        fflush(stdout);
                        sleepNs(int64_t(ms) * 1000000);
                        // The mixer may still be reading it: freed once the voice has ended.
                        for (int tries = 0; voice >= 0 && tries < 50 && bmds_game_voice_is_playing(a, voice); ++tries)
                                sleepNs(2000000);
                        if (voice >= 0 && bmds_game_voice_is_playing(a, voice)) {
                                bmds_game_voice_stop(a, voice);
                                sleepNs(20000000);
                        }
                        free(samples);
#else
                        (void)hz;
                        (void)ms;
                        (void)volume;
#endif
                }
                void tone(asIScriptGeneric *gen) {
                        playTone(gen->GetArgFloat(0), gen->GetArgDWord(1), gen->GetArgFloat(2));
                }
                void beep(asIScriptGeneric *) {
                        playTone(800, 250, 0.5f);
                }

                // ---- Music files

                void playMusic(asIScriptGeneric *gen) {
                        const String *path = static_cast<const String *>(gen->GetArgAddress(0));
#ifdef ANGEL_BMDS
                        bmds_game_audio_t *a = audio();
                        if (a == nullptr) return;
                        const uint32_t flags = gen->GetArgByte(1) != 0 ? BMDS_GAME_PLAY_LOOP : 0;
                        const int r = bmds_game_music_play(a, path->cStr(), clampf(gen->GetArgFloat(2), 0, 1), flags);
                        if (r != 0) raise("Can't play '%s': %s", path->cStr(), strerror(-r));
#else
                        raise("Playing music (%s) needs BMDS", path->cStr());
#endif
                }
                void stopMusic(asIScriptGeneric *) {
#ifdef ANGEL_BMDS
                        if (mixer != nullptr) bmds_game_music_stop(mixer);
#endif
                }
                void musicVolume(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        if (mixer != nullptr) bmds_game_music_volume(mixer, clampf(gen->GetArgFloat(0), 0, 1));
#else
                        (void)gen;
#endif
                }

                // ---- Speech

                void say(asIScriptGeneric *gen) {
                        const String *text = static_cast<const String *>(gen->GetArgAddress(0));
#ifdef ANGEL_BMDS
                        fflush(stdout);
                        const int32_t r = bmds_say(text->cStr());
                        if (r < 0 && r != -BMDS_EINTR) raise("Can't speak: %s", strerror(-r));
#else
                        (void)text;
#endif
                }

        } // namespace

        void finishSound() {
                // What play() put in the background is heard to its end.
                sleepNs(playEndNs - nowNs());
                return;
        }

        void registerSound(asIScriptEngine *engine) {
                def(engine, "void play(const string &in music)", play,
                    "Plays notes in QBasic's music language: \"T120 O3 L8 CDEFGAB>C\" (A-G notes, # sharp, - flat, "
                    "O octave, < >, L length, T tempo, P pause, N note, MN ML MS styles, MB in the background, "
                    "I instrument, V volume).");
                def(engine, "void tone(float hz, uint ms, float volume = 0.5)", tone,
                    "Sounds a tone of a frequency for so many milliseconds (QBasic's SOUND), and waits for it.");
                def(engine, "void beep()", beep, "A short beep.");
                def(engine, "void playMusic(const string &in path, bool loop = false, float volume = 1)", playMusic,
                    "Plays a music file in the background: MIDI, MP3, Ogg Vorbis, FLAC or WAV; again from its start if "
                    "loop.");
                def(engine, "void stopMusic()", stopMusic, "Stops the music.");
                def(engine, "void musicVolume(float volume)", musicVolume, "The music's volume, 0 to 1.");
                def(engine, "void say(const string &in text)", say, "Says the text aloud, and waits until it has.");
                def(engine, "void noteOn(int note, int volume = 100)", noteOn,
                    "Starts a note on the synth now, held until noteOff (note 60 is middle C, 61 C#...; volume 1 to "
                    "127).");
                def(engine, "void noteOff(int note)", noteOff, "Lets go of a note noteOn started.");
                def(engine, "void instrument(int program)", instrument,
                    "The instrument noteOn plays: General MIDI's 0 to 127 (0 piano, 19 organ, 40 violin...).");
                def(engine, "void allNotesOff()", allNotesOff, "Lets go of every note noteOn started.");

                check(engine->RegisterObjectType("Sound", 0, asOBJ_REF), "Sound");
                behaviour(engine, "Sound", asBEHAVE_FACTORY, "Sound@ f(const string &in path)", soundLoad,
                          "A sound effect from a file (WAV, MP3, Ogg Vorbis, FLAC), loaded whole.");
                behaviour(engine, "Sound", asBEHAVE_ADDREF, "void f()", soundAddRef, nullptr);
                behaviour(engine, "Sound", asBEHAVE_RELEASE, "void f()", soundRelease, nullptr);
                method(engine, "Sound", "void play(float volume = 1, float pan = 0)", soundPlayOnce,
                       "Plays it once, at a volume (0 to 1) and place (-1 left to 1 right); several at once mix.");
                method(engine, "Sound", "void loop(float volume = 1, float pan = 0)", soundLoop,
                       "Plays it over and over.");
                method(engine, "Sound", "void stop()", soundStop, "Stops it wherever it plays.");
                method(engine, "Sound", "bool playing()", soundPlaying, "It is playing.");
                method(engine, "Sound", "float length() const", soundLength, "How long it is, in seconds.");
                return;
        }

} // namespace angel
