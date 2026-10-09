/**
 * @file      midi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's MIDI: its router, the /dev/midi files, the settings under midi/
 * (devplan/audio.md §21).
 */

#include <atomic>
#include <bmds/log.h>
#include <bmds/midi.h>
#include <bmds/midifile.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // devplan/audio.md §21: the keys are an interface once shipped.
        BMDS_SYSCONF(midiDefault, "midi/default", Config::Type::String, "midi0",
                     "The MIDI port /dev/midi opens (midiN or a port's name)", 0,
                     static_cast<int32_t>(MidiRouter::MaxName - 1), nullptr, nullptr)
        BMDS_SYSCONF(midiConnect, "midi/connect", Config::Type::String, "",
                     "MIDI connections made whenever both ports exist: SOURCE>DEST, comma-separated", 0,
                     static_cast<int32_t>(MidiRouter::MaxAutoConnect - 1), nullptr, nullptr)

        struct System {
                        MidiRouter        router;
                        SysConf::Watcher  watcher;
                        std::atomic<bool> connectChanged{true}; // read midi/connect at the next use
        };

        constinit StaticObject<System> midi;
        constinit std::atomic<bool>    ready{false};

        // On the task that changed it, which may hold the registry: the next user reads it.
        void connectChanged(void *) {
                midi->connectChanged.store(true, std::memory_order_release);
                return;
        }

} // namespace

Error Midi::init() {
        midi.create();
        if (SysConf::isReady()) {
                const Error err = midi->watcher.start(bmdsConf_midiConnect.key, connectChanged, nullptr);
                if (err.isError()) bmdsWarn("midi: midi/connect won't be followed: %s", err.desc());
        }
        ready.store(true, std::memory_order_release);
        applySettings();
        const Error err = MidiFile::addFiles();
        if (err.isError()) bmdsWarn("midi: no /dev/midi files: %s", err.desc());
        return Error::Ok;
}

bool Midi::isReady() {
        return ready.load(std::memory_order_acquire);
}

MidiRouter &Midi::router() {
        return midi->router;
}

void Midi::applySettings() {
        System &s = *midi;
        if (!s.connectChanged.exchange(false, std::memory_order_acq_rel) || !SysConf::isReady()) return;
        String list;
        if (SysConf::get(bmdsConf_midiConnect.key, list).isOk() && s.router.setAutoConnect(list).isError())
                bmdsWarn("midi: midi/connect is too long");
        return;
}

Result<uint8_t> Midi::defaultPort() {
        String name;
        if (!SysConf::isReady() || SysConf::get(bmdsConf_midiDefault.key, name).isError())
                return router().find("midi0");
        return router().find(name);
}

const char *Midi::kindName(MidiRouter::Kind kind) {
        switch (kind) {
                case MidiRouter::Kind::Usb: return "usb";
                case MidiRouter::Kind::Serial: return "serial";
                case MidiRouter::Kind::Virtual: return "virtual";
        }
        return "?";
}

BMDS_NAMESPACE_END
