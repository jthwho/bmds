/**
 * @file      guilistener.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Remote programs (devplan/guiprograms.md §4g, §16): while `gui/listen` is `loopback`, a TCP
 * listener on 127.0.0.1:`gui/port` takes connections that SSH forwards from other machines, each
 * a GUI connection as `gui_open`'s are, which must present the user's cookie (`gui/cookie`). A
 * thread waits on one semaphore that the listening socket's readiness and the settings' watcher
 * both release, and runs only while listening is on.
 */

#include <atomic>
#include <cstring>
#include <bmds/guiservice.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/random.h>
#include <bmds/semaphore.h>
#include <bmds/socket.h>
#include <bmds/staticobject.h>
#include <bmds/sysconf.h>
#include <bmds/thread.h>
#include <bmds/vfs.h>
#include <bmds/waitlist.h>
#include "guiservicepriv.h"

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(guiListen, "gui/listen", Config::Type::Enum, "off",
                     "Take GUI connections that SSH forwards from other machines (loopback: on 127.0.0.1 only)", 0, 0,
                     "off|loopback", nullptr)
        BMDS_SYSCONF(guiPort, "gui/port", Config::Type::Int, "6400", "The port gui/listen takes connections on", 1,
                     65535, nullptr, nullptr)
        BMDS_SYSCONF(guiCookie, "gui/cookie", Config::Type::Path, "/home/.config/gui/cookie",
                     "The file holding the cookie a remote GUI program presents (made when listening starts)", 0, 255,
                     nullptr, nullptr)

        constexpr size_t       CookieBytes = 32;
        constexpr unsigned int RetryMs = 5'000; // a port that wouldn't bind tried again
        constexpr int          Backlog = 4;

        struct State {
                        SysConf::Watcher watcher;
                        Semaphore        wake{1, 0}; // a setting changed, or a connection waits
                        Mutex            lock;       // the thread's start and end
                        Thread           thread;
                        bool             running = false; // the thread runs (lock)
        };

        constinit StaticObject<State> state;

        bool enabled() {
                String value(MemSpace::Psram);
                return SysConf::get(StringView(bmdsConf_guiListen.key), value).isOk() && value.view() == "loopback";
        }

        uint16_t port() {
                auto [p, err] = SysConf::getInt(StringView(bmdsConf_guiPort.key));
                return err.isOk() && p > 0 && p < 65536 ? uint16_t(p) : uint16_t(6400);
        }

        int hexDigit(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        // The cookie file's 32 bytes into @p out. @return False if it isn't there or isn't one.
        bool readCookie(uint8_t (&out)[CookieBytes]) {
                String path(MemSpace::Psram);
                if (SysConf::get(StringView(bmdsConf_guiCookie.key), path).isError()) return false;
                auto [file, err] = Vfs::open(path.view(), Vfs::Read, Vfs::Context());
                if (err.isError()) return false;
                char   text[CookieBytes * 2];
                size_t got = 0;
                while (got < sizeof(text)) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, sizeof(text) - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                if (got != sizeof(text)) return false;
                for (size_t i = 0; i < CookieBytes; ++i) {
                        const int hi = hexDigit(text[2 * i]), lo = hexDigit(text[2 * i + 1]);
                        if (hi < 0 || lo < 0) return false;
                        out[i] = uint8_t(hi << 4 | lo);
                }
                return true;
        }

        // A cookie made if there is none (its folders too).
        void ensureCookie() {
                uint8_t cookie[CookieBytes];
                if (readCookie(cookie)) return;
                String path(MemSpace::Psram);
                if (SysConf::get(StringView(bmdsConf_guiCookie.key), path).isError()) return;
                const StringView p = path.view();
                for (size_t at = 1; at < p.size(); ++at)
                        if (p[at] == '/') (void)Vfs::mkdir(p.left(at), Vfs::Context());
                Random::fill(cookie, sizeof(cookie));
                static constexpr char Hex[] = "0123456789abcdef";
                char                  text[CookieBytes * 2 + 1];
                for (size_t i = 0; i < CookieBytes; ++i) {
                        text[2 * i] = Hex[cookie[i] >> 4];
                        text[2 * i + 1] = Hex[cookie[i] & 15];
                }
                text[CookieBytes * 2] = '\n';
                std::memset(cookie, 0, sizeof(cookie));
                auto [file, err] = Vfs::open(p, Vfs::Write | Vfs::Create | Vfs::Truncate, Vfs::Context());
                if (err.isOk())
                        err = file->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), sizeof(text)), WaitForever)
                                      .error();
                std::memset(text, 0, sizeof(text));
                if (err.isError()) bmdsErr("gui: can't make the cookie %.*s: %s", int(p.size()), p.data(), err.desc());
                return;
        }

        void drainWatcher() {
                SysConf::Id id;
                while (state->watcher.next(id)) {}
                (void)state->watcher.overflowed();
                return;
        }

        // The listening socket: bound to 127.0.0.1:@p at, its readiness waking the thread.
        struct Listening {
                        FileHandle::Ptr handle;
                        Socket         *socket = nullptr;
                        size_t          slot = WaitList::NoSlot;
                        uint16_t        port = 0;

                        Error open(uint16_t at) {
                                auto [h, err] = Socket::create(Socket::Type::Stream, 0);
                                if (err.isError()) return err;
                                Socket *s = h->stream().socket();
                                (void)s->setOption(Socket::Option::ReuseAddress, 1);
                                SocketAddress loopback;
                                loopback.ipv4 = __builtin_bswap32(0x7f000001u); // network order
                                loopback.port = at;
                                err = s->bind(loopback);
                                if (err.isOk()) err = s->listen(Backlog);
                                if (err.isError()) return err;
                                handle = std::move(h);
                                socket = s;
                                port = at;
                                slot = socket->readinessWaiters()->add(state->wake);
                                return Error::Ok;
                        }

                        void close() {
                                if (socket != nullptr && slot != WaitList::NoSlot)
                                        socket->readinessWaiters()->remove(slot);
                                slot = WaitList::NoSlot;
                                socket = nullptr;
                                handle = FileHandle::Ptr();
                                port = 0;
                                return;
                        }
        };

        void serve(void *) {
                Listening listening;
                bool      failed = false; // the port wouldn't bind: tried again in a while
                for (;;) {
                        drainWatcher();
                        if (!enabled()) break;
                        const uint16_t want = port();
                        if (listening.socket != nullptr && listening.port != want) listening.close();
                        if (listening.socket == nullptr) {
                                ensureCookie();
                                const Error err = listening.open(want);
                                if (err.isError()) {
                                        if (!failed) bmdsErr("gui: can't listen on 127.0.0.1:%u: %s", want, err.desc());
                                        failed = true;
                                } else {
                                        failed = false;
                                        bmdsInfo("gui: taking remote programs on 127.0.0.1:%u", want);
                                }
                        }
                        // The connections waiting, each a GUI connection.
                        while (listening.socket != nullptr) {
                                SocketAddress peer;
                                auto [conn, aerr] = listening.socket->accept(&peer, Socket::DontWait);
                                if (aerr.isError()) break;
                                (void)conn->stream().socket()->setOption(Socket::Option::NoDelay, 1);
                                const Error oerr = GuiServicePriv::openRemote(std::move(conn));
                                if (oerr.isError()) bmdsWarn("gui: a remote program refused: %s", oerr.desc());
                        }
                        (void)state->wake.acquire(failed ? RetryMs : WaitForever);
                }
                listening.close();
                bmdsInfo("gui: remote programs no longer taken");
                {
                        Mutex::Locker lock(state->lock, WaitForever);
                        state->running = false;
                        state->thread.detach();
                }
                return;
        }

        // On and not running: the thread (none while off).
        void startThread() {
                Mutex::Locker lock(state->lock, WaitForever);
                if (state->running || !enabled()) return;
                Thread::Config config;
                config.name = "gui.listen";
                config.stackBytes = 4096;
                config.stackSpace = MemSpace::Psram;
                config.kernel = true;
                const Error err = state->thread.start(config, &serve, nullptr);
                if (err.isError()) {
                        bmdsErr("gui: no listener thread: %s", err.desc());
                        return;
                }
                state->running = true;
                return;
        }

        void settingsChanged(void *) {
                state->wake.release();
                startThread();
                return;
        }

} // namespace

bool GuiServicePriv::cookieMatches(StringView name, ByteSpan data) {
        uint8_t cookie[CookieBytes];
        if (name != "BMDS-COOKIE-1" || data.size() != CookieBytes || !readCookie(cookie)) return false;
        uint8_t differ = 0; // every byte compared: the time says nothing
        for (size_t i = 0; i < CookieBytes; ++i) differ |= uint8_t(cookie[i] ^ data[i]);
        std::memset(cookie, 0, sizeof(cookie));
        return differ == 0;
}

Error GuiService::listen() {
        state.create();
        const Error err = state->watcher.start(StringView("gui"), &settingsChanged, nullptr);
        if (err.isError()) return err;
        startThread();
        return Error::Ok;
}

BMDS_NAMESPACE_END
