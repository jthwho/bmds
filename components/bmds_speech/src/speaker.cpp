/**
 * @file      speaker.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A Pico system and engine in one pool (devplan/speech.md §3), the voice's
 * files used in place (thirdparty/svox-pico's bmds-memory.diff).
 */

#include <cmath>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/speaker.h>

extern "C" {
#include "picoapi.h"

size_t bmds_pico_pool_used(pico_System system); // picopool.c
}

BMDS_NAMESPACE_BEGIN

namespace {

        // The voice definition's name: one to a system.
        constexpr const char *VoiceName = "voice";

        pico_Char *picoText(const char *s) {
                return reinterpret_cast<pico_Char *>(const_cast<char *>(s));
        }

        bool inRange(const Speaker::Prosody &p) {
                return p.speed >= Speaker::MinSpeed && p.speed <= Speaker::MaxSpeed && p.pitch >= Speaker::MinPitch &&
                       p.pitch <= Speaker::MaxPitch && p.volume >= Speaker::MinVolume && p.volume <= Speaker::MaxVolume;
        }

        // A failure loading the voice or making the engine. The pool holds every voice there is
        // (measured), so running out of it is a voice like no other: two text analyses, say.
        Error loadError(pico_Status status) {
                return status == PICO_EXC_OUT_OF_MEM ? Error::NotSupported : Error::CorruptData;
        }

} // namespace

Error Speaker::open(const Voice &voice, bool markup, const Prosody &prosody) {
        if (isOpen()) return Error::InvalidState;
        if (!inRange(prosody)) return Error::Invalid;
        _pool = Memory::alloc(MemSpace::Psram, PoolBytes);
        if (_pool == nullptr) return Error::NoMem;
        pico_Status status = pico_initialize(_pool, PoolBytes, &_system);
        if (status != PICO_OK) {
                close();
                return Error::NoMem;
        }
        status = pico_loadResourceInPlace(_system, voice.analysis.data(), pico_Uint32(voice.analysis.size()),
                                          &_analysis);
        if (status == PICO_OK)
                status = pico_loadResourceInPlace(_system, voice.signal.data(), pico_Uint32(voice.signal.size()),
                                                  &_signal);
        // A file loaded twice (the same one given for both) is a warning to Pico, and no resource.
        if (status == PICO_OK && (_analysis == nullptr || _signal == nullptr)) status = PICO_EXC_FILE_CORRUPT;
        char analysisName[PICO_MAX_RESOURCE_NAME_SIZE] = {};
        char signalName[PICO_MAX_RESOURCE_NAME_SIZE] = {};
        if (status == PICO_OK) status = pico_getResourceName(_system, _analysis, analysisName);
        if (status == PICO_OK) status = pico_getResourceName(_system, _signal, signalName);
        if (status == PICO_OK) {
                status = pico_createVoiceDefinition(_system, picoText(VoiceName));
                _voiceDefined = status == PICO_OK;
        }
        if (status == PICO_OK)
                status = pico_addResourceToVoiceDefinition(_system, picoText(VoiceName), picoText(analysisName));
        if (status == PICO_OK)
                status = pico_addResourceToVoiceDefinition(_system, picoText(VoiceName), picoText(signalName));
        if (status == PICO_OK) status = pico_newEngine(_system, picoText(VoiceName), &_engine);
        if (status != PICO_OK) {
                close();
                return loadError(status);
        }
        (void)pico_setEngineMarkup(_engine, markup ? 1 : 0);
        _boostDb = voice.boostDb;
        (void)setProsody(prosody);
        return Error::Ok;
}

void Speaker::close() {
        if (_engine != nullptr) (void)pico_disposeEngine(_system, &_engine);
        _engine = nullptr;
        if (_voiceDefined) (void)pico_releaseVoiceDefinition(_system, picoText(VoiceName));
        _voiceDefined = false;
        if (_signal != nullptr) (void)pico_unloadResource(_system, &_signal);
        _signal = nullptr;
        if (_analysis != nullptr) (void)pico_unloadResource(_system, &_analysis);
        _analysis = nullptr;
        if (_system != nullptr) (void)pico_terminate(&_system);
        _system = nullptr;
        Memory::free(_pool);
        _pool = nullptr;
        _stagedAt = _stagedEnd = 0;
        return;
}

Error Speaker::setProsody(const Prosody &prosody) {
        if (!isOpen()) return Error::InvalidState;
        if (!inRange(prosody)) return Error::Invalid;
        const pico_Status status = pico_setEngineProsody(_engine, pico_Int16(prosody.speed), pico_Int16(prosody.pitch),
                                                         pico_Int16(picoVolume(prosody, _boostDb)));
        return status == PICO_OK ? Error::Ok : Error::LibraryFailure;
}

Result<size_t> Speaker::put(ByteSpan text) {
        if (!isOpen()) return makeError<size_t>(Error::InvalidState);
        constexpr size_t MaxPut = 32767; // Pico's count is an int16
        const size_t     n = text.size() < MaxPut ? text.size() : MaxPut;
        if (n == 0) return makeResult(size_t(0));
        pico_Int16        taken = 0;
        const pico_Status status = pico_putTextUtf8(_engine, text.data(), pico_Int16(n), &taken);
        if (status != PICO_OK || taken < 0) return makeError<size_t>(Error::LibraryFailure);
        return makeResult(size_t(taken));
}

Result<size_t> Speaker::render(Span<int16_t> out) {
        if (!isOpen()) return makeError<size_t>(Error::InvalidState);
        size_t made = 0;
        while (made < out.size()) {
                if (_stagedEnd - _stagedAt >= 2) {
                        const size_t have = (_stagedEnd - _stagedAt) / 2;
                        const size_t n = have < out.size() - made ? have : out.size() - made;
                        std::memcpy(out.data() + made, _staging + _stagedAt, n * 2);
                        _stagedAt = uint16_t(_stagedAt + n * 2);
                        made += n;
                        continue;
                }
                pico_Int16        got = 0;
                pico_Int16        type = 0;
                const pico_Status status = pico_getData(_engine, _staging, pico_Int16(StagingBytes), &got, &type);
                if (status != PICO_STEP_BUSY && status != PICO_STEP_IDLE) {
                        reset();
                        return makeError<size_t>(Error::LibraryFailure);
                }
                _stagedAt = 0;
                _stagedEnd = uint16_t(got > 0 ? got : 0);
                if (status == PICO_STEP_IDLE && got <= 0) break;
        }
        return makeResult(made);
}

void Speaker::reset() {
        if (isOpen()) (void)pico_resetEngine(_engine, PICO_RESET_SOFT);
        _stagedAt = _stagedEnd = 0;
        return;
}

uint16_t Speaker::picoVolume(const Prosody &prosody, float boostDb) {
        // Pico's volume at which the loudest voice's worst peak (between samples, over speeds
        // 20-500 and five texts) was -1.5 dBFS at these pitches; interpolated between them.
        struct Point {
                        uint16_t pitch;
                        float    volume;
        };
        static constexpr Point Curve[] = {{50, 28.5f},  {60, 31.1f},  {70, 35.3f},  {85, 38.9f}, {100, 41.2f},
                                          {125, 43.2f}, {150, 45.2f}, {175, 51.9f}, {200, 53.8f}};
        constexpr size_t       Points = sizeof(Curve) / sizeof(Curve[0]);
        const uint16_t         pitch = prosody.pitch < Curve[0].pitch            ? Curve[0].pitch
                                       : prosody.pitch > Curve[Points - 1].pitch ? Curve[Points - 1].pitch
                                                                                 : prosody.pitch;
        size_t                 i = 0;
        while (i + 2 < Points && pitch > Curve[i + 1].pitch) ++i;
        const Point &a = Curve[i];
        const Point &b = Curve[i + 1];
        const float  at = a.volume + (b.volume - a.volume) * float(pitch - a.pitch) / float(b.pitch - a.pitch);
        const float  volume = at * float(prosody.volume) / 100.0f * powf(10.0f, boostDb / 20.0f);
        const float  held = volume < 1.0f ? 1.0f : (volume > 500.0f ? 500.0f : volume);
        return static_cast<uint16_t>(held + 0.5f);
}

size_t Speaker::poolUsed() const {
        return _system != nullptr ? bmds_pico_pool_used(_system) : 0;
}

BMDS_NAMESPACE_END
