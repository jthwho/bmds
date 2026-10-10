/**
 * @file      player.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/audiofile.h>
#include <bmds/config.h>
#include <bmds/decoder.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The media player's playback thread (`devplan/mediaplayer.md` §6b):
 *        opens the files it is given, decodes them and writes their sound to
 *        `/dev/dsp`, a period at a time. No toolkit: the loop sends it
 *        commands and takes its reports.
 *
 * **Tracks** are numbered by the loop (never `TrackClock::NoTrack`, 0).
 * `play()` with the number of the track open seeks in it; with another it
 * opens that file. Without `Follow`, what is queued is thrown away first;
 * with it (the next in the list, at a track's `Ended`), the new track goes
 * on after it: into the same stream without a gap when the rate and the
 * channels are the same, else after the stream has drained.
 *
 * **Reports** (`take()`) say where each track lies on the stream (a stream
 * frame and a file frame), so the loop can tell what is heard from the
 * stream's status (`status()`, `TrackClock`). The stream's frames count from
 * 0 when it is configured again; a report that throws away what was queued
 * says so (`Report::fresh`).
 *
 * The device is open only while there is something to play: a stop or the
 * end of the list closes it, and its ring goes.
 *
 * @par Thread Safety
 * `play()`, `pause()`, `stop()`, `finish()`, `setGain()`, `status()` and
 * `take()` from one thread (the loop); the rest is the playback thread's.
 */
class Player {
        public:
                /** @brief Told on the playback thread that a report is waiting. Must not block. */
                using Notify = void (*)(void *arg);

                /** @brief How `play()` starts. */
                enum class Start : uint8_t {
                        Now,    ///< What is queued is thrown away.
                        Follow, ///< After what is queued (gapless where it can be).
                };

                /** @brief The longest tag value a report carries (bytes, cut on a character). */
                static constexpr size_t MaxTag = 127;

                /** @brief What a report says. */
                enum class Event : uint8_t {
                        Started, ///< A track (or a seek in it) goes on the stream at `streamFrame`.
                        Ended,   ///< Its file is all decoded: the next is wanted (`play()` `Follow`, or `finish()`).
                        Failed,  ///< It couldn't be played (`error`; `frame`: where, for damage).
                        Stopped, ///< Nothing plays any more (stopped, or the list's end has played out).
                };

                /** @brief A report from the playback thread. */
                struct Report {
                                Event         event = Event::Stopped;
                                uint32_t      track = 0;
                                Error         error = Error::Ok;
                                bool          fresh = false;   ///< `Started`: the marks before are over.
                                bool          isMidi = false;  ///< `Failed`: the file is a MIDI file.
                                uint64_t      streamFrame = 0; ///< `Started`: the stream's frame it starts at.
                                uint64_t      frame = 0;       ///< `Started`: its file's frame there; `Failed`: where.
                                Decoder::Info info;            ///< `Started`.
                                char          title[MaxTag + 1] = {};
                                char          artist[MaxTag + 1] = {};
                                char          album[MaxTag + 1] = {};
                };

                /** @brief The ring, in milliseconds of sound (a card's pauses covered). */
                static constexpr uint32_t RingMs = 500;

                /** @brief A write's sound, in milliseconds: the longest a command waits. */
                static constexpr uint32_t PeriodMs = 20;

                Player() = default;
                ~Player() { quit(); }
                Player(const Player &) = delete;
                Player &operator=(const Player &) = delete;

                /**
                 * @brief Starts the playback thread, which reports through @p notify
                 *        and opens files in @p context's terms.
                 * @return `Ok`, `NoMem`, or the thread's error.
                 */
                Error start(Notify notify, void *arg, StringView cwd);

                /** @brief Stops and joins the playback thread (sound queued is thrown away). @blocking */
                void quit();

                /**
                 * @brief Plays track @p track, file @p path, from @p startMs.
                 * @return `Ok`, `Full` (commands not taken yet), or `NoMem`.
                 */
                Error play(uint32_t track, StringView path, uint64_t startMs, Start how);

                /** @brief Pauses (the ring kept) or resumes. @return `Ok` or `Full`. */
                Error pause(bool paused);

                /** @brief Stops at once, track @p track (reported as `Stopped`). @return `Ok` or `Full`. */
                Error stop(uint32_t track);

                /** @brief No next track: plays out what is queued, then `Stopped` with @p track. @return `Ok` or `Full`. */
                Error finish(uint32_t track);

                /** @brief Sets the stream's gain (`AudioFile::MutedCentiDb` to 0), now and for later streams. */
                void setGain(int32_t centiDb);

                /** @brief Returns the stream's status (state `Idle` without one). */
                AudioFile::Status status();

                /** @brief Takes the oldest report into @p out. @return False if there is none. */
                bool take(Report &out);

        private:
                enum class Kind : uint8_t {
                        Play,
                        Pause,
                        Resume,
                        Stop,
                        Finish,
                        Quit,
                };

                struct Command {
                                Kind     kind = Kind::Stop;
                                Start    how = Start::Now;
                                uint32_t track = 0;
                                uint64_t startMs = 0;
                                String   path{MemSpace::Psram};
                };

                // What the thread is doing.
                enum class State : uint8_t {
                        Idle,     // nothing open (a stream may play its tail out)
                        Decoding, // writing a track's sound
                        Ended,    // its file decoded: waiting for the next or the end
                };

                static constexpr size_t MaxCommands = 8;
                static constexpr size_t MaxReports = 16;

                static void  entry(void *arg);
                void         run();
                Error        send(Command &&command, bool replaces);
                void         handle(Command &command);
                void         startTrack(const Command &command);
                void         seekTrack(const Command &command);
                void         decodeSome();
                void         dropQueued(); // the stream's queue thrown away; _written follows
                void         drainAndClose();
                void         closeDecoder();
                void         closeDevice();
                Error        openDevice(const Decoder::Info &info);
                void         report(const Report &r);
                void         reportStarted(bool fresh);
                void         reportFailed(uint32_t track, Error error, uint64_t frame, bool isMidi);
                void         reportStopped(uint32_t track);
                static Error readAt(void *arg, uint64_t offset, WritableByteSpan buf);

                // The loop's side.
                Thread           _thread;
                Semaphore        _wake{1, 0};
                Mutex            _commandLock;
                Command          _commands[MaxCommands];
                size_t           _commandHead = 0;
                size_t           _commandCount = 0;
                Mutex            _reportLock;
                Report          *_reports = nullptr; // MaxReports, PSRAM
                size_t           _reportHead = 0;
                size_t           _reportCount = 0;
                Notify           _notify = nullptr;
                void            *_notifyArg = nullptr;
                std::atomic<int> _gain{0};
                Mutex            _deviceLock; // _device's pointer: the loop's status() and drop()
                FileHandle::Ptr  _device;
                AudioFile       *_audio = nullptr;
                String           _cwd{MemSpace::Psram};
                bool             _started = false;

                // The thread's own.
                State             _state = State::Idle;
                bool              _paused = false;
                bool              _quit = false;
                uint32_t          _track = 0; // the track open (0: none)
                String            _path{MemSpace::Psram};
                FileHandle::Ptr   _file;
                uint64_t          _fileSize = 0;
                Decoder          *_decoder = nullptr;
                Decoder::Info     _info;             // the open track's
                AudioFile::Config _config;           // the stream's, while the device is open
                uint64_t          _written = 0;      // the stream's frame the next write goes to
                uint8_t          *_buffer = nullptr; // a period's sound, PSRAM
                size_t            _bufferBytes = 0;
                Command           _taken; // a command being handled
};

BMDS_NAMESPACE_END
