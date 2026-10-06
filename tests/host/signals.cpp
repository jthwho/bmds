/**
 * @file      signals.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Signals: a process's actions, mask and pending set (programs.md §15),
 * and the task's interrupt its blocking calls wake on.
 */

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <bmds/errno.h>
#include <bmds/fdtable.h>
#include <bmds/interrupt.h>
#include <bmds/pipe.h>
#include <bmds/process.h>
#include <bmds/signals.h>
#include <bmds/sys.h>
#include "hostmemory.h"

using bmds::Interrupt;
using bmds::Process;
using bmds::Signals;
using Action = bmds::Signals::Action;
using Posted = bmds::Signals::Posted;

TEST_CASE("Signals: default actions end the process or drop the signal") {
        Signals s;
        CHECK(Signals::endsByDefault(BMDS_SIGINT));
        CHECK(Signals::endsByDefault(BMDS_SIGTERM));
        CHECK_FALSE(Signals::endsByDefault(BMDS_SIGCHLD));
        CHECK_FALSE(Signals::endsByDefault(BMDS_SIGWINCH));
        CHECK_FALSE(Signals::endsByDefault(BMDS_SIGTSTP));

        CHECK(s.post(BMDS_SIGCHLD, BMDS_SI_KERNEL) == Posted::Dropped);
        CHECK(s.pending() == 0);
        CHECK_FALSE(s.interrupt().isRaised());
        CHECK(s.post(BMDS_SIGTERM, BMDS_SI_USER) == Posted::Terminate);
        CHECK(s.terminateSignal() == BMDS_SIGTERM);
        CHECK(s.isDue());
        CHECK(s.interrupt().isRaised());
        // SIGKILL comes first.
        CHECK(s.post(BMDS_SIGKILL, BMDS_SI_USER) == Posted::Terminate);
        CHECK(s.terminateSignal() == BMDS_SIGKILL);
        const Signals::Taken t = s.take();
        CHECK(t.signal == BMDS_SIGKILL);
        CHECK(t.terminate);
}

TEST_CASE("Signals: caught signals wait to be taken, lowest first, with their code") {
        Signals s;
        CHECK(s.setAction(BMDS_SIGUSR2, Action::Catch));
        CHECK(s.setAction(BMDS_SIGUSR1, Action::Catch));
        CHECK(s.action(BMDS_SIGUSR1) == Action::Catch);
        CHECK(s.post(BMDS_SIGUSR2, BMDS_SI_USER) == Posted::Pending);
        CHECK(s.post(BMDS_SIGUSR1, BMDS_SI_KERNEL) == Posted::Pending);
        CHECK(s.isDue());
        CHECK(s.interrupt().isRaised());
        CHECK(s.terminateSignal() == 0);
        Signals::Taken t = s.take();
        CHECK(t.signal == BMDS_SIGUSR1);
        CHECK(t.code == BMDS_SI_KERNEL);
        CHECK_FALSE(t.terminate);
        t = s.take();
        CHECK(t.signal == BMDS_SIGUSR2);
        CHECK(t.code == BMDS_SI_USER);
        CHECK(s.take().signal == 0);
        CHECK_FALSE(s.isDue());
        // Nothing due: the next system call clears the interrupt.
        s.prepare();
        CHECK_FALSE(s.interrupt().isRaised());
        // Caught by default-dropped signals too.
        CHECK(s.setAction(BMDS_SIGCHLD, Action::Catch));
        CHECK(s.post(BMDS_SIGCHLD, BMDS_SI_KERNEL) == Posted::Pending);
        CHECK(s.take().signal == BMDS_SIGCHLD);
}

TEST_CASE("Signals: the mask holds signals until unblocked") {
        Signals s;
        CHECK(s.setAction(BMDS_SIGUSR1, Action::Catch));
        s.setBlocked(Signals::bit(BMDS_SIGUSR1) | Signals::bit(BMDS_SIGTERM));
        CHECK(s.post(BMDS_SIGUSR1, BMDS_SI_USER) == Posted::Pending);
        CHECK(s.post(BMDS_SIGTERM, BMDS_SI_USER) == Posted::Pending);
        CHECK_FALSE(s.isDue());
        CHECK_FALSE(s.interrupt().isRaised());
        CHECK(s.take().signal == 0);
        CHECK(s.pending() == (Signals::bit(BMDS_SIGUSR1) | Signals::bit(BMDS_SIGTERM)));
        // SIGKILL and SIGSTOP can't be blocked.
        s.setBlocked(0xffffffffu);
        CHECK((s.blocked() & Signals::bit(BMDS_SIGKILL)) == 0);
        CHECK((s.blocked() & Signals::bit(BMDS_SIGSTOP)) == 0);
        // Unblocking: the caught one is due, then the one that ends the process.
        s.setBlocked(Signals::bit(BMDS_SIGTERM));
        CHECK(s.isDue());
        CHECK(s.take().signal == BMDS_SIGUSR1);
        CHECK_FALSE(s.isDue());
        s.setBlocked(0);
        CHECK(s.terminateSignal() == BMDS_SIGTERM);
}

TEST_CASE("Signals: ignoring drops signals, a pending one too") {
        Signals s;
        CHECK_FALSE(s.setAction(BMDS_SIGKILL, Action::Ignore));
        CHECK_FALSE(s.setAction(BMDS_SIGSTOP, Action::Catch));
        CHECK(s.setAction(BMDS_SIGINT, Action::Ignore));
        CHECK(s.post(BMDS_SIGINT, BMDS_SI_KERNEL) == Posted::Dropped);
        CHECK(s.pending() == 0);
        s.setBlocked(Signals::bit(BMDS_SIGTERM));
        CHECK(s.post(BMDS_SIGTERM, BMDS_SI_USER) == Posted::Pending);
        CHECK(s.setAction(BMDS_SIGTERM, Action::Ignore));
        CHECK(s.pending() == 0);
        s.setBlocked(0);
        CHECK_FALSE(s.isDue());
        // Back to the default: ends the process again.
        CHECK(s.setAction(BMDS_SIGINT, Action::Default));
        CHECK(s.post(BMDS_SIGINT, BMDS_SI_KERNEL) == Posted::Terminate);
}

TEST_CASE("Signals: the Interrupt action raises the interrupt, nothing pending") {
        Signals s;
        REQUIRE(s.setAction(BMDS_SIGINT, Action::Interrupt));
        CHECK(s.action(BMDS_SIGINT) == Action::Interrupt);
        CHECK(s.post(BMDS_SIGINT, BMDS_SI_KERNEL) == Posted::Taken);
        CHECK(s.interrupt().isRaised());
        CHECK(s.pending() == 0);
        CHECK_FALSE(s.isDue());
        CHECK(s.terminateSignal() == 0);
        // SIGKILL can't have it; another action replaces it; exec drops it.
        CHECK_FALSE(s.setAction(BMDS_SIGKILL, Action::Interrupt));
        REQUIRE(s.setAction(BMDS_SIGINT, Action::Ignore));
        s.interrupt().clear();
        CHECK(s.post(BMDS_SIGINT, BMDS_SI_KERNEL) == Posted::Dropped);
        CHECK_FALSE(s.interrupt().isRaised());
        REQUIRE(s.setAction(BMDS_SIGTERM, Action::Interrupt));
        s.resetForExec();
        CHECK(s.action(BMDS_SIGTERM) == Action::Default);
}

TEST_CASE("Signals: exec and vfork children") {
        Signals parent;
        CHECK(parent.setAction(BMDS_SIGUSR1, Action::Catch));
        CHECK(parent.setAction(BMDS_SIGHUP, Action::Ignore));
        parent.setBlocked(Signals::bit(BMDS_SIGUSR2));
        CHECK(parent.post(BMDS_SIGUSR2, BMDS_SI_USER) == Posted::Pending);

        Signals child;
        child.inherit(parent);
        CHECK(child.action(BMDS_SIGUSR1) == Action::Catch);
        CHECK(child.action(BMDS_SIGHUP) == Action::Ignore);
        CHECK(child.blocked() == Signals::bit(BMDS_SIGUSR2));
        CHECK(child.pending() == 0);

        parent.resetForExec();
        CHECK(parent.action(BMDS_SIGUSR1) == Action::Default);
        CHECK(parent.action(BMDS_SIGHUP) == Action::Ignore);
        CHECK(parent.blocked() == Signals::bit(BMDS_SIGUSR2));
        CHECK(parent.pending() == Signals::bit(BMDS_SIGUSR2));
}

TEST_CASE("Signals: a signal sent during prepare() still leaves the interrupt raised") {
        // Each round, a sender posts while the task clears at the start of a
        // system call; once the post is done, the interrupt must be raised.
        Signals s;
        REQUIRE(s.setAction(BMDS_SIGUSR1, Action::Catch));
        constexpr int    Rounds = 20000;
        std::atomic<int> round{0}, posted{0};
        std::thread      sender([&] {
                for (int r = 1; r <= Rounds; ++r) {
                        while (round.load() < r) {}
                        (void)s.post(BMDS_SIGUSR1, BMDS_SI_USER);
                        posted.store(r);
                }
        });
        int              lost = 0;
        for (int r = 1; r <= Rounds; ++r) {
                (void)s.take();
                s.prepare(); // nothing due: cleared
                round.store(r);
                s.prepare(); // racing the post
                while (posted.load() < r) {}
                if (!s.interrupt().isRaised()) ++lost;
        }
        sender.join();
        CHECK(lost == 0);
}

TEST_CASE("Interrupt: current() is the calling thread's, set by Scope") {
        Interrupt a, b;
        CHECK(Interrupt::current() == nullptr);
        {
                Interrupt::Scope outer(&a);
                CHECK(Interrupt::current() == &a);
                {
                        Interrupt::Scope inner(&b);
                        CHECK(Interrupt::current() == &b);
                        Interrupt  *seen = &a;
                        std::thread other([&] { seen = Interrupt::current(); });
                        other.join();
                        CHECK(seen == nullptr);
                }
                CHECK(Interrupt::current() == &a);
        }
        CHECK(Interrupt::current() == nullptr);
}

TEST_CASE("Process: signals of its own; SIGPIPE on a write nobody reads") {
        const size_t base = HostMemory::liveAllocations();
        {
                bmds::FdTable fds;
                Process      *p = Process::create(fds, nullptr);
                REQUIRE(p != nullptr);
                CHECK(p->interrupted() == 0);
                REQUIRE(p->signals().setAction(BMDS_SIGINT, Action::Catch));
                CHECK(p->signals().post(BMDS_SIGINT, BMDS_SI_KERNEL) == Posted::Pending);
                CHECK(p->interrupted() == 1);
                // A due signal stops a sleep at once.
                CHECK(p->sleep(1'000'000'000) == -BMDS_EINTR);
                CHECK(p->signals().take().signal == BMDS_SIGINT);
                CHECK(p->interrupted() == 0);

                // The child gets the actions and mask, not the pending signals.
                REQUIRE(p->signals().setAction(BMDS_SIGPIPE, Action::Ignore));
                Process *child = p->createChild();
                REQUIRE(child != nullptr);
                CHECK(child->signals().action(BMDS_SIGINT) == Action::Catch);
                CHECK(child->signals().action(BMDS_SIGPIPE) == Action::Ignore);
                Process::destroy(child);

                int32_t ends[2] = {-1, -1};
                REQUIRE(p->pipe(ends, 0) == 0);
                REQUIRE(p->close(ends[0]) == 0);
                CHECK(p->write(ends[1], "x", 1) == -BMDS_EPIPE);
                CHECK(p->signals().pending() == 0); // ignored
                REQUIRE(p->signals().setAction(BMDS_SIGPIPE, Action::Default));
                CHECK(p->write(ends[1], "x", 1) == -BMDS_EPIPE);
                CHECK(p->signals().terminateSignal() == BMDS_SIGPIPE);
                Process::destroy(p);
        }
        CHECK(HostMemory::liveAllocations() == base);
}
