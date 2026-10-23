/**
 * @file      guiclipboard.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/clipboard.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

class GuiApplication;

/**
 * @brief The clipboard for an application on the toolkit
 *        (`devplan/clipboard.md` C13): `GuiApplication::clipboard()`.
 *
 * A copy is text stored now (`copyText()`), or types rendered when another
 * program asks for one (`copy()` with a `Provider`, called on the loop's
 * thread). A paste (`paste()`) is read on a thread of the clipboard's and
 * handed to the loop's thread whole (`Received`), so the application never
 * waits for another program. The bytes a provider renders are written into
 * the asker's pipe on that thread too. It starts at the first copy on
 * request or paste.
 *
 * A copy may also be answered later (`offer()`): the application is told of
 * each request on the loop's thread (`Asked`) and answers it in pieces
 * (`answer()`, `finish()`), as the GUI service does for its clients
 * (`devplan/guiprograms.md` §11 3d).
 *
 * When the application ends (`~GuiApplication`), a copy it still owns keeps
 * its first type: rendered and stored, so it outlives the application (one
 * answered later is withdrawn: nothing could answer it).
 *
 * @par Thread Safety
 * The loop's thread (with the application's lock held), as the widgets.
 */
class GuiClipboard {
        public:
                using Selection = Clipboard::Selection;

                /**
                 * @brief Renders @p type of a copy made with `copy()` into @p out (on
                 *        the loop's thread).
                 * @return `Ok`, or an error (the asker gets nothing).
                 */
                using Provider = Error (*)(void *arg, StringView type, String &out);

                /**
                 * @brief A paste's bytes, as @p type (on the loop's thread): @p err
                 *        `Ok`, or why there are none (`Empty`, `NotFound`, `Timeout`...).
                 */
                using Received = void (*)(void *arg, Error err, StringView type, StringView data);

                /** @brief Another program copied onto @p selection, which was this one's. */
                using Lost = void (*)(void *arg, Selection selection);

                /**
                 * @brief Asked for @p type of the copy on @p selection made with `offer()` (on the
                 *        loop's thread): answer request @p request with `answer()`, then `finish()`.
                 */
                using Asked = void (*)(void *arg, uint32_t request, Selection selection, StringView type);

                /** @brief The most pastes waiting at once. */
                static constexpr size_t MaxPastes = 4;

                /** @brief How long a paste waits for its owner's bytes, and a provider's reader. */
                static constexpr unsigned int TimeoutMs = 5000;

                explicit GuiClipboard(GuiApplication &app) : _app(app) {}
                ~GuiClipboard();

                GuiClipboard(const GuiClipboard &) = delete;
                GuiClipboard &operator=(const GuiClipboard &) = delete;

                /**
                 * @brief Copies @p text onto @p selection (stored now).
                 * @return `Ok`, `NoSpace` (over the clipboard's limit), `NotExist`
                 *         (no clipboard), ...
                 */
                Error copyText(Selection selection, StringView text);

                /**
                 * @brief Offers @p types (the best first) on @p selection, each
                 *        rendered by @p fn when asked for.
                 * @return `Ok`, `Invalid` (a bad type), `NotExist`, `NoMem`.
                 */
                Error copy(Selection selection, Span<const StringView> types, Provider fn, void *arg);

                /**
                 * @brief Offers @p types (the best first) on @p selection, each asked of
                 *        @p fn when another program wants it and answered later.
                 * @return `Ok`, `Invalid` (a bad type), `NotExist`, `NoMem`.
                 */
                Error offer(Selection selection, Span<const StringView> types, Asked fn, void *arg);

                /**
                 * @brief Adds @p data to request @p request's answer.
                 * @return `Ok`, `NotFound` (no such request waiting: finished, or given up after
                 *         `TimeoutMs`), `NoSpace` (over the clipboard's limit), `NoMem`.
                 */
                Error answer(uint32_t request, StringView data);

                /**
                 * @brief Ends request @p request: its answer written to the asker (none: it gets
                 *        nothing). @return `Ok`, `NotFound`.
                 */
                Error finish(uint32_t request);

                /**
                 * @brief @p arg is going: its copies made with `offer()` are withdrawn and its
                 *        requests end unanswered.
                 */
                void forget(void *arg);

                /**
                 * @brief Empties @p selection if it holds this application's copy
                 *        (a cut pasted: `devplan/filebrowser.md` §4j).
                 * @return `Ok`, `InvalidState` (none of this application's, or
                 *         another program's now), `NotExist` (no clipboard).
                 */
                Error clear(Selection selection);

                /** @brief Returns true if @p selection holds this application's copy. */
                bool owns(Selection selection) const;

                /** @brief Returns this application's copy's serial on @p selection (0: none). */
                uint32_t serial(Selection selection) const;

                /** @brief Returns true if @p selection can be read as @p type (offered, or converted). */
                bool canPaste(Selection selection, StringView type) const;

                /**
                 * @brief Reads @p selection as @p type, @p fn told with it later.
                 * @return `Ok` (told), or `Full` (`MaxPastes` waiting), `NoMem`.
                 */
                Error paste(Selection selection, StringView type, Received fn, void *arg);

                /** @brief Forgets the pastes asked for with @p arg (it is going): their `Received` isn't told. */
                void cancel(void *arg);

                /** @brief Names the application to `clip list` (`Clipboard::MaxOwnerName` bytes kept). */
                void setName(StringView name);

                /**
                 * @brief Keeps this application's copies on request: each renders its
                 *        first type, stored, and is withdrawn (`GuiApplication::exec()`
                 *        as it ends, while the providers' objects still exist).
                 */
                void keep();

                /** @brief Has @p fn told when another copy replaces one of this application's. */
                void setLost(Lost fn, void *arg) {
                        _lost = fn;
                        _lostArg = arg;
                        return;
                }

        private:
                // A copy of this application's on a selection.
                struct Own {
                                uint32_t serial = 0; // 0: none
                                Provider fn = nullptr;
                                void    *arg = nullptr;
                                Asked    asked = nullptr; // answered later (offer()); fn null
                };

                // Where a paste or a request is: waiting (a paste to read, a request to render),
                // busy (being read, rendered or written), ready (read: to tell; rendered: to write),
                // asked (a request told to its asked copy's owner, its answer coming).
                enum class State : uint8_t {
                        Free,
                        Waiting,
                        Busy,
                        Ready,
                        Asked
                };

                // A paste: what the thread reads, then what it read.
                struct Paste {
                                State     state = State::Free;
                                Selection selection = Selection::Clipboard;
                                char      type[ClipboardType::BufferBytes] = {};
                                Received  fn = nullptr;
                                void     *arg = nullptr;
                                Error     err;
                                String    data{MemSpace::Psram};
                };

                // An asker's request for a type, then its bytes to write.
                struct Request {
                                State           state = State::Free;
                                uint32_t        serial = 0;
                                char            type[ClipboardType::BufferBytes] = {};
                                FileHandle::Ptr write;
                                String          data{MemSpace::Psram};
                                uint32_t        id = 0;         // asked: its number for answer()
                                int64_t         deadlineNs = 0; // asked: given up after
                                void           *askedArg = nullptr;
                };

                static constexpr size_t MaxRequests = 4;

                static void requested(void *arg, uint32_t serial, StringView type, FileHandle::Ptr write);
                static void lost(void *arg, uint32_t serial);
                static void threadMain(void *arg);
                static void drain(void *arg); // on the loop's thread: results, requests, losses

                Error      wake();             // the thread, started the first time
                void       render(Request &r); // on the loop's thread
                void       read(Paste &p);     // on the thread
                Clipboard *clipboard() const;
                Request   *askedRequest(uint32_t id); // the loop's thread: one waiting for its answer

                GuiApplication   &_app;
                Own               _own[Clipboard::Selections];
                Lost              _lost = nullptr;
                void             *_lostArg = nullptr;
                Mutex             _jobsLock; // _pastes, _requests, _lostSerials
                Paste             _pastes[MaxPastes];
                Request           _requests[MaxRequests];
                uint32_t          _lostSerials[Clipboard::Selections] = {};
                Semaphore         _work{1, 0};
                Thread            _thread;
                std::atomic<bool> _started{false};
                std::atomic<bool> _quit{false};
                std::atomic<bool> _posted{false}; // a drain() queued
                uint32_t          _nextRequest = 0;
                char              _name[Clipboard::MaxOwnerName + 1] = "application";
};

BMDS_NAMESPACE_END
