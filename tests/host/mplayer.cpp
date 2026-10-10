/**
 * @file      mplayer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The media player's pure parts (devplan/mediaplayer.md §6): the playlist and its orders, M3U,
 * the FFT and the spectrum's bands, the meter bars, the MIDI channels, the library's index, the
 * track clock, the settings.
 */

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <bmds/fft.h>

#include "libraryindex.h"
#include "m3u.h"
#include "midichannels.h"
#include "playersettings.h"
#include "playlist.h"
#include "spectrum.h"
#include "trackclock.h"

using bmds::Error;
using bmds::Playlist;
using bmds::StringView;

namespace {

        std::string str(StringView v) {
                return std::string(v.data(), v.size());
        }

        Playlist numbered(size_t n) {
                Playlist p;
                for (size_t i = 0; i < n; ++i) REQUIRE(p.append(("/m/" + std::to_string(i) + ".mp3").c_str()).isOk());
                return p;
        }

        // The entries a whole shuffled round plays, from follow() at the start.
        std::vector<size_t> round(Playlist &p, size_t n) {
                std::vector<size_t> played;
                for (size_t i = 0; i < n; ++i)
                        played.push_back(i == 0 && p.current() != Playlist::NoEntry ? p.current() : p.follow());
                return played;
        }

} // namespace

TEST_CASE("Playlist: in order, the end, Repeat All and One, Next and Previous") {
        Playlist p = numbered(3);
        CHECK(p.current() == Playlist::NoEntry);
        CHECK(p.follow() == 0);
        CHECK(p.follow() == 1);
        CHECK(p.follow() == 2);
        CHECK(p.follow() == Playlist::NoEntry);
        CHECK(p.current() == Playlist::NoEntry);
        CHECK(p.follow() == 0); // starts again from the top

        p.setRepeat(Playlist::Repeat::All);
        p.setCurrent(2);
        CHECK(p.follow() == 0);

        p.setRepeat(Playlist::Repeat::One);
        CHECK(p.follow() == 0);
        CHECK(p.follow() == 0);
        CHECK(p.next() == 1); // Next moves on with Repeat One
        CHECK(p.previous() == 0);
        CHECK(p.previous() == 0); // the first stays

        p.setRepeat(Playlist::Repeat::Off);
        p.setCurrent(2);
        CHECK(p.next() == Playlist::NoEntry);
        Playlist empty;
        CHECK(empty.follow() == Playlist::NoEntry);
        CHECK(empty.next() == Playlist::NoEntry);
        CHECK(empty.previous() == Playlist::NoEntry);
}

TEST_CASE("Playlist: edits keep the current entry, and removing it leaves its follower next") {
        Playlist p = numbered(5);
        p.setCurrent(2);
        REQUIRE(p.insert(0, "/m/new.mp3").isOk());
        CHECK(p.current() == 3);
        CHECK(str(p.path(3)) == "/m/2.mp3");
        p.remove(0);
        CHECK(p.current() == 2);
        p.move(2, 4);
        CHECK(p.current() == 4);
        CHECK(str(p.path(4)) == "/m/2.mp3");
        p.move(0, 4);
        CHECK(p.current() == 3);
        p.move(3, 0);
        CHECK(p.current() == 0);
        CHECK(str(p.path(0)) == "/m/2.mp3");

        // Removing the one playing: none current; the next is the one that followed.
        p.remove(0);
        CHECK(p.current() == Playlist::NoEntry);
        CHECK(p.follow() == 0);
        CHECK(str(p.path(0)) == "/m/1.mp3"); // the list was 2, 1, 3, 4, 0

        // ...and one inserted where it was plays next.
        p.setCurrent(1);
        p.remove(1);
        REQUIRE(p.insert(1, "/m/x.mp3").isOk());
        CHECK(p.follow() == 1);
        CHECK(str(p.path(1)) == "/m/x.mp3");

        // Removing the last while it plays: the end next.
        p.setCurrent(p.count() - 1);
        p.remove(p.count() - 1);
        CHECK(p.follow() == Playlist::NoEntry);

        while (p.count() > 0) p.remove(0);
        CHECK(p.current() == Playlist::NoEntry);
        CHECK(p.follow() == Playlist::NoEntry);

        CHECK(p.append("").isError());
        CHECK(p.append(StringView()) == Error::Invalid);
}

TEST_CASE("Playlist: information, the texts compacted, the most entries") {
        Playlist       p = numbered(3);
        Playlist::Info info;
        info.title = "Axel F";
        info.artist = "Harold Faltermeyer";
        info.album = "Beverly Hills Cop";
        info.format = "MIDI";
        info.lengthMs = 180000;
        info.known = true;
        REQUIRE(p.setInfo(1, info).isOk());
        // Many changes: the old texts are compacted away, the current ones unharmed.
        for (int i = 0; i < 2000; ++i) {
                const std::string t = "Title " + std::to_string(i);
                info.title = t.c_str();
                REQUIRE(p.setInfo(i % 3, info).isOk());
        }
        CHECK(str(p.info(0).title) == "Title 1998");
        CHECK(str(p.info(1).title) == "Title 1999");
        CHECK(str(p.info(2).title) == "Title 1997");
        CHECK(str(p.info(1).artist) == "Harold Faltermeyer");
        CHECK(p.info(1).lengthMs == 180000);
        CHECK(str(p.path(2)) == "/m/2.mp3");
        p.setError(2, Error::CorruptData);
        CHECK(p.error(2) == Error::CorruptData);
        CHECK(p.error(1) == Error::Ok);

        Playlist full = numbered(Playlist::MaxEntries);
        CHECK(full.append("/m/one-more.mp3") == Error::Full);
}

TEST_CASE("Playlist: shuffle plays each entry once a round, the chosen one first, never twice in a row") {
        for (uint32_t seed = 1; seed <= 50; ++seed) {
                Playlist p = numbered(20);
                p.setSeed(seed);
                REQUIRE(p.setShuffled(true).isOk());
                p.setCurrent(5);
                CHECK(p.current() == 5);
                std::vector<size_t> first = round(p, 20);
                CHECK(first[0] == 5);
                CHECK(std::set<size_t>(first.begin(), first.end()).size() == 20);
                CHECK(p.follow() == Playlist::NoEntry); // Repeat Off: the end

                p.setRepeat(Playlist::Repeat::All);
                p.setCurrent(3);
                std::vector<size_t> a = round(p, 20);
                std::vector<size_t> b;
                for (int i = 0; i < 20; ++i) b.push_back(p.follow());
                CHECK(std::set<size_t>(b.begin(), b.end()).size() == 20);
                CHECK(b[0] != a.back());
        }
}

TEST_CASE("Playlist: shuffled edits: added ones play this round, removed ones don't; off keeps the place") {
        Playlist p = numbered(10);
        p.setSeed(7);
        REQUIRE(p.setShuffled(true).isOk());
        p.setCurrent(0);
        std::vector<size_t> seen{p.current()};
        for (int i = 0; i < 3; ++i) seen.push_back(p.follow());
        REQUIRE(p.append("/m/added.mp3").isOk()); // entry 10
        // Remove one not yet played (the highest numbered not seen).
        size_t gone = 9;
        while (std::find(seen.begin(), seen.end(), gone) != seen.end()) --gone;
        const std::string gonePath = str(p.path(gone));
        const size_t      playing = p.current();
        p.remove(gone);
        CHECK(p.current() == (playing > gone ? playing - 1 : playing));
        std::vector<std::string> rest;
        for (size_t e = p.follow(); e != Playlist::NoEntry; e = p.follow()) rest.push_back(str(p.path(e)));
        CHECK(rest.size() == 6); // 10 + 1 − 1, less the 4 played
        CHECK(std::find(rest.begin(), rest.end(), "/m/added.mp3") != rest.end());
        CHECK(std::find(rest.begin(), rest.end(), gonePath) == rest.end());

        // Shuffle off: the same entry stays current, and order is the list's again.
        p.setCurrent(4);
        REQUIRE(p.setShuffled(false).isOk());
        CHECK(p.current() == 4);
        CHECK(p.follow() == 5);
        // Moving entries while shuffled keeps the current one.
        REQUIRE(p.setShuffled(true).isOk());
        const std::string current = str(p.path(p.current()));
        p.move(p.current(), 0);
        CHECK(str(p.path(p.current())) == current);
        p.move(0, p.count() - 1);
        CHECK(str(p.path(p.current())) == current);
}

TEST_CASE("M3U: extended and plain, relative paths, URLs skipped, written back the same") {
        const char text[] = "\xEF\xBB\xBF#EXTM3U\r\n"
                            "#EXTINF:181,Klaus Badelt - He's a Pirate\r\n"
                            "pirates.mid\r\n"
                            "\r\n"
                            "# a comment\n"
                            "#EXTINF:-1 tvg-name=\"x,y\",Just A Title\n"
                            "sub\\dir/song.mp3\n"
                            "../up.flac\n"
                            "/sd0/p0/abs.ogg\n"
                            "http://example.com/stream.mp3\n"
                            "file:///sd0/p0/My%20Song.wav\n";
        Playlist   p;
        auto [r, err] = bmds::M3u::read(text, "/sd0/p0/music", p);
        REQUIRE(err.isOk());
        CHECK(r.added == 5);
        CHECK(r.skipped == 1);
        REQUIRE(p.count() == 5);
        CHECK(str(p.path(0)) == "/sd0/p0/music/pirates.mid");
        CHECK(str(p.info(0).title) == "He's a Pirate");
        CHECK(str(p.info(0).artist) == "Klaus Badelt");
        CHECK(p.info(0).lengthMs == 181000);
        CHECK_FALSE(p.info(0).known);
        CHECK(str(p.path(1)) == "/sd0/p0/music/sub/dir/song.mp3");
        CHECK(str(p.info(1).title) == "Just A Title");
        CHECK(p.info(1).lengthMs == 0);
        CHECK(str(p.path(2)) == "/sd0/p0/up.flac");
        CHECK(str(p.info(2).title).empty());
        CHECK(str(p.path(3)) == "/sd0/p0/abs.ogg");
        CHECK(str(p.path(4)) == "/sd0/p0/My Song.wav");

        bmds::String out;
        REQUIRE(bmds::M3u::write(p, "/sd0/p0/music", out).isOk());
        CHECK(str(out.view()) == "#EXTM3U\n"
                                 "#EXTINF:181,Klaus Badelt - He's a Pirate\n"
                                 "pirates.mid\n"
                                 "#EXTINF:-1,Just A Title\n"
                                 "sub/dir/song.mp3\n"
                                 "/sd0/p0/up.flac\n"
                                 "/sd0/p0/abs.ogg\n"
                                 "/sd0/p0/My Song.wav\n");
        Playlist again;
        auto [r2, err2] = bmds::M3u::read(out.view(), "/sd0/p0/music", again);
        REQUIRE(err2.isOk());
        REQUIRE(again.count() == p.count());
        for (size_t i = 0; i < p.count(); ++i) CHECK(str(again.path(i)) == str(p.path(i)));

        // A list at the root.
        Playlist root;
        REQUIRE(root.append("/a.mp3").isOk());
        REQUIRE(bmds::M3u::write(root, "/", out).isOk());
        CHECK(str(out.view()) == "#EXTM3U\na.mp3\n");
        Playlist none;
        REQUIRE(bmds::M3u::read("", "/", none).isOk());
        CHECK(none.count() == 0);
}

TEST_CASE("Fft: every size against a plain DFT; a full-scale sine's peak is N/2") {
        std::mt19937                          rng(42);
        std::uniform_real_distribution<float> dist(-1, 1);
        for (size_t n = bmds::Fft::MinPoints; n <= 2048; n *= 2) {
                bmds::Fft fft;
                REQUIRE(fft.init(n, bmds::MemSpace::Psram).isOk());
                std::vector<float> x(n);
                for (float &v : x) v = dist(rng);
                std::vector<float> data = x;
                fft.forward(data.data());
                double worst = 0;
                for (size_t k = 0; k <= n / 2; ++k) {
                        double re = 0;
                        double im = 0;
                        for (size_t t = 0; t < n; ++t) {
                                re += x[t] * std::cos(2 * M_PI * k * t / n);
                                im -= x[t] * std::sin(2 * M_PI * k * t / n);
                        }
                        double gotRe;
                        double gotIm = 0;
                        if (k == 0) {
                                gotRe = data[0];
                        } else if (k == n / 2) {
                                gotRe = data[1];
                        } else {
                                gotRe = data[2 * k];
                                gotIm = data[2 * k + 1];
                        }
                        worst = std::max(worst, std::hypot(gotRe - re, gotIm - im));
                }
                CAPTURE(n);
                CHECK(worst < 1e-5 * n);
        }
        bmds::Fft fft;
        CHECK(fft.init(1000, bmds::MemSpace::Psram) == Error::Invalid);
        CHECK(fft.init(2, bmds::MemSpace::Psram) == Error::Invalid);
        CHECK(fft.init(16384, bmds::MemSpace::Psram) == Error::Invalid);
        REQUIRE(fft.init(1024, bmds::MemSpace::Psram).isOk());
        std::vector<float> sine(1024);
        for (size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(std::sin(2 * M_PI * 37 * i / 1024.0));
        fft.forward(sine.data());
        std::vector<float> power(513);
        bmds::Fft::power(sine.data(), 1024, power.data());
        CHECK(std::sqrt(power[37]) == doctest::Approx(512).epsilon(1e-4));
        CHECK(std::sqrt(power[36]) < 0.01);
}

TEST_CASE("Spectrum: a full-scale sine near 0 dB in its band, quiet elsewhere; silence at the floor") {
        bmds::Spectrum s;
        REQUIRE(s.init(1024, 48000, bmds::MemSpace::Psram).isOk());
        std::vector<float> samples(1024);
        std::vector<float> work(1024);
        float              db[bmds::Spectrum::Bands];
        for (const double hz : {250.0, 1000.0, 5000.0}) {
                for (size_t i = 0; i < samples.size(); ++i)
                        samples[i] = static_cast<float>(std::sin(2 * M_PI * hz * i / 48000.0));
                s.analyse(samples.data(), work.data(), db);
                size_t loudest = 0;
                for (size_t b = 1; b < bmds::Spectrum::Bands; ++b)
                        if (db[b] > db[loudest]) loudest = b;
                // The band hz is in.
                const double place = std::log(hz / 40.0) / std::log(16000.0 / 40.0) * bmds::Spectrum::Bands;
                CAPTURE(hz);
                CHECK(std::abs(static_cast<double>(loudest) - place) <= 1.0);
                CHECK(db[loudest] > -3.5f);
                CHECK(db[loudest] < 0.5f);
                CHECK(db[bmds::Spectrum::Bands - 1] < -40); // 16 kHz: far away
        }
        std::fill(samples.begin(), samples.end(), 0.0f);
        s.analyse(samples.data(), work.data(), db);
        for (const float v : db) CHECK(v == bmds::Spectrum::FloorDb);
        CHECK(s.init(1024, 0, bmds::MemSpace::Psram) == Error::Invalid);
}

TEST_CASE("MeterBars: up at once, down 20 dB a second, the peak held a second") {
        bmds::MeterBars bars(2, -60);
        CHECK(bars.isAtFloor());
        const float loud[2] = {0, -30};
        bars.update(loud, 0);
        CHECK(bars.level(0) == 0);
        CHECK(bars.peak(0) == 0);
        bars.fall(500);
        CHECK(bars.level(0) == doctest::Approx(-10));
        CHECK(bars.peak(0) == 0); // held
        bars.fall(600);
        CHECK(bars.level(0) == doctest::Approx(-22));
        CHECK(bars.peak(0) == doctest::Approx(-2)); // 100 ms past the hold
        bars.fall(10000);
        CHECK(bars.isAtFloor());
        const float under[2] = {-100, -100};
        bars.update(under, 30);
        CHECK(bars.level(1) == -60);
        bars.reset();
        CHECK(bars.isAtFloor());
}

TEST_CASE("MidiChannels: notes, programs and controllers at the frame heard; tempo, bars and beats") {
        using Kind = bmds::MidiChannels::Activity::Kind;
        auto ev = [](uint64_t frame, Kind kind, uint8_t channel, uint8_t a, uint8_t b, uint32_t value = 0) {
                bmds::MidiChannels::Activity x;
                x.frame = frame;
                x.kind = kind;
                x.channel = channel;
                x.a = a;
                x.b = b;
                x.value = value;
                return x;
        };
        auto m = std::make_unique<bmds::MidiChannels>(48000);
        m->add(ev(100, Kind::Program, 0, 30, 0));
        m->add(ev(100, Kind::Control, 0, 7, 90));
        m->add(ev(200, Kind::NoteOn, 0, 60, 100));
        m->add(ev(200, Kind::NoteOn, 0, 64, 70));
        m->add(ev(300, Kind::NoteOn, 9, 36, 127));
        m->add(ev(400, Kind::NoteOff, 0, 60, 0));
        m->add(ev(500, Kind::NoteOn, 0, 64, 0)); // a note-on of velocity 0 is an off
        m->advanceTo(99);
        CHECK(m->channel(0).program == 0);
        m->advanceTo(250);
        CHECK(m->channel(0).program == 30);
        CHECK(m->channel(0).volume == 90);
        CHECK(m->channel(0).notes == 2);
        CHECK(m->channel(0).loudest == 100);
        CHECK(m->channel(9).notes == 0);
        m->advanceTo(400);
        CHECK(m->channel(9).loudest == 127);
        CHECK(m->channel(0).notes == 1);
        CHECK(m->channel(0).loudest == 70);
        m->advanceTo(500);
        CHECK(m->channel(0).notes == 0);
        CHECK(m->channel(0).loudest == 0);
        CHECK(m->takeStruck(0) == 100);
        CHECK(m->takeStruck(0) == 0);
        CHECK(m->channel(0).used);
        CHECK_FALSE(m->channel(1).used);

        // 120 beats a minute at 48 kHz: a beat every 24,000 frames.
        m->reset();
        m->advanceTo(23999);
        CHECK(m->position().beat == 1);
        m->advanceTo(24000);
        CHECK(m->position().beat == 2);
        m->advanceTo(4 * 24000);
        CHECK(m->position().bar == 2);
        CHECK(m->position().beat == 1);
        // 3/4 from bar 3; then 60 beats a minute.
        m->add(ev(8 * 24000, Kind::TimeSignature, 0, 3, 4));
        m->add(ev(8 * 24000, Kind::Tempo, 0, 0, 0, 1000000));
        m->advanceTo(8 * 24000);
        CHECK(m->position().bar == 3);
        CHECK(m->position().beatsPerBar == 3);
        m->advanceTo(8 * 24000 + 3 * 48000);
        CHECK(m->position().bar == 4);
        CHECK(m->position().beat == 1);
        CHECK(m->position().tempo == 1000000);

        // A reset; the queue's overflow noted.
        m->add(ev(1000000, Kind::Reset, 0, 0, 0));
        m->advanceTo(1000000);
        CHECK(m->channel(0).program == 0);
        CHECK_FALSE(m->hasOverflowed());
        for (size_t i = 0; i <= bmds::MidiChannels::QueueSize; ++i) m->add(ev(2000000 + i, Kind::NoteOn, 1, 60, 1));
        CHECK(m->hasOverflowed());
        m->reset();
        CHECK_FALSE(m->hasOverflowed());
}

TEST_CASE("LibraryIndex: sorted by path, replaced, escapes, bad lines, written and read back") {
        bmds::LibraryIndex         index;
        bmds::LibraryIndex::Record r;
        r.path = "/m/b.mp3";
        r.size = 4000000;
        r.modified = 1790000000;
        r.lengthMs = 200500;
        r.format = "MP3";
        r.title = "Tab\there";
        r.artist = "Back\\slash";
        r.album = "Two\nlines";
        r.track = 3;
        r.year = 1984;
        REQUIRE(index.set(r).isOk());
        r.path = "/m/a.mp3";
        r.title = "A";
        REQUIRE(index.set(r).isOk());
        r.path = "/m/c.mp3";
        r.modified = -5;
        REQUIRE(index.set(r).isOk());
        REQUIRE(index.count() == 3);
        CHECK(str(index.record(0).path) == "/m/a.mp3");
        CHECK(index.find("/m/b.mp3") == 1);
        CHECK(index.find("/m/bb.mp3") == -1);
        r.path = "/m/a.mp3";
        r.title = "A again";
        REQUIRE(index.set(r).isOk());
        CHECK(index.count() == 3);
        CHECK(str(index.record(0).title) == "A again");

        bmds::String out;
        REQUIRE(index.write(out).isOk());
        bmds::LibraryIndex back;
        auto [bad, err] = back.read(out.view());
        REQUIRE(err.isOk());
        CHECK(bad == 0);
        REQUIRE(back.count() == 3);
        for (size_t i = 0; i < 3; ++i) {
                const auto x = index.record(i);
                const auto y = back.record(i);
                CHECK(str(x.path) == str(y.path));
                CHECK(str(x.title) == str(y.title));
                CHECK(str(x.artist) == str(y.artist));
                CHECK(str(x.album) == str(y.album));
                CHECK(x.size == y.size);
                CHECK(x.modified == y.modified);
                CHECK(x.lengthMs == y.lengthMs);
                CHECK(x.track == y.track);
                CHECK(x.year == y.year);
        }
        CHECK(str(back.record(1).title) == "Tab\there");

        // Lines it can't read are counted and skipped; extra fields are ignored.
        bmds::LibraryIndex other;
        auto [bad2, err2] = other.read("#bmds-library 1\n"
                                       "/x.mp3\t1\t2\t3\tMP3\tT\tA\tL\t1\t2\textra\tmore\r\n"
                                       "/short.mp3\t1\t2\n"
                                       "/bad.mp3\tone\t2\t3\tMP3\tT\tA\tL\t1\t2\n"
                                       "\t1\t2\t3\tMP3\tT\tA\tL\t1\t2\n"
                                       "/y.mp3\t1\t2\t3\tMP3\tT\tA\tL\t1\t99999\n");
        REQUIRE(err2.isOk());
        CHECK(bad2 == 4);
        REQUIRE(other.count() == 1);
        CHECK(str(other.record(0).album) == "L");

        index.remove("/m/b.mp3");
        CHECK(index.count() == 2);
        CHECK(index.find("/m/b.mp3") == -1);
        index.remove("/m/none.mp3");
        CHECK(index.count() == 2);
        index.clear();
        CHECK(index.count() == 0);
}

TEST_CASE("TrackClock: what is heard, across seeks and gapless changes") {
        using bmds::TrackClock;
        TrackClock clock;
        CHECK(clock.at(1000).track == TrackClock::NoTrack);

        // Track 1 from its start at stream frame 0.
        clock.start(1, 0, 0, true);
        CHECK(clock.shown() == 1);
        CHECK(clock.at(0).frame == 0);
        CHECK(clock.at(4800).frame == 4800);

        // A seek to file frame 96000, written from stream frame 9600 (the old sound thrown away):
        // until it is heard, the target is shown.
        clock.start(1, 9600, 96000, true);
        CHECK(clock.at(5000).frame == 96000);
        CHECK(clock.at(9600).frame == 96000);
        CHECK(clock.at(10600).frame == 97000);

        // Track 2 gapless after it, from stream frame 50000: track 1 shown until then.
        clock.start(2, 50000, 0, false);
        CHECK(clock.newest() == 2);
        TrackClock::Now now = clock.at(49999);
        CHECK(now.track == 1);
        CHECK(now.frame == 96000 + 49999 - 9600);
        now = clock.at(50000);
        CHECK(now.track == 2);
        CHECK(now.frame == 0);
        CHECK(clock.shown() == 2);
        // Track 1's mark is gone: earlier frames show track 2 at its start.
        CHECK(clock.at(40000).track == 2);
        CHECK(clock.at(40000).frame == 0);

        // Short tracks ahead: past MaxMarks the oldest goes.
        clock.start(3, 60000, 0, false);
        clock.start(4, 61000, 0, false);
        clock.start(5, 62000, 0, false);
        clock.start(6, 63000, 10, false);
        CHECK(clock.shown() == 3);
        CHECK(clock.at(62500).track == 5);
        CHECK(clock.at(62500).frame == 500);
        CHECK(clock.at(70000).track == 6);
        CHECK(clock.at(70000).frame == 7010);

        // A stream configured again counts from 0: a fresh mark forgets the rest.
        clock.start(7, 0, 0, true);
        CHECK(clock.at(100).track == 7);
        CHECK(clock.at(100).frame == 100);
        clock.clear();
        CHECK(clock.shown() == TrackClock::NoTrack);
}

TEST_CASE("TrackClock: frames heard from a status") {
        using bmds::TrackClock;
        TrackClock::Sample s;
        s.playedFrames = 48000;
        s.timestampNs = 1000000000;
        s.lagFrames = 1920;
        s.rateHz = 48000;
        s.playing = true;
        // At the status: played less what the device still holds.
        CHECK(TrackClock::heard(s, 1000000000) == 48000 - 1920);
        // 10 ms later: moved on 480 frames.
        CHECK(TrackClock::heard(s, 1010000000) == 48000 - 1920 + 480);
        // A late status is moved on at most MaxExtrapolateMs.
        CHECK(TrackClock::heard(s, 3000000000) == 48000 - 1920 + 48000 * TrackClock::MaxExtrapolateMs / 1000);
        // Paused or waiting: where it is.
        s.playing = false;
        CHECK(TrackClock::heard(s, 1010000000) == 48000 - 1920);
        // No status yet, or less played than the lag: 0.
        s.playedFrames = 100;
        CHECK(TrackClock::heard(s, 1010000000) == 0);
        s.timestampNs = 0;
        s.playing = true;
        s.playedFrames = 5000;
        CHECK(TrackClock::heard(s, 1010000000) == 5000 - 1920);
}

TEST_CASE("TimeText") {
        char t[bmds::TimeText::MaxText];
        CHECK(std::string(bmds::TimeText::format(0, t)) == "0:00");
        CHECK(std::string(bmds::TimeText::format(59999, t)) == "0:59");
        CHECK(std::string(bmds::TimeText::format(201000, t)) == "3:21");
        CHECK(std::string(bmds::TimeText::format(3599999, t)) == "59:59");
        CHECK(std::string(bmds::TimeText::format(3600000, t)) == "1:00:00");
        CHECK(std::string(bmds::TimeText::format(37230000, t)) == "10:20:30");
        CHECK(std::string(bmds::TimeText::format(UINT64_MAX, t)).size() < sizeof(t));
}

TEST_CASE("PlayerSettings: defaults, read, bad values kept out, written and read back") {
        using bmds::PlayerSettings;
        using bmds::Playlist;
        PlayerSettings d;
        REQUIRE(d.parse("").isOk());
        CHECK(d.volumeDb == 0);
        CHECK(d.entry == PlayerSettings::NoEntry);
        CHECK(d.width == 0);
        CHECK(d.lastFolder.isEmpty());

        PlayerSettings s;
        REQUIRE(s.parse("# a comment\n[Playback]\nVolume=-12\nMuted=yes\nRepeat=One\nShuffle=true\nEntry=3\r\n"
                        "Position=61500\nUnknown=1\n[Window]\nWidth=700\nHeight=500\nTab=2\n"
                        "[Folders]\nLast=/home/Music\n[Other]\nVolume=-30\n")
                        .isOk());
        CHECK(s.volumeDb == -12);
        CHECK(s.muted);
        CHECK(s.repeat == Playlist::Repeat::One);
        CHECK(s.shuffled);
        CHECK(s.entry == 2);
        CHECK(s.positionMs == 61500);
        CHECK(s.width == 700);
        CHECK(s.height == 500);
        CHECK(s.tab == 2);
        CHECK(std::string(s.lastFolder.cStr()) == "/home/Music");

        // Out of range or unreadable: the value before stays.
        REQUIRE(s.parse("[Playback]\nVolume=-61\nRepeat=sometimes\nEntry=0\nPosition=x\nMuted=maybe\n"
                        "[Window]\nWidth=0\nHeight=4001\nTab=-1\n[Folders]\nLast=relative\n")
                        .isOk());
        CHECK(s.volumeDb == -12);
        CHECK(s.repeat == Playlist::Repeat::One);
        CHECK(s.entry == 2);
        CHECK(s.positionMs == 61500);
        CHECK(s.muted);
        CHECK(s.width == 700);
        CHECK(s.height == 500);
        CHECK(s.tab == 2);
        CHECK(std::string(s.lastFolder.cStr()) == "/home/Music");

        bmds::String text(bmds::MemSpace::Psram);
        REQUIRE(s.write(text).isOk());
        PlayerSettings back;
        REQUIRE(back.parse(text.view()).isOk());
        CHECK(back.volumeDb == -12);
        CHECK(back.muted);
        CHECK(back.repeat == Playlist::Repeat::One);
        CHECK(back.shuffled);
        CHECK(back.entry == 2);
        CHECK(back.positionMs == 61500);
        CHECK(back.width == 700);
        CHECK(back.height == 500);
        CHECK(back.tab == 2);
        CHECK(std::string(back.lastFolder.cStr()) == "/home/Music");

        // Nothing playing and the defaults: their keys left out, read back as defaults.
        REQUIRE(d.write(text).isOk());
        CHECK(std::string(text.cStr()).find("Entry=") == std::string::npos);
        CHECK(std::string(text.cStr()).find("Width=") == std::string::npos);
        PlayerSettings again;
        REQUIRE(again.parse(text.view()).isOk());
        CHECK(again.entry == PlayerSettings::NoEntry);
        CHECK(again.width == 0);
}
