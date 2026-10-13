/**
 * @file      voices.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The voices speakers share (devplan/speech.md §3): found in speech/path,
 * mapped from /rom or read into PSRAM.
 */

#include <cstring>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>
#include <bmds/voices.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // devplan/speech.md §4: the keys are an interface once shipped.
        BMDS_SYSCONF(speechPath, "speech/path", Config::Type::String, Voices::DefaultPath,
                     "The directories text-to-speech voices are looked for in, : between them", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(speechVoice, "speech/voice", Config::Type::String, Voices::DefaultVoice,
                     "The voice say and programs speak with when they name none", 0, 0, nullptr, nullptr)

        constexpr StringView AnalysisSuffix = "_ta.bin";
        constexpr StringView SignalSuffix = "_sg.bin";
        constexpr size_t     MaxListed = 32; // names list() remembers, to give each once

        // How far Pico's quieter voices may be raised and still peak no higher than the loudest
        // (es-ES, it-IT, de-DE): the least, over pitches 50-200, of their worst peaks' distance
        // below the loudest's (devplan/speech.md §13). Another voice gets none: as loud as those.
        struct Boost {
                        StringView name;
                        float      db;
        };
        constexpr Boost Boosts[] = {{"en-US", 1.7f}, {"en-GB", 3.1f}, {"fr-FR", 3.7f}};

        float boostOf(StringView name) {
                for (const Boost &b : Boosts)
                        if (b.name == name) return b.db;
                return 0.0f;
        }

        const Vfs::Context Root{"/", nullptr};

} // namespace

struct Voices::Voice {
                String         name;
                Speaker::Voice files;
                uint8_t       *copies[2] = {}; // PSRAM, when not in place
                uint32_t       users = 0;
};

namespace {

        class Table {
                public:
                        Mutex         lock;
                        Voices::Voice voices[Voices::MaxInUse];
        };

        constinit PsramObject<Table> table;

        // Calls @p each for every directory in speech/path; stops at its first error.
        template <typename Each> Error eachDirectory(Each each) {
                String path;
                if (!SysConf::isReady() || SysConf::get(bmdsConf_speechPath.key, path).isError()) {
                        const Error err = path.assign(Voices::DefaultPath);
                        if (err.isError()) return err;
                }
                StringView rest = path.view();
                while (!rest.isEmpty()) {
                        const size_t     colon = rest.indexOf(':');
                        const StringView dir = colon == StringView::NPos ? rest : rest.left(colon);
                        rest = colon == StringView::NPos ? StringView() : rest.mid(colon + 1);
                        if (dir.isEmpty()) continue;
                        const Error err = each(dir);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        Error joinPath(String &out, StringView dir, StringView name) {
                Error err = out.assign(dir);
                if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(name);
                return err;
        }

        // A voice's name: not empty, no '/' (a path) or '_' (the files' separator).
        bool isVoiceName(StringView name) {
                if (name.isEmpty()) return false;
                return !name.contains('/') && !name.contains('_');
        }

        // Voice @p name's signal file in @p dir: the first LANG_*_sg.bin (empty: none).
        Error findSignal(StringView dir, StringView name, String &out) {
                out.clear();
                auto [d, err] = Vfs::openDirectory(dir, Root);
                if (err.isError()) return err;
                for (;;) {
                        auto [info, nerr] = d.next();
                        if (nerr == Error::EndOfFile) return Error::Ok;
                        if (nerr.isError()) return nerr;
                        const StringView n = info.name.view();
                        if (info.type == FileType::Directory || n.size() <= name.size() + 1 + SignalSuffix.size() ||
                            !n.startsWith(name) || n[name.size()] != '_' || !n.endsWith(SignalSuffix))
                                continue;
                        return joinPath(out, dir, n);
                }
        }

        // @p path's bytes: where they lie, else read into PSRAM (@p copy set to free).
        Result<ByteSpan> readVoiceFile(StringView path, uint8_t *&copy) {
                copy = nullptr;
                auto [mapped, merr] = Vfs::map(path, Root);
                if (merr.isOk()) return makeResult(mapped);
                if (merr != Error::NotSupported) return makeError<ByteSpan>(merr);
                auto [info, serr] = Vfs::stat(path, Root);
                if (serr.isError()) return makeError<ByteSpan>(serr);
                if (info.size == 0 || info.size > 64u * 1024 * 1024) return makeError<ByteSpan>(Error::CorruptData);
                auto [file, oerr] = Vfs::open(path, Vfs::Read, Root);
                if (oerr.isError()) return makeError<ByteSpan>(oerr);
                const auto size = static_cast<size_t>(info.size);
                copy = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size));
                if (copy == nullptr) return makeError<ByteSpan>(Error::NoMem);
                size_t done = 0;
                while (done < size) {
                        auto [n, rerr] = file->read(WritableByteSpan(copy + done, size - done), WaitForever);
                        if (rerr.isOk() && n == 0) rerr = Error::EndOfFile;
                        if (rerr.isError()) {
                                Memory::free(copy);
                                copy = nullptr;
                                return makeError<ByteSpan>(rerr == Error::EndOfFile ? Error::CorruptData : rerr);
                        }
                        done += n;
                }
                return makeResult(ByteSpan(copy, size));
        }

        // With the lock held.
        void unload(Voices::Voice &v) {
                Memory::free(v.copies[0]);
                Memory::free(v.copies[1]);
                v.copies[0] = v.copies[1] = nullptr;
                v.files = {};
                v.name.clear();
                v.users = 0;
                return;
        }

} // namespace

Error Voices::init() {
        return table.create() != nullptr ? Error::Ok : Error::NoMem;
}

Error Voices::defaultVoice(String &out) {
        if (SysConf::isReady() && SysConf::get(bmdsConf_speechVoice.key, out).isOk() && !out.isEmpty())
                return Error::Ok;
        return out.assign(DefaultVoice);
}

Error Voices::find(StringView name, Found &found) {
        if (!isVoiceName(name)) return Error::Invalid;
        bool        have = false;
        const Error err = eachDirectory([&](StringView dir) -> Error {
                String analysis;
                Error  e = joinPath(analysis, dir, name);
                if (e.isOk()) e = analysis.append(AnalysisSuffix);
                if (e.isError()) return e;
                if (Vfs::stat(analysis.view(), Root).isError()) return Error::Ok;
                String signal;
                e = findSignal(dir, name, signal);
                if (e.isError() || signal.isEmpty()) return Error::Ok; // a directory unreadable, or half a voice
                e = found.name.assign(name);
                if (e.isOk()) found.analysis = std::move(analysis);
                if (e.isOk()) found.signal = std::move(signal);
                have = e.isOk();
                return e.isOk() ? Error::Cancelled : e; // found: stop
        });
        if (err.isError() && err != Error::Cancelled) return err;
        return have ? Error::Ok : Error::NotExist;
}

Error Voices::list(Error (*each)(void *arg, const Found &found), void *arg) {
        String listed[MaxListed];
        size_t count = 0;
        return eachDirectory([&](StringView dir) -> Error {
                auto [d, derr] = Vfs::openDirectory(dir, Root);
                if (derr.isError()) return Error::Ok; // not there: the next
                for (;;) {
                        auto [info, nerr] = d.next();
                        if (nerr == Error::EndOfFile) return Error::Ok;
                        if (nerr.isError()) return Error::Ok;
                        const StringView n = info.name.view();
                        if (info.type == FileType::Directory || n.size() <= AnalysisSuffix.size() ||
                            !n.endsWith(AnalysisSuffix))
                                continue;
                        const StringView name = n.left(n.size() - AnalysisSuffix.size());
                        if (!isVoiceName(name)) continue;
                        bool seen = false;
                        for (size_t i = 0; i < count && !seen; ++i) seen = listed[i].view() == name;
                        if (seen) continue;
                        Found found;
                        Error err = findSignal(dir, name, found.signal);
                        if (err.isError() || found.signal.isEmpty()) continue;
                        err = found.name.assign(name);
                        if (err.isOk()) err = joinPath(found.analysis, dir, n);
                        if (err.isOk() && count < MaxListed) err = listed[count++].assign(name);
                        if (err.isOk()) err = each(arg, found);
                        if (err.isError()) return err;
                }
        });
}

Result<Voices::Voice *> Voices::acquire(StringView asked) {
        String name;
        Error  err = asked.isEmpty() ? defaultVoice(name) : name.assign(asked);
        if (err.isError()) return makeError<Voice *>(err);
        Table        &t = *table;
        Mutex::Locker lock(t.lock, WaitForever);
        Voice        *place = nullptr;
        for (Voice &v : t.voices) {
                if (v.users != 0 && v.name.view() == name.view()) {
                        ++v.users;
                        return makeResult(&v);
                }
                if (v.users == 0 && place == nullptr) place = &v;
        }
        if (place == nullptr) return makeError<Voice *>(Error::Busy);
        // Found and read with the lock held: a second user of the same voice waits for it, not a copy.
        Found found;
        err = find(name.view(), found);
        if (err.isError()) return makeError<Voice *>(err);
        auto [analysis, aerr] = readVoiceFile(found.analysis.view(), place->copies[0]);
        if (aerr.isError()) return makeError<Voice *>(aerr);
        auto [signal, serr] = readVoiceFile(found.signal.view(), place->copies[1]);
        if (serr.isError()) {
                unload(*place);
                return makeError<Voice *>(serr);
        }
        place->files = {analysis, signal, boostOf(found.name.view())};
        place->name = std::move(found.name);
        place->users = 1;
        bmdsInfo("speech: %s: %s", place->name.cStr(),
                 place->copies[0] == nullptr && place->copies[1] == nullptr ? "read in place" : "read into memory");
        return makeResult(place);
}

void Voices::release(Voice *voice) {
        if (voice == nullptr) return;
        Mutex::Locker lock(table->lock, WaitForever);
        if (voice->users != 0 && --voice->users == 0) unload(*voice);
        return;
}

Speaker::Voice Voices::files(const Voice *voice) {
        return voice->files;
}

StringView Voices::name(const Voice *voice) {
        return voice->name.view();
}

bool Voices::isInPlace(const Voice *voice) {
        return voice->copies[0] == nullptr && voice->copies[1] == nullptr;
}

BMDS_NAMESPACE_END
