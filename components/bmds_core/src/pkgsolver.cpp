/**
 * @file      pkgsolver.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdarg>
#include <cstdint>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgsolver.h>
#include <bmds/pkgversion.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using PkgSolver::Package;

        constexpr size_t NPos = static_cast<size_t>(-1);

        Error fail(String &why, Error err, const char *fmt, ...) BMDS_PRINTF_FORMAT(3, 4);
        Error fail(String &why, Error err, const char *fmt, ...) {
                why.clear();
                va_list args;
                va_start(args, fmt);
                (void)why.vappendf(fmt, args);
                va_end(args);
                return err;
        }

        size_t find(Span<const Package> packages, StringView name) {
                for (size_t i = 0; i < packages.size(); ++i) {
                        if (packages[i].name == name) return i;
                }
                return NPos;
        }

        bool suits(StringView version, StringView minVersion) {
                return minVersion.isEmpty() || PkgVersion::compare(version, minVersion) >= 0;
        }

        // " (>= VERSION)", or nothing.
        void appendMin(String &out, StringView minVersion) {
                if (!minVersion.isEmpty()) (void)out.appendf(" (>= %.*s)", int(minVersion.size()), minVersion.data());
                return;
        }

        struct Frame {
                        size_t             candidate;
                        PkgDepends::Reader deps;
        };

        // The walk's state: what is chosen for each name, and each candidate's progress.
        class Walk {
                public:
                        Walk(Span<const Package> installed, Span<const Package> candidates)
                            : _installed(installed), _candidates(candidates), _chosen(MemSpace::Psram),
                              _state(MemSpace::Psram), _automatic(MemSpace::Psram), _stack(MemSpace::Psram) {}

                        Error init() {
                                Error err = _state.reserve(_candidates.size());
                                if (err.isOk()) err = _automatic.reserve(_candidates.size());
                                for (size_t i = 0; err.isOk() && i < _candidates.size(); ++i) {
                                        (void)_state.append(New);
                                        (void)_automatic.append(0);
                                }
                                return err;
                        }

                        // The candidate chosen for @p name, or NPos.
                        size_t chosen(StringView name) const {
                                for (size_t c : _chosen) {
                                        if (_candidates[c].name == name) return c;
                                }
                                return NPos;
                        }

                        Error choose(size_t c, bool automatic) {
                                _automatic[c] = automatic ? 1 : 0;
                                return _chosen.append(c);
                        }

                        // The newest candidate named @p name that suits @p minVersion, or NPos.
                        size_t best(StringView name, StringView minVersion) const {
                                size_t found = NPos;
                                for (size_t i = 0; i < _candidates.size(); ++i) {
                                        const Package &p = _candidates[i];
                                        if (p.name != name || !suits(p.version, minVersion)) continue;
                                        if (found == NPos ||
                                            PkgVersion::compare(p.version, _candidates[found].version) > 0)
                                                found = i;
                                }
                                return found;
                        }

                        // The names on the stack from candidate @p from, "a -> b -> c".
                        void chain(String &out, size_t from) const {
                                bool on = false;
                                for (const Frame &f : _stack) {
                                        on = on || f.candidate == from;
                                        if (!on) continue;
                                        const StringView n = _candidates[f.candidate].name;
                                        (void)out.appendf("%.*s -> ", int(n.size()), n.data());
                                }
                                return;
                        }

                        // Walks from requested candidate @p start, appending to @p out in order.
                        Error from(size_t start, List<PkgSolver::Step> &out, String &why, StringView *wanted) {
                                if (_state[start] == Done) return Error::Ok;
                                Error err = push(start);
                                while (err.isOk() && !_stack.isEmpty()) {
                                        Frame                 &top = _stack.back();
                                        const Package         &p = _candidates[top.candidate];
                                        PkgDepends::Dependency d;
                                        const Error            next = top.deps.next(d);
                                        if (next == Error::EndOfFile) {
                                                const size_t c = top.candidate;
                                                _state[c] = Done;
                                                _stack.truncate(_stack.size() - 1);
                                                err = out.append(PkgSolver::Step{c, _automatic[c] != 0});
                                                continue;
                                        }
                                        if (next.isError()) {
                                                return fail(why, Error::CorruptData,
                                                            "%.*s: its dependencies are damaged", int(p.name.size()),
                                                            p.name.data());
                                        }
                                        size_t c = chosen(d.name);
                                        if (c != NPos) {
                                                const Package &given = _candidates[c];
                                                if (!suits(given.version, d.minVersion)) {
                                                        String need;
                                                        appendMin(need, d.minVersion);
                                                        return fail(why, Error::InvalidState,
                                                                    "%.*s needs %.*s%s; %.*s is given",
                                                                    int(p.name.size()), p.name.data(),
                                                                    int(d.name.size()), d.name.data(), need.cStr(),
                                                                    int(given.version.size()), given.version.data());
                                                }
                                                if (_state[c] == Done) continue;
                                                if (_state[c] == OnStack) {
                                                        String cycle;
                                                        chain(cycle, c);
                                                        return fail(why, Error::InvalidState,
                                                                    "a dependency cycle: %s%.*s", cycle.cStr(),
                                                                    int(d.name.size()), d.name.data());
                                                }
                                                err = push(c);
                                                continue;
                                        }
                                        const size_t i = find(_installed, d.name);
                                        if (i != NPos && suits(_installed[i].version, d.minVersion)) continue;
                                        c = best(d.name, d.minVersion);
                                        if (c == NPos) {
                                                if (wanted != nullptr) *wanted = d.name;
                                                String need, by;
                                                appendMin(need, d.minVersion);
                                                chain(by, _stack.front().candidate);
                                                by.truncate(by.size() - 4); // the last " -> "
                                                return fail(why, Error::NotFound, "%.*s%s, needed by %s, %s",
                                                            int(d.name.size()), d.name.data(), need.cStr(), by.cStr(),
                                                            i != NPos ? "is installed in an older version and not given"
                                                                      : "isn't installed or given");
                                        }
                                        err = choose(c, true);
                                        if (err.isOk()) err = push(c);
                                }
                                return err;
                        }

                        // An installed package that stays may not lose a dependency to the plan.
                        Error checkInstalled(String &why) const {
                                for (const Package &p : _installed) {
                                        if (chosen(p.name) != NPos) continue; // replaced: its new version was walked
                                        PkgDepends::Reader     deps(p.depends);
                                        PkgDepends::Dependency d;
                                        while (deps.next(d).isOk()) {
                                                const size_t c = chosen(d.name);
                                                if (c == NPos || suits(_candidates[c].version, d.minVersion)) continue;
                                                const Package &given = _candidates[c];
                                                String         need;
                                                appendMin(need, d.minVersion);
                                                return fail(why, Error::InvalidState,
                                                            "installing %.*s %.*s would break %.*s, which needs %.*s%s",
                                                            int(given.name.size()), given.name.data(),
                                                            int(given.version.size()), given.version.data(),
                                                            int(p.name.size()), p.name.data(), int(d.name.size()),
                                                            d.name.data(), need.cStr());
                                        }
                                }
                                return Error::Ok;
                        }

                private:
                        enum : uint8_t {
                                New,
                                OnStack,
                                Done
                        };

                        Error push(size_t c) {
                                _state[c] = OnStack;
                                return _stack.append(Frame{c, PkgDepends::Reader(_candidates[c].depends)});
                        }

                        Span<const Package> _installed;
                        Span<const Package> _candidates;
                        List<size_t>        _chosen;
                        List<uint8_t>       _state;
                        List<uint8_t>       _automatic;
                        List<Frame>         _stack;
        };

} // namespace

Error PkgSolver::plan(Span<const Package> installed, Span<const Package> candidates, Span<const size_t> requested,
                      List<Step> &out, String &why, StringView *wanted) {
        out.clear();
        why.clear();
        Walk  walk(installed, candidates);
        Error err = walk.init();
        if (err.isError()) return fail(why, err, "out of memory");

        // What was asked for, each name once.
        for (size_t r : requested) {
                if (r >= candidates.size()) return fail(why, Error::Invalid, "no such candidate");
                const size_t already = walk.chosen(candidates[r].name);
                if (already == r) continue;
                if (already != NPos) {
                        const Package &a = candidates[already], &b = candidates[r];
                        return fail(why, Error::InvalidState, "%.*s is given twice (%.*s and %.*s)", int(b.name.size()),
                                    b.name.data(), int(a.version.size()), a.version.data(), int(b.version.size()),
                                    b.version.data());
                }
                err = walk.choose(r, false);
                if (err.isError()) return fail(why, err, "out of memory");
        }
        for (size_t r : requested) {
                err = walk.from(r, out, why, wanted);
                if (err == Error::NoMem) return fail(why, err, "out of memory");
                if (err.isError()) return err;
        }
        return walk.checkInstalled(why);
}

Error PkgSolver::unneeded(Span<const Package> installed, Span<const StringView> removing, List<size_t> &out) {
        out.clear();
        List<uint8_t> needed(MemSpace::Psram); // 1: staying and needed (asked for, or by one that is)
        List<size_t>  work(MemSpace::Psram);
        Error         err = needed.reserve(installed.size());
        for (size_t i = 0; err.isOk() && i < installed.size(); ++i) {
                bool going = false;
                for (StringView r : removing) going = going || r == installed[i].name;
                const bool asked = !going && !installed[i].automatic;
                (void)needed.append(asked ? 1 : 0);
                if (asked) err = work.append(i);
        }
        // What staying packages need, staying too, through any chain.
        while (err.isOk() && !work.isEmpty()) {
                const size_t           i = work.takeLast();
                PkgDepends::Reader     deps(installed[i].depends);
                PkgDepends::Dependency d;
                while (err.isOk() && deps.next(d).isOk()) {
                        const size_t j = find(installed, d.name);
                        if (j == NPos || needed[j] != 0) continue;
                        bool going = false;
                        for (StringView r : removing) going = going || r == d.name;
                        if (going) continue;
                        needed[j] = 1;
                        err = work.append(j);
                }
        }
        for (size_t i = 0; err.isOk() && i < installed.size(); ++i) {
                bool going = false;
                for (StringView r : removing) going = going || r == installed[i].name;
                if (!going && needed[i] == 0) err = out.append(i);
        }
        return err;
}

BMDS_NAMESPACE_END
