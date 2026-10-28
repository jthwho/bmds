/**
 * @file      kernelpatchsource.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The patchbay's graph from the kernel's public classes (devplan/audio.md
 * §24): the MIDI router's ports and connections, `Midi`'s watchers, and
 * `midi/connect` (connections kept by name: `SOURCE>DEST`, comma separated), and `synth/system`:
 * a connection from `system` is the system synth, set there (devplan/audio.md §26: one way to set
 * it), never kept in `midi/connect`.
 */

#include <cstdio>
#include <cstring>
#include <bmds/midi.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>

#include "patchsource.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *ConnectKey = "midi/connect";
        constexpr const char *SystemKey = "synth/system";

        // Sets and saves @p key.
        Error setSaved(const char *key, StringView value) {
                const StringView k(key);
                Error            err = SysConf::set(k, value);
                if (err.isOk()) err = SysConf::save(k);
                return err;
        }

        // midi/connect without the pairs from @p from (to @p to; empty: to any), saved if it changed.
        Error dropKept(StringView from, StringView to) {
                String list(MemSpace::Psram);
                (void)SysConf::get(StringView(ConnectKey), list);
                String     out(MemSpace::Psram);
                bool       dropped = false;
                StringView rest = list.view();
                while (!rest.isEmpty()) {
                        const size_t     comma = rest.indexOf(',');
                        const StringView item = (comma == StringView::NPos ? rest : rest.left(comma)).trimmed();
                        rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                        if (item.isEmpty()) continue;
                        const size_t arrow = item.indexOf('>');
                        if (arrow != StringView::NPos && item.left(arrow).trimmed().equalsIgnoreCase(from) &&
                            (to.isEmpty() || item.mid(arrow + 1).trimmed().equalsIgnoreCase(to))) {
                                dropped = true;
                                continue;
                        }
                        if (out.appendf("%s%.*s", out.isEmpty() ? "" : ",", int(item.size()), item.data()).isError())
                                return Error::NoMem;
                }
                return dropped ? setSaved(ConnectKey, out.view()) : Error(Error::Ok);
        }

} // namespace

size_t KernelPatchSource::ports(Span<Port> out) {
        MidiRouter::PortInfo all[MidiRouter::MaxPorts];
        const size_t         n = Midi::router().ports(Span<MidiRouter::PortInfo>(all, MidiRouter::MaxPorts));
        for (size_t i = 0; i < n && i < out.size(); ++i) {
                Port &p = out[i];
                p = Port();
                p.number = all[i].number;
                p.source = (all[i].sides & MidiRouter::Source) != 0;
                p.destination = (all[i].sides & MidiRouter::Destination) != 0;
                std::snprintf(p.name, sizeof(p.name), "%s", all[i].name);
                std::snprintf(p.kind, sizeof(p.kind), "%s", Midi::kindName(all[i].kind));
        }
        return n;
}

size_t KernelPatchSource::connections(Span<Connection> out) {
        MidiRouter::Connection all[MidiRouter::MaxConnections];
        const size_t n = Midi::router().connections(Span<MidiRouter::Connection>(all, MidiRouter::MaxConnections));
        for (size_t i = 0; i < n && i < out.size(); ++i) {
                out[i].source = all[i].source;
                out[i].destination = all[i].destination;
        }
        return n;
}

// midi/connect with SOURCE>DEST (by the ports' names) in it or out of it, saved.
Error KernelPatchSource::keep(uint8_t source, uint8_t destination, bool kept) {
        auto [from, ferr] = Midi::router().port(source);
        auto [to, terr] = Midi::router().port(destination);
        if (ferr.isError() || terr.isError()) return Error::NotExist;
        String list(MemSpace::Psram);
        (void)SysConf::get(StringView(ConnectKey), list);
        String     out(MemSpace::Psram);
        bool       found = false;
        StringView rest = list.view();
        while (!rest.isEmpty()) {
                const size_t     comma = rest.indexOf(',');
                const StringView item = (comma == StringView::NPos ? rest : rest.left(comma)).trimmed();
                rest = comma == StringView::NPos ? StringView() : rest.mid(comma + 1);
                if (item.isEmpty()) continue;
                const size_t arrow = item.indexOf('>');
                const bool   same = arrow != StringView::NPos &&
                                  item.left(arrow).trimmed().equalsIgnoreCase(StringView(from.name)) &&
                                  item.mid(arrow + 1).trimmed().equalsIgnoreCase(StringView(to.name));
                if (same) {
                        found = true;
                        if (!kept) continue; // taken out
                }
                if (out.appendf("%s%.*s", out.isEmpty() ? "" : ",", int(item.size()), item.data()).isError())
                        return Error::NoMem;
        }
        if (kept && !found && out.appendf("%s%s>%s", out.isEmpty() ? "" : ",", from.name, to.name).isError())
                return Error::NoMem;
        if (kept == found) return Error::Ok; // as it was
        return setSaved(ConnectKey, out.view());
}

Error KernelPatchSource::connect(uint8_t source, uint8_t destination) {
        if (source == Midi::systemPort()) {
                // The system synth: synth/system names it (the router's route follows, the old one
                // goes), and what midi/connect kept from system goes with it.
                auto [to, terr] = Midi::router().port(destination);
                if (terr.isError()) return Error::NotExist;
                Error err = dropKept(StringView("system"), StringView());
                if (err.isOk()) err = setSaved(SystemKey, StringView(to.name));
                Midi::applySettings();
                return err;
        }
        Error err = Midi::router().connect(source, destination);
        if (err == Error::Exists) err = Error::Ok;
        if (err.isOk()) err = keep(source, destination, true);
        return err;
}

Error KernelPatchSource::disconnect(uint8_t source, uint8_t destination) {
        if (source == Midi::systemPort()) {
                // The system synth taken away: no default route (synth/system empty), and a pair
                // midi/connect kept for it gone too; first, as the settings would bring it back.
                auto [to, terr] = Midi::router().port(destination);
                if (terr.isError()) return Error::NotExist;
                Error  kept = dropKept(StringView("system"), StringView(to.name));
                String system(MemSpace::Psram);
                if (kept.isOk() && SysConf::get(StringView(SystemKey), system).isOk() &&
                    system.view().equalsIgnoreCase(StringView(to.name)))
                        kept = setSaved(SystemKey, StringView());
                Midi::applySettings();
                Error err = Midi::router().disconnect(source, destination);
                if (err == Error::NotExist) err = Error::Ok;
                return err.isOk() ? kept : err;
        }
        const Error kept = keep(source, destination, false); // first: the setting would bring it back
        Error       err = Midi::router().disconnect(source, destination);
        if (err == Error::NotExist) err = Error::Ok;
        return err.isOk() ? kept : err;
}

Error KernelPatchSource::watch(Changed fn, void *arg) {
        unwatch();
        const Error err = Midi::addWatch(fn, arg);
        if (err.isOk()) {
                _fn = fn;
                _arg = arg;
        }
        return err;
}

void KernelPatchSource::unwatch() {
        if (_fn != nullptr) Midi::removeWatch(_fn, _arg);
        _fn = nullptr;
        _arg = nullptr;
        return;
}

BMDS_NAMESPACE_END
