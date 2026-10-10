/**
 * @file      notifications.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The notifications kept (devplan/notifications.md §3.2): ids, replacing, a balloon's end, closing,
 * read and unread, the order, a full log, texts cut at a character's end.
 */

#include <doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include <bmds/notificationlog.h>

#include "hostmemory.h"

using bmds::Error;
using bmds::Notification;
using bmds::NotificationLog;
using bmds::Span;

namespace {

        Notification make(const char *title, uint32_t replaces = 0, uint8_t flags = 0) {
                Notification n;
                std::snprintf(n.title, sizeof(n.title), "%s", title);
                n.replaces = replaces;
                n.flags = flags;
                return n;
        }

        std::vector<uint32_t> idsOf(const NotificationLog &log) {
                std::vector<uint32_t> out(NotificationLog::Capacity);
                const size_t          n = log.ids(Span<uint32_t>(out.data(), out.size()));
                out.resize(n);
                return out;
        }

} // namespace

TEST_CASE("NotificationLog: posting, the order, a balloon's end, reading") {
        const size_t base = HostMemory::liveAllocations();
        {
                NotificationLog log;
                CHECK(log.count() == 0);
                CHECK(log.lastChange() == 0);
                const uint32_t a = log.post(make("one"), 5, 0, 100).value();
                const uint32_t b = log.post(make("two"), 6, 9, 200).value();
                CHECK(a != 0);
                CHECK(b != a);
                CHECK(idsOf(log) == std::vector<uint32_t>{b, a}); // newest first
                Notification n;
                REQUIRE(log.get(b, n).isOk());
                CHECK(std::string(n.title) == "two");
                CHECK(n.owner == 6);
                CHECK(n.listener == 9);
                CHECK(n.postedNs == 200);
                CHECK(n.balloon);
                CHECK_FALSE(n.read);
                CHECK(log.unread() == 2);
                const uint64_t before = log.lastChange();

                // Expired: kept, unread; dismissed or clicked: read.
                CHECK(log.endBalloon(a, NotificationLog::Ended::Expired).isOk());
                REQUIRE(log.get(a, n).isOk());
                CHECK_FALSE(n.balloon);
                CHECK_FALSE(n.read);
                CHECK(log.lastChange() > before);
                CHECK(log.endBalloon(b, NotificationLog::Ended::Activated).isOk());
                CHECK(log.unread() == 1);
                log.markRead(0);
                CHECK(log.unread() == 0);
                CHECK(log.endBalloon(77, NotificationLog::Ended::Expired) == Error::NotExist);
        }
        CHECK(HostMemory::liveAllocations() == base);
}

TEST_CASE("NotificationLog: replacing one's own, not another's") {
        NotificationLog log;
        const uint32_t  a = log.post(make("track 1"), 5, 0, 1).value();
        const uint32_t  b = log.post(make("other"), 6, 0, 2).value();
        CHECK(log.endBalloon(a, NotificationLog::Ended::Dismissed).isOk());
        // The same id, updated, its balloon due again, unread, and the newest.
        CHECK(log.post(make("track 2", a), 5, 0, 3).value() == a);
        Notification n;
        REQUIRE(log.get(a, n).isOk());
        CHECK(std::string(n.title) == "track 2");
        CHECK(n.balloon);
        CHECK_FALSE(n.read);
        CHECK(idsOf(log) == std::vector<uint32_t>{a, b});
        CHECK(log.count() == 2);
        // Someone else's: refused. One gone: a new one.
        CHECK(log.post(make("hijack", b), 5, 0, 4).error() == Error::PermissionDenied);
        const uint32_t c = log.post(make("again", 999), 5, 0, 5).value();
        CHECK(c != a);
        CHECK(c != b);
}

TEST_CASE("NotificationLog: closing, transient, clearing, listeners") {
        NotificationLog log;
        const uint32_t  a = log.post(make("a"), 5, 3, 1).value();
        const uint32_t  t = log.post(make("t", 0, Notification::Transient), 5, 3, 2).value();
        const uint32_t  c = log.post(make("c"), 6, 0, 3).value();
        CHECK(log.close(a, 6) == Error::PermissionDenied);
        CHECK(log.close(a, 5).isOk());
        CHECK(log.close(a, 5) == Error::NotExist);
        // A transient one goes with its balloon.
        CHECK(log.endBalloon(t, NotificationLog::Ended::Expired).isOk());
        CHECK(idsOf(log) == std::vector<uint32_t>{c});
        // Clear All leaves the balloons due.
        const uint32_t d = log.post(make("d"), 6, 0, 4).value();
        CHECK(log.endBalloon(c, NotificationLog::Ended::Expired).isOk());
        log.clear();
        CHECK(idsOf(log) == std::vector<uint32_t>{d});
        // A listener gone: told to nobody.
        const uint32_t e = log.post(make("e"), 7, 4, 5).value();
        log.dropListener(4);
        Notification n;
        REQUIRE(log.get(e, n).isOk());
        CHECK(n.listener == 0);
        CHECK(log.close(e, -1).isOk()); // anyone (the server)
}

TEST_CASE("NotificationLog: full, the oldest without a balloon goes") {
        NotificationLog       log;
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < NotificationLog::Capacity; ++i)
                ids.push_back(log.post(make("n"), 1, 0, int64_t(i)).value());
        // All but the first two have their balloons done: the third is the oldest quiet one.
        for (size_t i = 2; i < ids.size(); ++i) CHECK(log.endBalloon(ids[i], NotificationLog::Ended::Expired).isOk());
        const uint32_t extra = log.post(make("extra"), 1, 0, 1000).value();
        CHECK(log.count() == NotificationLog::Capacity);
        Notification n;
        CHECK(log.get(ids[0], n).isOk());
        CHECK(log.get(ids[1], n).isOk());
        CHECK(log.get(ids[2], n) == Error::NotExist);
        CHECK(log.get(extra, n).isOk());
}

TEST_CASE("NotificationLog: texts cut at a character's end, actions at three") {
        NotificationLog log;
        Notification    n;
        std::memset(n.title, 'a', sizeof(n.title));
        // "é" (2 bytes) in the title's last two: the last must be the NUL, so the é goes whole.
        n.title[sizeof(n.title) - 2] = char(0xc3);
        n.title[sizeof(n.title) - 1] = char(0xa9);
        n.actionCount = 9;
        n.urgency = Notification::Urgency(7);
        const uint32_t id = log.post(n, 1, 0, 1).value();
        Notification   got;
        REQUIRE(log.get(id, got).isOk());
        CHECK(std::strlen(got.title) == sizeof(got.title) - 2); // not half of it
        CHECK(got.actionCount == Notification::MaxActions);
        CHECK(got.urgency == Notification::Urgency::Normal);
}

// ---- The kernel's registry: the server told, listeners told, an events fd.

#include <bmds/notifications.h>

namespace {

        struct Told {
                        std::vector<std::vector<uint32_t>> events; // id, event, action
                        static void fn(void *arg, uint32_t id, bmds::Notifications::Event e, uint32_t action) {
                                static_cast<Told *>(arg)->events.push_back({id, uint32_t(e), action});
                        }
        };

} // namespace

TEST_CASE("Notifications: the server told, a listener told, an events fd") {
        namespace N = bmds::Notifications;
        REQUIRE(N::init().isOk());
        int  changes = 0;
        auto changed = [](void *arg) {
                ++*static_cast<int *>(arg);
        };
        REQUIRE(N::setServer(changed, &changes).isOk());
        CHECK(N::setServer(changed, &changes) == Error::Exists);
        CHECK(N::hasServer());

        Told        told;
        N::Listener listener(Told::fn, &told);
        REQUIRE(listener.start().isOk());
        const uint32_t a = N::post(make("a"), 5, &listener).value();
        CHECK(changes == 1);
        N::endBalloon(a, NotificationLog::Ended::Activated, 2);
        REQUIRE(told.events.size() == 1);
        CHECK(told.events[0] == std::vector<uint32_t>{a, uint32_t(N::Event::Activated), 2});
        N::endBalloon(a, NotificationLog::Ended::Expired); // already ended: nothing
        CHECK(told.events.size() == 1);
        N::activate(a, 0); // from the centre
        CHECK(told.events.back() == std::vector<uint32_t>{a, uint32_t(N::Event::Activated), 0});
        listener.stop();
        const uint32_t b = N::post(make("b"), 5, nullptr).value();
        N::endBalloon(b, NotificationLog::Ended::Dismissed);
        CHECK(told.events.size() == 2); // nobody told

        // An events fd: a dismissal read as a record.
        auto [handle, err] = N::openEvents();
        REQUIRE(err.isOk());
        N::Listener *fdListener = N::listenerOf(handle->stream());
        REQUIRE(fdListener != nullptr);
        const uint32_t c = N::post(make("c"), 7, fdListener).value();
        CHECK((handle->stream().readiness() & bmds::Stream::Readable) == 0);
        N::endBalloon(c, NotificationLog::Ended::Dismissed);
        CHECK((handle->stream().readiness() & bmds::Stream::Readable) != 0);
        N::EventRecord rec;
        auto [n, rerr] = handle->read(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(&rec), sizeof(rec)), 0);
        REQUIRE(rerr.isOk());
        CHECK(n == sizeof(rec));
        CHECK(rec.id == c);
        CHECK(rec.event == uint32_t(N::Event::Dismissed));
        handle.reset(); // its listener stops; the notification stays, told to nobody
        Notification got;
        REQUIRE(N::get(c, got).isOk());
        CHECK(got.listener == 0);

        // Closing: one's own only; the centre's Clear All; reading.
        CHECK(N::close(c, 8) == Error::PermissionDenied);
        CHECK(N::close(c, 7).isOk());
        N::markRead(0);
        CHECK(N::unread() == 0);
        N::clear();
        uint32_t ids[4];
        CHECK(N::ids(Span<uint32_t>(ids, 4)) == 0);
        N::clearServer(&changes);
        CHECK_FALSE(N::hasServer());
}
