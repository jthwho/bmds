/**
 * @file      audio.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Audio and speech for scripts (devplan/guiprograms.md §14, step 6c): `AudioPlayer` (a sound file
 * decoded and played, paused, sought, its tags), `mixer` (the default device's volume), `Voice`
 * (speech in the background, each text told when heard) and `MidiPort` (a port's messages sent
 * and called back).
 *
 * A player and a speaking voice pump on a thread of their own (the decoder's or the speaker's
 * samples into /dev/dsp), started when they begin and joined when they end. The threads touch no
 * script object: under the object's lock they leave what happened, and tell the loop through one
 * pipe they share; the loop calls the script back. A playing player and a speaking voice are kept
 * (a reference) until they end, so one let go of is still heard.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

#ifdef ANGEL_BMDS
#include <bmds/sys.h>
#endif

namespace angel {

        namespace {

                const String &argString(asIScriptGeneric *gen, int i) {
                        return *static_cast<const String *>(gen->GetArgAddress(asUINT(i)));
                }
                void returnString(asIScriptGeneric *gen, const char *text) {
                        new (gen->GetAddressOfReturnLocation()) String(text);
                }
                float clampf(float v, float lo, float hi) {
                        return v < lo ? lo : (v > hi ? hi : v);
                }

                // ---- What the threads tell the loop: one pipe, one source

                int        wake[2] = {-1, -1};
                LoopSource wakeSource{-1, nullptr, nullptr, LoopHold::Nothing};
                bool       wakeListed = false;

                void tellLoop() {
                        const char b = 1;
                        if (wake[1] >= 0) (void)write(wake[1], &b, 1);
                        return;
                }

                void updateHold(); // the source holds app.run() while a callback may come
                void wakeReady(LoopSource *source);

                // The pipe and its source, made at the first use. False with the problem raised.
                bool listen() {
                        if (wake[0] < 0 && pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0) {
                                wake[0] = wake[1] = -1;
                                raise("Can't make a pipe: %s", strerror(errno));
                                return false;
                        }
                        if (!wakeListed) {
                                wakeSource.fd = wake[0];
                                wakeSource.ready = wakeReady;
                                if (!addLoopSource(&wakeSource)) return false;
                                wakeListed = true;
                        }
                        return true;
                }

                // ---- AudioPlayer

                constexpr size_t   MaxKept = 16; // players and voices sounding at once
                constexpr uint32_t ChunkFrames = 1024;

                class AudioPlayer {
                        public:
                                ~AudioPlayer();
                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }

                                int      decoder = -1;
                                int      dsp = -1; // while it sounds (from play() until it ends or stops)
                                uint32_t rate = 0, channels = 0, frameBytes = 0, bitrate = 0;
                                int64_t  frames = -1; // -1: not known
                                char     codec[8] = "";
                                float    volume = 1;
                                bool     paused = false;
                                bool     kept = false;
                                // The position: the frame played from at the last seek, and the
                                // device's count then.
                                int64_t            base = 0;
                                uint64_t           playedAtBase = 0;
                                asIScriptFunction *onFinished = nullptr;

                                // Shared with the thread, under the lock.
                                pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
                                bool            loop = false;
                                bool            stopping = false;
                                bool            finished = false; // its end heard (told once)
                                bool            running = false;  // the thread (joined when it isn't)
                                bool            joinable = false;
                                pthread_t       thread{};

                        private:
                                int _refs = 1;
                };

                AudioPlayer *keptPlayers[MaxKept];
                size_t       keptPlayerCount = 0;

                AudioPlayer *player(asIScriptGeneric *gen) {
                        return static_cast<AudioPlayer *>(gen->GetObject());
                }

#ifdef ANGEL_BMDS
                int32_t gainOf(float volume) {
                        if (volume <= 0.0001f) return BMDS_AUDIO_GAIN_MUTED;
                        const int32_t cdb = int32_t(2000.0f * log10f(volume));
                        return cdb < BMDS_AUDIO_GAIN_MUTED ? BMDS_AUDIO_GAIN_MUTED : cdb;
                }

                void *pump(void *arg) {
                        auto          *p = static_cast<AudioPlayer *>(arg);
                        const uint32_t chunk = ChunkFrames * p->frameBytes;
                        uint8_t       *buffer = static_cast<uint8_t *>(malloc(chunk));
                        bool           ended = false;
                        for (bool rewound = false; buffer != nullptr;) {
                                pthread_mutex_lock(&p->lock);
                                const bool stop = p->stopping, loop = p->loop;
                                pthread_mutex_unlock(&p->lock);
                                if (stop) break;
                                const ssize_t n = read(p->decoder, buffer, chunk);
                                if (n == 0 && loop && !rewound) {
                                        rewound = true; // an empty file doesn't spin
                                        if (lseek(p->decoder, 0, SEEK_SET) < 0) break;
                                        continue;
                                }
                                if (n <= 0) {
                                        // Its end: what is queued plays out (a stop drops it).
                                        if (n == 0) ended = bmds_audio_control(p->dsp, BMDS_AUDIO_DRAIN, 0) == 0;
                                        break;
                                }
                                rewound = false;
                                for (ssize_t done = 0; done < n;) {
                                        const ssize_t w = write(p->dsp, buffer + done, size_t(n - done));
                                        if (w <= 0) break;
                                        done += w;
                                }
                        }
                        free(buffer);
                        pthread_mutex_lock(&p->lock);
                        p->finished = ended && !p->stopping;
                        p->running = false;
                        pthread_mutex_unlock(&p->lock);
                        tellLoop();
                        return nullptr;
                }

                // The thread stopped (what is queued thrown away) and joined.
                void halt(AudioPlayer *p) {
                        pthread_mutex_lock(&p->lock);
                        p->stopping = true;
                        const bool joinable = p->joinable;
                        pthread_mutex_unlock(&p->lock);
                        if (p->dsp >= 0) {
                                (void)bmds_audio_control(p->dsp, BMDS_AUDIO_RESUME, 0); // a paused write
                                (void)bmds_audio_control(p->dsp, BMDS_AUDIO_DROP, 0);   // returns
                        }
                        if (joinable) pthread_join(p->thread, nullptr);
                        // What the thread wrote after the drop, before it saw the stop, goes too.
                        if (p->dsp >= 0) (void)bmds_audio_control(p->dsp, BMDS_AUDIO_DROP, 0);
                        pthread_mutex_lock(&p->lock);
                        p->joinable = p->running = p->stopping = false;
                        pthread_mutex_unlock(&p->lock);
                        p->paused = false;
                        return;
                }

                uint64_t playedFrames(AudioPlayer *p) {
                        bmds_audio_status_t s{};
                        s.size = sizeof(s);
                        return p->dsp >= 0 && bmds_audio_status(p->dsp, &s) == 0 ? s.played_frames : 0;
                }

                // Its thread started from the decoder's position (the stream opened if it isn't).
                bool start(AudioPlayer *p) {
                        if (p->dsp < 0) {
                                const int fd = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
                                if (fd < 0) {
                                        raise("Can't play: %s", strerror(errno));
                                        return false;
                                }
                                bmds_audio_config_t c{};
                                c.size = sizeof(c);
                                c.format = BMDS_AUDIO_S16LE;
                                c.channels = p->channels;
                                c.rate = p->rate;
                                c.ring_frames = p->rate / 2;
                                const int32_t r = bmds_audio_config(fd, &c, nullptr);
                                if (r < 0) {
                                        close(fd);
                                        raise("Can't play: %s", strerror(-r));
                                        return false;
                                }
                                (void)bmds_audio_control(fd, BMDS_AUDIO_GAIN, gainOf(p->volume));
                                p->dsp = fd;
                                p->playedAtBase = 0;
                        }
                        pthread_mutex_lock(&p->lock);
                        p->stopping = p->finished = false;
                        p->running = true;
                        const bool made = pthread_create(&p->thread, nullptr, pump, p) == 0;
                        p->joinable = made;
                        p->running = made;
                        pthread_mutex_unlock(&p->lock);
                        if (!made) raise("Can't play: no thread");
                        return made;
                }
#endif

                bool settle(AudioPlayer *p);
                void unkeepPlayer(AudioPlayer *p);

#ifdef ANGEL_BMDS
                // Those that ended with no callback to come let go of (a script that never runs the
                // loop still starts more).
                void sweepPlayers() {
                        for (size_t i = keptPlayerCount; i-- > 0;) {
                                AudioPlayer *p = keptPlayers[i];
                                if (p->onFinished != nullptr) continue;
                                (void)settle(p);
                                pthread_mutex_lock(&p->lock);
                                const bool running = p->running;
                                pthread_mutex_unlock(&p->lock);
                                if (!running) unkeepPlayer(p);
                        }
                        return;
                }

                void keepPlayer(AudioPlayer *p) {
                        if (p->kept) return;
                        if (keptPlayerCount == MaxKept) sweepPlayers();
                        if (keptPlayerCount == MaxKept) {
                                raise("Too many players sounding (%u)", unsigned(MaxKept));
                                return;
                        }
                        p->addRef();
                        p->kept = true;
                        keptPlayers[keptPlayerCount++] = p;
                        updateHold();
                        return;
                }
#endif
                void unkeepPlayer(AudioPlayer *p) {
                        for (size_t i = 0; i < keptPlayerCount; ++i) {
                                if (keptPlayers[i] != p) continue;
                                keptPlayers[i] = keptPlayers[--keptPlayerCount];
                                p->kept = false;
                                updateHold();
                                p->release();
                                return;
                        }
                        return;
                }

                // A player whose thread ended: joined, its stream closed; @return its end was heard.
                bool settle(AudioPlayer *p) {
#ifdef ANGEL_BMDS
                        pthread_mutex_lock(&p->lock);
                        const bool ended = !p->running && p->joinable;
                        const bool finished = ended && p->finished;
                        pthread_mutex_unlock(&p->lock);
                        if (!ended) return false;
                        pthread_join(p->thread, nullptr);
                        p->joinable = false;
                        if (finished) {
                                // At its end: the position is its length until it plays again.
                                p->base = p->frames >= 0 ? p->frames : p->base;
                                close(p->dsp);
                                p->dsp = -1;
                                p->playedAtBase = 0;
                        }
                        return finished;
#else
                        (void)p;
                        return false;
#endif
                }

                AudioPlayer::~AudioPlayer() {
#ifdef ANGEL_BMDS
                        halt(this);
                        if (dsp >= 0) close(dsp);
                        if (decoder >= 0) close(decoder);
#endif
                        if (onFinished != nullptr) onFinished->Release();
                        pthread_mutex_destroy(&lock);
                }

                void playerMake(asIScriptGeneric *gen) {
                        const String &path = argString(gen, 0);
#ifdef ANGEL_BMDS
                        const int fd = open(path.cStr(), O_RDONLY | O_CLOEXEC);
                        if (fd < 0) {
                                raise("Can't play '%s': %s", path.cStr(), strerror(errno));
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        bmds_decode_config_t d{};
                        d.size = sizeof(d);
                        d.format = BMDS_AUDIO_S16LE;
                        d.flags = BMDS_O_CLOEXEC;
                        const int32_t dec = bmds_audio_decode(fd, &d);
                        close(fd); // the decoder keeps its own
                        bmds_decode_info_t info{};
                        info.size = sizeof(info);
                        int32_t r = dec < 0 ? dec : bmds_audio_decode_info(dec, &info);
                        if (r == 0 && info.channels != 1 && info.channels != 2) r = -BMDS_ENOTSUP;
                        if (r < 0) {
                                if (dec >= 0) close(dec);
                                raise("Can't play '%s': %s", path.cStr(),
                                      r == -BMDS_EINVAL ? "not a sound file known" : strerror(-r));
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        AudioPlayer *p = new AudioPlayer();
                        p->decoder = dec;
                        p->rate = info.rate;
                        p->channels = info.channels;
                        p->frameBytes = info.frame_bytes != 0 ? info.frame_bytes : 2 * info.channels;
                        p->bitrate = info.bitrate;
                        p->frames = info.frames == BMDS_DECODE_UNKNOWN_FRAMES ? -1 : int64_t(info.frames);
                        memcpy(p->codec, info.name, sizeof(p->codec));
                        p->codec[sizeof(p->codec) - 1] = 0;
                        gen->SetReturnAddress(p);
#else
                        raise("Playing '%s' needs BMDS", path.cStr());
                        gen->SetReturnAddress(nullptr);
#endif
                }

                void playerAddRef(asIScriptGeneric *gen) {
                        player(gen)->addRef();
                }
                void playerRelease(asIScriptGeneric *gen) {
                        player(gen)->release();
                }

                void playerPlay(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        AudioPlayer *p = player(gen);
                        (void)settle(p);
                        if (p->paused) {
                                (void)bmds_audio_control(p->dsp, BMDS_AUDIO_RESUME, 0);
                                p->paused = false;
                                return;
                        }
                        pthread_mutex_lock(&p->lock);
                        const bool running = p->running;
                        pthread_mutex_unlock(&p->lock);
                        if (running) return;
                        if (p->frames >= 0 && p->base >= p->frames) { // at its end: from the start
                                if (lseek(p->decoder, 0, SEEK_SET) < 0) return;
                                p->base = 0;
                        }
                        if (!listen()) return;
                        if (start(p)) keepPlayer(p);
#else
                        (void)gen;
#endif
                }
                void playerPause(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        AudioPlayer *p = player(gen);
                        pthread_mutex_lock(&p->lock);
                        const bool running = p->running;
                        pthread_mutex_unlock(&p->lock);
                        if (!running || p->paused) return;
                        (void)bmds_audio_control(p->dsp, BMDS_AUDIO_PAUSE, 0);
                        p->paused = true;
#else
                        (void)gen;
#endif
                }
                void playerStop(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
#ifdef ANGEL_BMDS
                        halt(p);
                        if (p->dsp >= 0) close(p->dsp);
                        p->dsp = -1;
                        (void)lseek(p->decoder, 0, SEEK_SET);
                        p->base = 0;
                        p->playedAtBase = 0;
#endif
                        unkeepPlayer(p);
                }
                void playerWait(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        AudioPlayer *p = player(gen);
                        pthread_mutex_lock(&p->lock);
                        const bool loop = p->loop, joinable = p->joinable;
                        pthread_mutex_unlock(&p->lock);
                        if (!joinable) return;
                        if (loop) {
                                raise("It loops: it would never end");
                                return;
                        }
                        if (p->paused) {
                                raise("It's paused: it would never end");
                                return;
                        }
                        fflush(stdout);
                        pthread_join(p->thread, nullptr);
                        p->joinable = false;
                        pthread_mutex_lock(&p->lock);
                        p->running = false;
                        const bool finished = p->finished;
                        p->finished = false; // waited for: not called back
                        pthread_mutex_unlock(&p->lock);
                        if (finished) {
                                p->base = p->frames >= 0 ? p->frames : p->base;
                                close(p->dsp);
                                p->dsp = -1;
                                p->playedAtBase = 0;
                        }
                        unkeepPlayer(p);
#else
                        (void)gen;
#endif
                }
                void playerPlaying(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
                        pthread_mutex_lock(&p->lock);
                        const bool running = p->running;
                        pthread_mutex_unlock(&p->lock);
                        gen->SetReturnByte(running && !p->paused);
                }

                // Where it plays, in frames.
                int64_t positionOf(AudioPlayer *p) {
#ifdef ANGEL_BMDS
                        int64_t at = p->base;
                        if (p->dsp >= 0) at += int64_t(playedFrames(p) - p->playedAtBase);
                        if (p->frames > 0) {
                                pthread_mutex_lock(&p->lock);
                                const bool loop = p->loop;
                                pthread_mutex_unlock(&p->lock);
                                if (loop) at %= p->frames;
                                if (at > p->frames) at = p->frames;
                        }
                        return at;
#else
                        return p->base;
#endif
                }
                void playerPosition(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
                        gen->SetReturnFloat(p->rate != 0 ? float(double(positionOf(p)) / p->rate) : 0.0f);
                }
                void playerSetPosition(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        AudioPlayer *p = player(gen);
                        double       seconds = gen->GetArgFloat(0);
                        if (seconds < 0) seconds = 0;
                        int64_t frame = int64_t(seconds * p->rate);
                        if (p->frames >= 0 && frame > p->frames) frame = p->frames;
                        (void)settle(p);
                        pthread_mutex_lock(&p->lock);
                        const bool running = p->running;
                        pthread_mutex_unlock(&p->lock);
                        const bool paused = p->paused;
                        if (running) halt(p);
                        if (lseek(p->decoder, off_t(frame * p->frameBytes), SEEK_SET) < 0) {
                                raise("Can't go to %.2f s: %s", seconds, strerror(errno));
                                return;
                        }
                        p->base = frame;
                        p->playedAtBase = playedFrames(p);
                        if (running && start(p)) {
                                if (paused) {
                                        (void)bmds_audio_control(p->dsp, BMDS_AUDIO_PAUSE, 0);
                                        p->paused = true;
                                }
                        } else if (running) {
                                unkeepPlayer(p);
                        }
#else
                        (void)gen;
#endif
                }
                void playerDuration(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
                        gen->SetReturnFloat(p->frames >= 0 && p->rate != 0 ? float(double(p->frames) / p->rate)
                                                                           : -1.0f);
                }
                void playerVolume(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(player(gen)->volume);
                }
                void playerSetVolume(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
                        p->volume = clampf(gen->GetArgFloat(0), 0, 1);
#ifdef ANGEL_BMDS
                        if (p->dsp >= 0) (void)bmds_audio_control(p->dsp, BMDS_AUDIO_GAIN, gainOf(p->volume));
#endif
                }
                void playerLoop(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
                        pthread_mutex_lock(&p->lock);
                        const bool loop = p->loop;
                        pthread_mutex_unlock(&p->lock);
                        gen->SetReturnByte(loop);
                }
                void playerSetLoop(asIScriptGeneric *gen) {
                        AudioPlayer *p = player(gen);
                        pthread_mutex_lock(&p->lock);
                        p->loop = gen->GetArgByte(0) != 0;
                        pthread_mutex_unlock(&p->lock);
                }
                void playerCodec(asIScriptGeneric *gen) {
                        returnString(gen, player(gen)->codec);
                }
                void playerRate(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(player(gen)->rate);
                }
                void playerChannels(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(player(gen)->channels);
                }
                void playerBitrate(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(player(gen)->bitrate);
                }
                void playerTag(asIScriptGeneric *gen) {
                        String *out = new (gen->GetAddressOfReturnLocation()) String();
#ifdef ANGEL_BMDS
                        AudioPlayer *p = player(gen);
                        const char  *want = argString(gen, 0).cStr();
                        char         key[32], value[256];
                        for (uint32_t i = 0;; ++i) {
                                const int32_t n =
                                        bmds_audio_decode_tag(p->decoder, i, key, sizeof(key), value, sizeof(value));
                                if (n < 0) break;
                                if (strcasecmp(key, want) != 0) continue;
                                out->assign(value, strlen(value));
                                break;
                        }
#else
                        (void)gen;
                        (void)out;
#endif
                }
                void playerTagNames(asIScriptGeneric *gen) {
                        CScriptArray *names = CScriptArray::Create(stringArrayType(gen->GetEngine()));
#ifdef ANGEL_BMDS
                        AudioPlayer *p = player(gen);
                        char         key[32];
                        for (uint32_t i = 0; bmds_audio_decode_tag(p->decoder, i, key, sizeof(key), nullptr, 0) >= 0;
                             ++i) {
                                String k(key);
                                names->InsertLast(&k);
                        }
#endif
                        gen->SetReturnAddress(names);
                }
                void playerOnFinished(asIScriptGeneric *gen) {
                        AudioPlayer       *p = player(gen);
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (p->onFinished != nullptr) p->onFinished->Release();
                        p->onFinished = fn;
                        if (fn != nullptr && !listen()) return;
                        updateHold();
                }

                // ---- The mixer: the default device's volume and mute (not saved)

                int mixerObject; // `mixer`'s address: the type has no state of its own

#ifdef ANGEL_BMDS
                // The default device's setting @p name (volume, mute): its id, or -1.
                int32_t deviceSetting(const char *name) {
                        char          device[32] = "dsp0";
                        const int32_t d = bmds_sysconf_find("audio/default");
                        if (d >= 0 && bmds_sysconf_get(d, BMDS_SYSCONF_FIELD_VALUE, device, sizeof(device)) <= 0)
                                strcpy(device, "dsp0");
                        char key[64];
                        snprintf(key, sizeof(key), "audio/%s/%s", device, name);
                        return bmds_sysconf_find(key);
                }
                void readSetting(const char *name, char *out, size_t size) {
                        out[0] = 0;
                        const int32_t id = deviceSetting(name);
                        if (id < 0 || bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, out, uint32_t(size)) < 0)
                                out[0] = 0;
                        return;
                }
                void writeSetting(const char *name, const char *value) {
                        const int32_t id = deviceSetting(name);
                        const int32_t r = id < 0 ? id : bmds_sysconf_set(id, BMDS_SYSCONF_OP_SET, value);
                        if (r < 0) raise("Can't set the mixer's %s to %s: %s", name, value, strerror(-r));
                        return;
                }
#endif
                void mixerVolume(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        char value[16];
                        readSetting("volume", value, sizeof(value));
                        gen->SetReturnDWord(asDWORD(atoi(value)));
#else
                        gen->SetReturnDWord(0);
#endif
                }
                void mixerSetVolume(asIScriptGeneric *gen) {
                        int db = int(gen->GetArgDWord(0));
                        db = db < -60 ? -60 : (db > 0 ? 0 : db);
#ifdef ANGEL_BMDS
                        char value[16];
                        snprintf(value, sizeof(value), "%d", db);
                        writeSetting("volume", value);
#else
                        raise("No mixer here (%d dB)", db);
#endif
                }
                void mixerMuted(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        char value[16];
                        readSetting("mute", value, sizeof(value));
                        gen->SetReturnByte(strcmp(value, "true") == 0);
#else
                        gen->SetReturnByte(0);
#endif
                }
                void mixerSetMuted(asIScriptGeneric *gen) {
#ifdef ANGEL_BMDS
                        writeSetting("mute", gen->GetArgByte(0) != 0 ? "true" : "false");
#else
                        (void)gen;
                        raise("No mixer here");
#endif
                }

                // ---- Voice

                struct Said { // a text queued, or said
                                Said *next;
                                char  text[1];
                };

                class Voice {
                        public:
                                ~Voice();
                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }

                                String             name; // "" the default
                                int                speed = 100, pitch = 100, volume = 100;
                                bool               kept = false;
                                asIScriptFunction *onSaid = nullptr;

                                // Shared with the thread, under the lock.
                                pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
                                Said           *queued = nullptr; // to say, oldest first
                                Said           *said = nullptr;   // heard, not yet told, oldest first
                                bool            running = false;
                                bool            joinable = false;
                                bool            stopping = false;
                                int             dsp = -1; // the thread's stream (a stop drops it)
                                pthread_t       thread{};

                        private:
                                int _refs = 1;
                };

                Voice *keptVoices[MaxKept];
                size_t keptVoiceCount = 0;

                Voice *voice(asIScriptGeneric *gen) {
                        return static_cast<Voice *>(gen->GetObject());
                }

                void freeList(Said *s) {
                        while (s != nullptr) {
                                Said *next = s->next;
                                free(s);
                                s = next;
                        }
                        return;
                }
                void append(Said *&list, Said *s) {
                        Said **at = &list;
                        while (*at != nullptr) at = &(*at)->next;
                        *at = s;
                        return;
                }

#ifdef ANGEL_BMDS
                bmds_speak_config_t speakConfig(const Voice *v) {
                        bmds_speak_config_t c{};
                        c.size = sizeof(c);
                        c.flags = BMDS_O_CLOEXEC;
                        c.voice = v->name.length() != 0 ? v->name.cStr() : nullptr;
                        c.speed = uint32_t(v->speed);
                        c.pitch = uint32_t(v->pitch);
                        c.volume = uint32_t(v->volume);
                        return c;
                }

                void *speakThread(void *arg) {
                        auto                     *v = static_cast<Voice *>(arg);
                        const bmds_speak_config_t c = speakConfig(v);
                        const int32_t             speech = bmds_audio_speak(&c);
                        int                       dsp = open("/dev/dsp", O_WRONLY | O_CLOEXEC);
                        bmds_audio_config_t       a = {
                                sizeof(bmds_audio_config_t), BMDS_AUDIO_S16LE, 1, 16000, 32768, 4000, 0, 0};
                        if (dsp >= 0 && bmds_audio_config(dsp, &a, nullptr) < 0) {
                                close(dsp);
                                dsp = -1;
                        }
                        pthread_mutex_lock(&v->lock);
                        v->dsp = dsp;
                        pthread_mutex_unlock(&v->lock);
                        int16_t samples[256];
                        for (;;) {
                                pthread_mutex_lock(&v->lock);
                                Said *s = v->stopping ? nullptr : v->queued;
                                if (s != nullptr) v->queued = s->next;
                                pthread_mutex_unlock(&v->lock);
                                if (s == nullptr) break;
                                s->next = nullptr;
                                // The text and its NUL (said in full), the samples as they come.
                                const char *at = s->text;
                                size_t      left = strlen(s->text) + 1;
                                bool        ok = speech >= 0 && dsp >= 0;
                                while (ok) {
                                        pthread_mutex_lock(&v->lock);
                                        const bool stop = v->stopping;
                                        pthread_mutex_unlock(&v->lock);
                                        if (stop) break;
                                        if (left != 0) {
                                                const ssize_t w = write(speech, at, left);
                                                if (w > 0) {
                                                        at += w;
                                                        left -= size_t(w);
                                                } else if (w < 0 && errno != EAGAIN) {
                                                        break;
                                                }
                                        }
                                        const ssize_t n = read(speech, samples, sizeof(samples));
                                        if (n < 0) break;
                                        if (n == 0) {
                                                if (left == 0) {
                                                        // All of it made: heard once the stream drains.
                                                        ok = bmds_audio_control(dsp, BMDS_AUDIO_DRAIN, 0) == 0;
                                                        break;
                                                }
                                                continue;
                                        }
                                        for (ssize_t done = 0; done < n;) {
                                                const ssize_t w = write(dsp, reinterpret_cast<char *>(samples) + done,
                                                                        size_t(n - done));
                                                if (w <= 0) {
                                                        ok = false;
                                                        break;
                                                }
                                                done += w;
                                        }
                                }
                                pthread_mutex_lock(&v->lock);
                                if (!v->stopping)
                                        append(v->said, s); // told, even if it couldn't be heard
                                else
                                        free(s);
                                pthread_mutex_unlock(&v->lock);
                                tellLoop();
                        }
                        pthread_mutex_lock(&v->lock);
                        v->dsp = -1;
                        v->running = false;
                        pthread_mutex_unlock(&v->lock);
                        if (speech >= 0) close(speech);
                        if (dsp >= 0) close(dsp);
                        tellLoop();
                        return nullptr;
                }
#endif

                // The thread ended and joined, if it has ended.
                void settleVoice(Voice *v) {
#ifdef ANGEL_BMDS
                        pthread_mutex_lock(&v->lock);
                        const bool ended = !v->running && v->joinable;
                        pthread_mutex_unlock(&v->lock);
                        if (!ended) return;
                        pthread_join(v->thread, nullptr);
                        v->joinable = false;
#else
                        (void)v;
#endif
                        return;
                }

                void haltVoice(Voice *v) {
                        pthread_mutex_lock(&v->lock);
                        v->stopping = true;
                        freeList(v->queued);
                        v->queued = nullptr;
#ifdef ANGEL_BMDS
                        if (v->dsp >= 0) (void)bmds_audio_control(v->dsp, BMDS_AUDIO_DROP, 0);
#endif
                        const bool joinable = v->joinable;
                        pthread_mutex_unlock(&v->lock);
                        if (joinable) pthread_join(v->thread, nullptr);
                        pthread_mutex_lock(&v->lock);
                        v->joinable = v->running = v->stopping = false;
                        pthread_mutex_unlock(&v->lock);
                        return;
                }

                Voice::~Voice() {
                        haltVoice(this);
                        freeList(said);
                        if (onSaid != nullptr) onSaid->Release();
                        pthread_mutex_destroy(&lock);
                }

                bool voiceBusy(Voice *v);
                void unkeepVoice(Voice *v);

                void keepVoice(Voice *v) {
                        if (v->kept) return;
                        for (size_t i = keptVoiceCount; keptVoiceCount == MaxKept && i-- > 0;) {
                                Voice *w = keptVoices[i];
                                settleVoice(w);
                                pthread_mutex_lock(&w->lock);
                                const bool told = w->onSaid == nullptr || w->said == nullptr;
                                pthread_mutex_unlock(&w->lock);
                                if (told && !voiceBusy(w)) unkeepVoice(w);
                        }
                        if (keptVoiceCount == MaxKept) {
                                raise("Too many voices speaking (%u)", unsigned(MaxKept));
                                return;
                        }
                        v->addRef();
                        v->kept = true;
                        keptVoices[keptVoiceCount++] = v;
                        updateHold();
                        return;
                }
                void unkeepVoice(Voice *v) {
                        for (size_t i = 0; i < keptVoiceCount; ++i) {
                                if (keptVoices[i] != v) continue;
                                keptVoices[i] = keptVoices[--keptVoiceCount];
                                v->kept = false;
                                updateHold();
                                v->release();
                                return;
                        }
                        return;
                }

                void voiceMake(asIScriptGeneric *gen) {
                        const String &name = argString(gen, 0);
#ifdef ANGEL_BMDS
                        Voice *v = new Voice();
                        v->name = name;
                        // There: the speaker opened once (a voice not found raises now, not later).
                        const bmds_speak_config_t c = speakConfig(v);
                        const int32_t             fd = bmds_audio_speak(&c);
                        if (fd < 0) {
                                v->release();
                                if (fd == -BMDS_ENOENT)
                                        raise("No voice '%s' (voices() lists them)", name.cStr());
                                else
                                        raise("Can't speak: %s", strerror(-fd));
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        close(fd);
                        gen->SetReturnAddress(v);
#else
                        Voice *v = new Voice();
                        v->name = name;
                        gen->SetReturnAddress(v);
#endif
                }
                void voiceAddRef(asIScriptGeneric *gen) {
                        voice(gen)->addRef();
                }
                void voiceRelease(asIScriptGeneric *gen) {
                        voice(gen)->release();
                }
                void voiceName(asIScriptGeneric *gen) {
                        new (gen->GetAddressOfReturnLocation()) String(voice(gen)->name);
                }

                // A setting of the voice's within its limits (applied at its next text).
                void voiceInt(asIScriptGeneric *gen, int Voice::*field, int lo, int hi, const char *what) {
                        const int v = int(gen->GetArgDWord(0));
                        if (v < lo || v > hi) {
                                raise("The %s %d isn't %d to %d", what, v, lo, hi);
                                return;
                        }
                        voice(gen)->*field = v;
                }
                void voiceSpeed(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(voice(gen)->speed));
                }
                void voiceSetSpeed(asIScriptGeneric *gen) {
                        voiceInt(gen, &Voice::speed, 20, 500, "speed");
                }
                void voicePitch(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(voice(gen)->pitch));
                }
                void voiceSetPitch(asIScriptGeneric *gen) {
                        voiceInt(gen, &Voice::pitch, 50, 200, "pitch");
                }
                void voiceVolume(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(voice(gen)->volume));
                }
                void voiceSetVolume(asIScriptGeneric *gen) {
                        voiceInt(gen, &Voice::volume, 1, 500, "volume");
                }

                // Queued; the thread started if it isn't running. False with the problem raised.
                bool speak(Voice *v, const String &text) {
                        if (!listen()) return false;
                        Said *s = static_cast<Said *>(malloc(sizeof(Said) + text.length()));
                        if (s == nullptr) {
                                raise("Out of memory");
                                return false;
                        }
                        s->next = nullptr;
                        memcpy(s->text, text.data(), text.length());
                        s->text[text.length()] = 0;
#ifdef ANGEL_BMDS
                        settleVoice(v);
                        pthread_mutex_lock(&v->lock);
                        append(v->queued, s);
                        bool made = true;
                        if (!v->running) {
                                v->running = true;
                                made = pthread_create(&v->thread, nullptr, speakThread, v) == 0;
                                v->running = v->joinable = made;
                                if (!made) {
                                        freeList(v->queued);
                                        v->queued = nullptr;
                                }
                        }
                        pthread_mutex_unlock(&v->lock);
                        if (!made) {
                                raise("Can't speak: no thread");
                                return false;
                        }
#else
                        // Nothing heard on the host: said at once, told in the loop.
                        pthread_mutex_lock(&v->lock);
                        append(v->said, s);
                        pthread_mutex_unlock(&v->lock);
                        tellLoop();
#endif
                        keepVoice(v);
                        return true;
                }

                bool voiceBusy(Voice *v) {
                        pthread_mutex_lock(&v->lock);
                        const bool busy = v->running || v->queued != nullptr;
                        pthread_mutex_unlock(&v->lock);
                        return busy;
                }

                void voiceSpeak(asIScriptGeneric *gen) {
                        (void)speak(voice(gen), argString(gen, 0));
                }
                void voiceWait(asIScriptGeneric *gen) {
                        Voice *v = voice(gen);
                        fflush(stdout);
#ifdef ANGEL_BMDS
                        pthread_mutex_lock(&v->lock);
                        const bool joinable = v->joinable;
                        pthread_mutex_unlock(&v->lock);
                        if (joinable) {
                                pthread_join(v->thread, nullptr);
                                v->joinable = false;
                        }
#endif
                        // What was said isn't told (waited for).
                        pthread_mutex_lock(&v->lock);
                        freeList(v->said);
                        v->said = nullptr;
                        pthread_mutex_unlock(&v->lock);
                        unkeepVoice(v);
                }
                void voiceSay(asIScriptGeneric *gen) {
                        if (!speak(voice(gen), argString(gen, 0))) return;
                        voiceWait(gen);
                }
                void voiceStop(asIScriptGeneric *gen) {
                        Voice *v = voice(gen);
                        haltVoice(v);
                        pthread_mutex_lock(&v->lock);
                        freeList(v->said);
                        v->said = nullptr;
                        pthread_mutex_unlock(&v->lock);
                        unkeepVoice(v);
                }
                void voiceSpeaking(asIScriptGeneric *gen) {
                        gen->SetReturnByte(voiceBusy(voice(gen)));
                }
                void voiceOnSaid(asIScriptGeneric *gen) {
                        Voice             *v = voice(gen);
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (v->onSaid != nullptr) v->onSaid->Release();
                        v->onSaid = fn;
                        if (fn != nullptr && !listen()) return;
                        updateHold();
                }

                // voices(): the names in speech/path's directories (NAME_ta.bin), each once.
                void voicesList(asIScriptGeneric *gen) {
                        CScriptArray *names = CScriptArray::Create(stringArrayType(gen->GetEngine()));
#ifdef ANGEL_BMDS
                        char          path[256] = "/rom/share/speech";
                        const int32_t id = bmds_sysconf_find("speech/path");
                        if (id >= 0 && bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, path, sizeof(path)) < 0)
                                strcpy(path, "/rom/share/speech");
                        for (char *dir = path; dir != nullptr && *dir != 0;) {
                                char *colon = strchr(dir, ':');
                                if (colon != nullptr) *colon = 0;
                                if (DIR *d = opendir(dir)) {
                                        while (struct dirent *e = readdir(d)) {
                                                const size_t n = strlen(e->d_name);
                                                if (n <= 7 || strcmp(e->d_name + n - 7, "_ta.bin") != 0) continue;
                                                String name(e->d_name, n - 7);
                                                bool   seen = false;
                                                for (asUINT i = 0; i < names->GetSize() && !seen; ++i)
                                                        seen = strcmp(static_cast<String *>(names->At(i))->cStr(),
                                                                      name.cStr()) == 0;
                                                if (!seen) names->InsertLast(&name);
                                        }
                                        closedir(d);
                                }
                                dir = colon != nullptr ? colon + 1 : nullptr;
                        }
#endif
                        gen->SetReturnAddress(names);
                }

                // ---- MidiPort

                class MidiPort {
                        public:
                                ~MidiPort() { shut(); }
                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                        return;
                                }
                                void quiet() {
                                        if (listed) removeLoopSource(&source);
                                        listed = false;
                                        if (onMessage != nullptr) onMessage->Release();
                                        onMessage = nullptr;
                                        return;
                                }
                                void shut() {
                                        quiet();
                                        if (source.fd >= 0) close(source.fd);
                                        source.fd = -1;
                                        return;
                                }

                                String             name;
                                int                number = -1;
                                LoopSource         source{-1, nullptr, nullptr, LoopHold::Run};
                                bool               listed = false;
                                asIScriptFunction *onMessage = nullptr;

                        private:
                                int _refs = 1;
                };

                constexpr size_t MaxPorts = 8; // with callbacks
                MidiPort        *keptPorts[MaxPorts];
                size_t           keptPortCount = 0;

                MidiPort *port(asIScriptGeneric *gen) {
                        return static_cast<MidiPort *>(gen->GetObject());
                }

#ifdef ANGEL_BMDS
                constexpr int MaxPortNumber = 32;

                // A port opened for the sides it has (a synth's has only a destination: read, it
                // refuses), non-blocking. @return The fd, or -1 with errno.
                int openPort(const char *path) {
                        int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
                        if (fd < 0) fd = open(path, O_WRONLY | O_CLOEXEC | O_NONBLOCK);
                        if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
                        return fd;
                }

                // Port @p n's name into @p out; false if it isn't there.
                bool portName(int n, char *out, size_t size) {
                        char path[24];
                        snprintf(path, sizeof(path), "/dev/midi%d", n);
                        const int fd = openPort(path);
                        if (fd < 0) return false;
                        bmds_midi_status_t s{};
                        s.size = sizeof(s);
                        const bool ok = bmds_midi_status(fd, &s) == 0;
                        close(fd);
                        if (ok) snprintf(out, size, "%s", s.name);
                        return ok;
                }
#endif

                void midiPorts(asIScriptGeneric *gen) {
                        CScriptArray *names = CScriptArray::Create(stringArrayType(gen->GetEngine()));
#ifdef ANGEL_BMDS
                        char name[BMDS_MIDI_NAME_MAX];
                        for (int n = 0; n < MaxPortNumber; ++n) {
                                if (!portName(n, name, sizeof(name))) continue;
                                String s(name);
                                names->InsertLast(&s);
                        }
#endif
                        gen->SetReturnAddress(names);
                }

                void portMake(asIScriptGeneric *gen) {
                        const String &want = argString(gen, 0);
#ifdef ANGEL_BMDS
                        int  number = -1;
                        char name[BMDS_MIDI_NAME_MAX];
                        if (strncmp(want.cStr(), "midi", 4) == 0 && want.length() > 4) {
                                char      *end = nullptr;
                                const long n = strtol(want.cStr() + 4, &end, 10);
                                if (*end == 0 && n >= 0 && n < MaxPortNumber && portName(int(n), name, sizeof(name)))
                                        number = int(n);
                        }
                        for (int n = 0; number < 0 && n < MaxPortNumber; ++n)
                                if (portName(n, name, sizeof(name)) && strcmp(name, want.cStr()) == 0) number = n;
                        if (number < 0) {
                                raise("No MIDI port '%s' (midiPorts() lists them)", want.cStr());
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        char path[24];
                        snprintf(path, sizeof(path), "/dev/midi%d", number);
                        const int          fd = openPort(path);
                        bmds_midi_config_t c{};
                        c.size = sizeof(c);
                        c.flags = BMDS_MIDI_EVENTS;
                        c.event_size = sizeof(bmds_midi_event_t);
                        const int32_t r = fd < 0 ? -errno : bmds_midi_config(fd, &c, nullptr);
                        if (r < 0) {
                                if (fd >= 0) close(fd);
                                raise("Can't open MIDI port '%s': %s", want.cStr(), strerror(-r));
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        MidiPort *p = new MidiPort();
                        p->name.assign(name, strlen(name));
                        p->number = number;
                        p->source.fd = fd;
                        p->source.context = p;
                        gen->SetReturnAddress(p);
#else
                        raise("No MIDI port '%s' here", want.cStr());
                        gen->SetReturnAddress(nullptr);
#endif
                }
                void portAddRef(asIScriptGeneric *gen) {
                        port(gen)->addRef();
                }
                void portRelease(asIScriptGeneric *gen) {
                        port(gen)->release();
                }
                void portName(asIScriptGeneric *gen) {
                        new (gen->GetAddressOfReturnLocation()) String(port(gen)->name);
                }
                void portNumber(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(asDWORD(port(gen)->number));
                }

                void sendMessage(MidiPort *p, const uint8_t *data, uint8_t length) {
                        if (p->source.fd < 0) {
                                raise("The MIDI port '%s' is closed", p->name.cStr());
                                return;
                        }
#ifdef ANGEL_BMDS
                        bmds_midi_event_t e{};
                        e.length = length;
                        memcpy(e.data, data, length);
                        if (write(p->source.fd, &e, sizeof(e)) != ssize_t(sizeof(e)))
                                raise("Can't send to MIDI port '%s': %s", p->name.cStr(), strerror(errno));
#else
                        (void)data;
                        (void)length;
#endif
                }
                // A channel message: @p status's high nibble, channel 1 to 16.
                void channelMessage(asIScriptGeneric *gen, uint8_t kind, int dataCount, int data1, int data2) {
                        const int channel = int(gen->GetArgDWord(0));
                        if (channel < 1 || channel > 16 || data1 < 0 || data1 > 127 || data2 < 0 || data2 > 127) {
                                raise("A MIDI channel is 1 to 16, its values 0 to 127");
                                return;
                        }
                        const uint8_t m[3] = {uint8_t(kind | (channel - 1)), uint8_t(data1), uint8_t(data2)};
                        sendMessage(port(gen), m, uint8_t(1 + dataCount));
                }
                void portSend(asIScriptGeneric *gen) {
                        const int status = int(gen->GetArgDWord(0)), d1 = int(gen->GetArgDWord(1)),
                                  d2 = int(gen->GetArgDWord(2));
                        if (status < 0x80 || status > 0xff || d1 < 0 || d1 > 127 || d2 < 0 || d2 > 127 ||
                            status == 0xf0 || status == 0xf7) {
                                raise("Not a MIDI message: %d %d %d", status, d1, d2);
                                return;
                        }
                        // Its length from its status (system real-time: the byte alone).
                        uint8_t   length = 3;
                        const int kind = status & 0xf0;
                        if (kind == 0xc0 || kind == 0xd0 || status == 0xf1 || status == 0xf3) length = 2;
                        if (status >= 0xf4 && status != 0xf2) length = 1;
                        const uint8_t m[3] = {uint8_t(status), uint8_t(d1), uint8_t(d2)};
                        sendMessage(port(gen), m, length);
                }
                void portNoteOn(asIScriptGeneric *gen) {
                        channelMessage(gen, 0x90, 2, int(gen->GetArgDWord(1)), int(gen->GetArgDWord(2)));
                }
                void portNoteOff(asIScriptGeneric *gen) {
                        channelMessage(gen, 0x80, 2, int(gen->GetArgDWord(1)), 0);
                }
                void portControl(asIScriptGeneric *gen) {
                        channelMessage(gen, 0xb0, 2, int(gen->GetArgDWord(1)), int(gen->GetArgDWord(2)));
                }
                void portProgram(asIScriptGeneric *gen) {
                        channelMessage(gen, 0xc0, 1, int(gen->GetArgDWord(1)), 0);
                }

                // The loop: messages from the port's source.
                void portReady(LoopSource *source) {
#ifdef ANGEL_BMDS
                        auto             *p = static_cast<MidiPort *>(source->context);
                        bmds_midi_event_t events[16];
                        const ssize_t     n = read(source->fd, events, sizeof(events));
                        if (n <= 0) return;
                        p->addRef();
                        for (ssize_t i = 0; i < n / ssize_t(sizeof(events[0])) && p->onMessage != nullptr; ++i) {
                                const bmds_midi_event_t &e = events[i];
                                if (e.length == 0 || e.data[0] >= 0xf0) continue; // SysEx, real-time
                                const CallArg      args[3] = {{nullptr, 0, e.data[0]},
                                                              {nullptr, 0, e.length > 1 ? e.data[1] : 0},
                                                              {nullptr, 0, e.length > 2 ? e.data[2] : 0}};
                                asIScriptFunction *fn = p->onMessage;
                                fn->AddRef();
                                callScript(fn, args, 3);
                                fn->Release();
                        }
                        p->release();
#else
                        (void)source;
#endif
                }

                void unkeepPort(MidiPort *p) {
                        for (size_t i = 0; i < keptPortCount; ++i) {
                                if (keptPorts[i] != p) continue;
                                keptPorts[i] = keptPorts[--keptPortCount];
                                p->quiet();
                                p->release();
                                return;
                        }
                        p->quiet();
                        return;
                }
                void portOnMessage(asIScriptGeneric *gen) {
                        MidiPort          *p = port(gen);
                        asIScriptFunction *fn = static_cast<asIScriptFunction *>(gen->GetArgObject(0));
                        if (fn == nullptr) {
                                unkeepPort(p);
                                return;
                        }
                        if (p->source.fd < 0) {
                                fn->Release();
                                raise("The MIDI port '%s' is closed", p->name.cStr());
                                return;
                        }
                        if (p->onMessage != nullptr) {
                                p->onMessage->Release();
                                p->onMessage = fn;
                                return;
                        }
                        if (keptPortCount == MaxPorts) {
                                fn->Release();
                                raise("Too many MIDI ports called back (%u)", unsigned(MaxPorts));
                                return;
                        }
                        p->onMessage = fn;
                        p->source.ready = portReady;
                        if (!addLoopSource(&p->source)) {
                                p->quiet();
                                return;
                        }
                        p->listed = true;
                        p->addRef();
                        keptPorts[keptPortCount++] = p;
                }
                void portClose(asIScriptGeneric *gen) {
                        MidiPort *p = port(gen);
                        unkeepPort(p);
                        p->shut();
                }

                // ---- The loop: what the threads told

                void updateHold() {
                        bool hold = false;
                        for (size_t i = 0; i < keptPlayerCount && !hold; ++i)
                                hold = keptPlayers[i]->onFinished != nullptr;
                        for (size_t i = 0; i < keptVoiceCount && !hold; ++i) hold = keptVoices[i]->onSaid != nullptr;
                        wakeSource.hold = hold ? LoopHold::Run : LoopHold::Nothing;
                        return;
                }

                void wakeReady(LoopSource *source) {
                        char drain[32];
                        while (read(source->fd, drain, sizeof(drain)) > 0) {}
                        // Players that ended: their finish told, then let go of.
                        for (size_t i = keptPlayerCount; i-- > 0;) {
                                if (i >= keptPlayerCount) continue; // a callback let go of others
                                AudioPlayer *p = keptPlayers[i];
                                p->addRef();
                                const bool finished = settle(p);
                                pthread_mutex_lock(&p->lock);
                                const bool running = p->running;
                                pthread_mutex_unlock(&p->lock);
                                if (!running) unkeepPlayer(p);
                                if (finished && p->onFinished != nullptr) {
                                        asIScriptFunction *fn = p->onFinished;
                                        fn->AddRef();
                                        callScript(fn, nullptr, 0);
                                        fn->Release();
                                }
                                p->release();
                        }
                        // Voices: each text heard told; one silent and told everything let go of.
                        for (size_t i = keptVoiceCount; i-- > 0;) {
                                if (i >= keptVoiceCount) continue;
                                Voice *v = keptVoices[i];
                                v->addRef();
                                for (;;) {
                                        pthread_mutex_lock(&v->lock);
                                        Said *s = v->said;
                                        if (s != nullptr) v->said = s->next;
                                        pthread_mutex_unlock(&v->lock);
                                        if (s == nullptr) break;
                                        if (v->onSaid != nullptr) {
                                                const CallArg      arg{s->text, strlen(s->text), 0};
                                                asIScriptFunction *fn = v->onSaid;
                                                fn->AddRef();
                                                callScript(fn, &arg, 1);
                                                fn->Release();
                                        }
                                        free(s);
                                }
                                settleVoice(v);
                                if (!voiceBusy(v)) unkeepVoice(v);
                                v->release();
                        }
                        return;
                }

        } // namespace

        void registerAudio(asIScriptEngine *engine) {
                check(engine->RegisterObjectType("AudioPlayer", 0, asOBJ_REF), "AudioPlayer");
                behaviour(engine, "AudioPlayer", asBEHAVE_FACTORY, "AudioPlayer@ f(const string &in path)", playerMake,
                          "A sound file to play (WAV, MP3, Ogg Vorbis, FLAC, MIDI): play() plays it in the "
                          "background.");
                behaviour(engine, "AudioPlayer", asBEHAVE_ADDREF, "void f()", playerAddRef, nullptr);
                behaviour(engine, "AudioPlayer", asBEHAVE_RELEASE, "void f()", playerRelease, nullptr);
                method(engine, "AudioPlayer", "void play()", playerPlay,
                       "Plays it in the background (after pause(): goes on; after its end: from the start).");
                method(engine, "AudioPlayer", "void pause()", playerPause, "Pauses it where it is.");
                method(engine, "AudioPlayer", "void stop()", playerStop, "Stops it, back at the start.");
                method(engine, "AudioPlayer", "void wait()", playerWait, "Waits until it has played to its end.");
                method(engine, "AudioPlayer", "bool get_playing() const property", playerPlaying,
                       "It is playing (not paused, stopped or ended).");
                method(engine, "AudioPlayer", "float get_position() const property", playerPosition,
                       "Where it plays, in seconds.");
                method(engine, "AudioPlayer", "void set_position(float seconds) property", playerSetPosition,
                       "Goes to a place in it, in seconds.");
                method(engine, "AudioPlayer", "float get_duration() const property", playerDuration,
                       "How long it is, in seconds (-1 if that isn't known).");
                method(engine, "AudioPlayer", "float get_volume() const property", playerVolume, "Its volume, 0 to 1.");
                method(engine, "AudioPlayer", "void set_volume(float volume) property", playerSetVolume,
                       "Changes its volume, 0 to 1.");
                method(engine, "AudioPlayer", "bool get_loop() const property", playerLoop,
                       "It starts again at its end.");
                method(engine, "AudioPlayer", "void set_loop(bool loop) property", playerSetLoop,
                       "Makes it start again at its end (or not).");
                method(engine, "AudioPlayer", "string get_codec() const property", playerCodec,
                       "Its coding: wav, mp3, vorbis, flac, midi.");
                method(engine, "AudioPlayer", "int get_rate() const property", playerRate, "Samples a second.");
                method(engine, "AudioPlayer", "int get_channels() const property", playerChannels,
                       "1 (mono) or 2 (stereo).");
                method(engine, "AudioPlayer", "int get_bitrate() const property", playerBitrate,
                       "The file's bits a second (0 if not known).");
                method(engine, "AudioPlayer", "string tag(const string &in name) const", playerTag,
                       "One of its tags (TITLE, ARTIST, ALBUM, DATE...): \"\" if it hasn't that one.");
                method(engine, "AudioPlayer", "string[]@ tagNames() const", playerTagNames, "The names of its tags.");
                method(engine, "AudioPlayer", "void onFinished(Callback@ callback)", playerOnFinished,
                       "Calls back when it has played to its end (not when stopped), while app.run() runs.");

                check(engine->RegisterObjectType("Mixer", 0, asOBJ_REF | asOBJ_NOCOUNT), "Mixer");
                method(engine, "Mixer", "int get_volume() const property", mixerVolume,
                       "The sound's volume, in dB below full (-60 to 0).");
                method(engine, "Mixer", "void set_volume(int db) property", mixerSetVolume,
                       "Changes the volume (-60 to 0 dB), as the panel's control does; not saved.");
                method(engine, "Mixer", "bool get_muted() const property", mixerMuted, "The sound is muted.");
                method(engine, "Mixer", "void set_muted(bool muted) property", mixerSetMuted,
                       "Mutes the sound (or not); not saved.");
                property(engine, "Mixer mixer", &mixerObject, "The default sound device's volume.");

                check(engine->RegisterObjectType("Voice", 0, asOBJ_REF), "Voice");
                behaviour(engine, "Voice", asBEHAVE_FACTORY, "Voice@ f(const string &in name = \"\")", voiceMake,
                          "A voice to speak with (\"\": the default; voices() lists them).");
                behaviour(engine, "Voice", asBEHAVE_ADDREF, "void f()", voiceAddRef, nullptr);
                behaviour(engine, "Voice", asBEHAVE_RELEASE, "void f()", voiceRelease, nullptr);
                method(engine, "Voice", "string get_name() const property", voiceName,
                       "The voice's name (\"\": the default).");
                method(engine, "Voice", "int get_speed() const property", voiceSpeed, "How fast it speaks, percent.");
                method(engine, "Voice", "void set_speed(int percent) property", voiceSetSpeed,
                       "Changes how fast it speaks: 20 to 500 percent.");
                method(engine, "Voice", "int get_pitch() const property", voicePitch, "How high it speaks, percent.");
                method(engine, "Voice", "void set_pitch(int percent) property", voiceSetPitch,
                       "Changes how high it speaks: 50 to 200 percent.");
                method(engine, "Voice", "int get_volume() const property", voiceVolume, "How loud it speaks, percent.");
                method(engine, "Voice", "void set_volume(int percent) property", voiceSetVolume,
                       "Changes how loud it speaks: 1 to 500 percent (100: as loud as it goes without clipping).");
                method(engine, "Voice", "void speak(const string &in text)", voiceSpeak,
                       "Says the text in the background, after what it is saying.");
                method(engine, "Voice", "void say(const string &in text)", voiceSay,
                       "Says the text and waits until it has been heard.");
                method(engine, "Voice", "void wait()", voiceWait, "Waits until it has said everything.");
                method(engine, "Voice", "void stop()", voiceStop, "Stops speaking, forgetting what was to come.");
                method(engine, "Voice", "bool get_speaking() const property", voiceSpeaking, "It is speaking.");
                method(engine, "Voice", "void onSaid(StringCallback@ callback)", voiceOnSaid,
                       "Calls back with each text once it has been heard, while app.run() runs.");
                def(engine, "string[]@ voices()", voicesList, "The voices there are to speak with.");

                check(engine->RegisterObjectType("MidiPort", 0, asOBJ_REF), "MidiPort");
                behaviour(engine, "MidiPort", asBEHAVE_FACTORY, "MidiPort@ f(const string &in name)", portMake,
                          "A MIDI port by its name or number (midi1): sends to it, hears what it plays.");
                behaviour(engine, "MidiPort", asBEHAVE_ADDREF, "void f()", portAddRef, nullptr);
                behaviour(engine, "MidiPort", asBEHAVE_RELEASE, "void f()", portRelease, nullptr);
                method(engine, "MidiPort", "string get_name() const property", portName, "Its name.");
                method(engine, "MidiPort", "int get_number() const property", portNumber, "Its number (midiN).");
                method(engine, "MidiPort", "void send(int status, int data1 = 0, int data2 = 0)", portSend,
                       "Sends a MIDI message (0x90 60 100: a note on channel 1).");
                method(engine, "MidiPort", "void noteOn(int channel, int note, int velocity = 100)", portNoteOn,
                       "Starts a note (channel 1 to 16; note 60 is middle C).");
                method(engine, "MidiPort", "void noteOff(int channel, int note)", portNoteOff, "Ends a note.");
                method(engine, "MidiPort", "void control(int channel, int controller, int value)", portControl,
                       "Sends a controller's value (7: volume, 10: pan, 64: sustain).");
                method(engine, "MidiPort", "void program(int channel, int program)", portProgram,
                       "Chooses a channel's instrument (General MIDI's numbers from 0).");
                method(engine, "MidiPort", "void onMessage(IntIntIntCallback@ callback)", portOnMessage,
                       "Calls back with each message it plays (status, data1, data2), while app.run() runs.");
                method(engine, "MidiPort", "void close()", portClose, "Closes it.");
                def(engine, "string[]@ midiPorts()", midiPorts, "The MIDI ports' names.");
                return;
        }

        void releaseAudio() {
                while (keptPlayerCount != 0) {
                        AudioPlayer *p = keptPlayers[keptPlayerCount - 1];
#ifdef ANGEL_BMDS
                        halt(p);
#endif
                        unkeepPlayer(p);
                }
                while (keptVoiceCount != 0) {
                        Voice *v = keptVoices[keptVoiceCount - 1];
                        haltVoice(v);
                        unkeepVoice(v);
                }
                while (keptPortCount != 0) unkeepPort(keptPorts[keptPortCount - 1]);
                if (wakeListed) removeLoopSource(&wakeSource);
                wakeListed = false;
                for (int &fd : wake) {
                        if (fd >= 0) close(fd);
                        fd = -1;
                }
                return;
        }

} // namespace angel
