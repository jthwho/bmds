/**
 * @file      systemfonts.cpp
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cmath>
#include <bmds/bitmapfont.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/spinlock.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/systemfonts.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A setting naming a font must be one FontName reads.
        Error checkName(StringView value) {
                return FontName::parse(value).error();
        }

        BMDS_SYSCONF(fontPaths, "font/paths", Config::Type::String, "/rom/share/fonts;${HOME}/Fonts",
                     "The folders of TrueType fonts, separated by ; (each a text template)", 0, 255, nullptr, nullptr)
        BMDS_SYSCONF(fontUi, "font/ui", Config::Type::String, "IBM Plex Sans 13",
                     "The toolkit's font: a family, a size, bold or italic (devplan/fonts.md)", 0, 0, nullptr,
                     checkName)
        BMDS_SYSCONF(fontMono, "font/mono", Config::Type::String, "IBM Plex Mono 13",
                     "The font for fixed-width text outside the terminal", 0, 0, nullptr, checkName)
        BMDS_SYSCONF(fontTitle, "font/title", Config::Type::String, "IBM Plex Sans 13 bold",
                     "The font of window titles and headings", 0, 0, nullptr, checkName)
        BMDS_SYSCONF(fontTerminal, "font/terminal", Config::Type::String, "unscii 16",
                     "The terminal's font (a bitmap or fixed-width one)", 0, 0, nullptr, checkName)
        BMDS_SYSCONF(fontFallback, "font/fallback", Config::Type::String, "IBM Plex Sans;IBM Plex Mono",
                     "The families tried for a character a font lacks, separated by ; (then unscii)", 0, 255, nullptr,
                     nullptr)
        BMDS_SYSCONF(fontSmooth, "font/smooth", Config::Type::Bool, "true",
                     "Smooth (antialias) TrueType text, unless a program says otherwise", 0, 0, nullptr, nullptr)
        BMDS_SYSCONF(fontContrast, "font/contrast", Config::Type::Int, "30",
                     "How much darker smoothed text's edges are drawn: 0 as rendered, 100 the most", 0, 100, nullptr,
                     nullptr)
        BMDS_SYSCONF(fontCache, "font/cache", Config::Type::Int, "512",
                     "The glyph cache's budget in KB of PSRAM, shared by every program", 64, 8192, nullptr, nullptr)

        // The VFS as the library's files: a ROM image's mapped, others read into PSRAM.
        class VfsFiles final : public FontFiles {
                public:
                        Error list(StringView dir, Visit visit, void *arg) override {
                                auto [d, err] = Vfs::openDirectory(dir, Vfs::Context());
                                if (err.isError()) return err;
                                String path(MemSpace::Psram);
                                for (auto entry = d.next(); entry.isOk(); entry = d.next()) {
                                        const FileInfo &info = entry.value();
                                        if (info.type != FileType::File) continue;
                                        path.clear();
                                        if (path.append(dir).isError() || path.append('/').isError() ||
                                            path.append(info.name.view()).isError())
                                                continue;
                                        visit(arg, path.view());
                                }
                                return Error::Ok;
                        }

                        Result<ByteSpan> map(StringView path) override { return Vfs::map(path, Vfs::Context()); }

                        Result<Span<uint8_t>> load(StringView path) override {
                                auto [info, serr] = Vfs::stat(path, Vfs::Context());
                                if (serr.isError()) return makeError<Span<uint8_t>>(serr);
                                if (info.size == 0 || info.size > MaxFile)
                                        return makeError<Span<uint8_t>>(Error::OutOfRange);
                                auto [file, oerr] = Vfs::open(path, Vfs::Read, Vfs::Context());
                                if (oerr.isError()) return makeError<Span<uint8_t>>(oerr);
                                const size_t size = size_t(info.size);
                                auto        *p = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size));
                                if (p == nullptr) return makeError<Span<uint8_t>>(Error::NoMem);
                                for (size_t at = 0; at < size;) {
                                        auto [n, rerr] = file->read(WritableByteSpan(p + at, size - at), WaitForever);
                                        if (rerr.isError() || n == 0) {
                                                Memory::free(p);
                                                return makeError<Span<uint8_t>>(rerr.isError() ? rerr
                                                                                               : Error(Error::IoError));
                                        }
                                        at += n;
                                }
                                return makeResult(Span<uint8_t>(p, size));
                        }

                private:
                        static constexpr uint64_t MaxFile = 16u << 20; // a font file larger is refused
        };

        constinit SpinLock                       createLock;
        constinit StaticObject<Mutex>            initMutex;
        constinit StaticObject<VfsFiles>         files;
        constinit StaticObject<GlyphCache>       glyphCache;
        constinit StaticObject<FontLibrary>      fontLibrary;
        constinit StaticObject<SysConf::Watcher> watcher;
        constinit std::atomic<bool>              ready{false};
        constinit std::atomic<bool>              smooth{true};

        struct Watcher {
                        SystemFonts::Notify notify = nullptr;
                        void               *arg = nullptr;
        };
        constinit Watcher             watchers[SystemFonts::MaxWatchers] = {};
        constinit StaticObject<Mutex> watchMutex; // held while the listeners are called

        Mutex &watchLock() {
                SpinLock::Locker lock(createLock);
                if (!watchMutex.isCreated()) watchMutex.create();
                return *watchMutex;
        }

        Mutex &initLock() {
                SpinLock::Locker lock(createLock);
                if (!initMutex.isCreated()) initMutex.create();
                return *initMutex;
        }

        // The folders of font/paths, each template expanded, into @p out (separated by ;).
        Error folders(String &out) {
                String paths(MemSpace::Psram);
                if (Error err = SysConf::get("font/paths", paths); err.isError()) return err;
                StringView rest = paths.view();
                while (!rest.isEmpty()) {
                        const size_t     semi = rest.indexOf(';');
                        const StringView one = semi == StringView::NPos ? rest : rest.left(semi);
                        rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                        if (one.isEmpty()) continue;
                        String expanded(MemSpace::Psram);
                        if (SysConf::expand(one, expanded).isError()) continue;
                        if (!out.isEmpty() && out.append(';').isError()) return Error::NoMem;
                        if (out.append(expanded.view()).isError()) return Error::NoMem;
                }
                return Error::Ok;
        }

        // The roles, the fallback faces, smoothing and the budget, as the settings say.
        void apply() {
                FontLibrary &lib = *fontLibrary;
                constexpr struct {
                                FontName::Role role;
                                const char    *key;
                } Roles[] = {{FontName::Role::Ui, "font/ui"},
                             {FontName::Role::Mono, "font/mono"},
                             {FontName::Role::Title, "font/title"},
                             {FontName::Role::Terminal, "font/terminal"}};
                String value(MemSpace::Psram);
                for (const auto &r : Roles) {
                        value.clear();
                        if (SysConf::get(r.key, value).isError()) continue;
                        auto [name, err] = FontName::parse(value.view());
                        if (err.isOk() && name.role == FontName::Role::None) (void)lib.setRole(r.role, name);
                }
                value.clear();
                if (SysConf::get("font/fallback", value).isOk()) lib.setFallback(value.view());
                const Result<bool> s = SysConf::getBool("font/smooth");
                smooth.store(!s.isOk() || s.value());
                // The contrast: coverage c to 255 × (c / 255)^(1 / (1 + k / 100)), none at 0.
                const Result<int32_t> k = SysConf::getInt("font/contrast");
                const int32_t         contrast = k.isOk() ? k.value() : 30;
                if (contrast <= 0) {
                        glyphCache->setCurve(nullptr);
                } else {
                        uint8_t     curve[256];
                        const float e = 1.0f / (1.0f + float(contrast) / 100.0f);
                        for (unsigned i = 0; i < 256; ++i)
                                curve[i] = uint8_t(std::lround(255.0f * std::pow(float(i) / 255.0f, e)));
                        glyphCache->setCurve(&curve);
                }
                const Result<int32_t> kb = SysConf::getInt("font/cache");
                glyphCache->setBudget(size_t(kb.isOk() ? kb.value() : 512) * 1024);
                return;
        }

        Error scanFolders() {
                String list(MemSpace::Psram);
                if (Error err = folders(list); err.isError()) return err;
                const Error err = fontLibrary->scan(list.view());
                if (err == Error::Full)
                        bmdsWarn("fonts: more fonts than %u: the rest left out", unsigned(FontLibrary::MaxFaces));
                return err;
        }

        // A font/ setting changed: a scan if the folders did, then everything applied again.
        void changed(void *) {
                const Result<SysConf::Id> pathsId = SysConf::find("font/paths");
                bool                      rescan = watcher->overflowed();
                SysConf::Id               id;
                while (watcher->next(id))
                        if (pathsId.isOk() && id == pathsId.value()) rescan = true;
                {
                        Mutex::Locker lock(initLock(), WaitForever);
                        if (rescan) (void)scanFolders();
                        apply();
                }
                Mutex::Locker lock(watchLock(), WaitForever);
                for (const Watcher &w : watchers)
                        if (w.notify != nullptr) w.notify(w.arg);
                return;
        }

        void ensure() {
                if (ready.load(std::memory_order_acquire)) return;
                Mutex::Locker lock(initLock(), WaitForever);
                if (ready.load(std::memory_order_relaxed)) return;
                files.create();
                glyphCache.create(size_t(512) * 1024);
                fontLibrary.create(*glyphCache, &*files);
                // unscii-16's baseline is under its 13th row (its descenders take the last three).
                (void)fontLibrary->addBitmap(Fonts::unscii16, "unscii", 13);
                (void)scanFolders();
                apply();
                if (Error err = Memory::addReclaimer(&*fontLibrary); err.isError())
                        bmdsWarn("fonts: no memory reclaimer: %s", err.desc());
                watcher.create();
                if (Error err = watcher->start("font", changed, nullptr); err.isError())
                        bmdsWarn("fonts: settings not followed: %s", err.desc());
                ready.store(true, std::memory_order_release);
                return;
        }

} // namespace

FontLibrary &SystemFonts::library() {
        ensure();
        return *fontLibrary;
}

GlyphCache &SystemFonts::cache() {
        ensure();
        return *glyphCache;
}

bool SystemFonts::smoothing() {
        ensure();
        return smooth.load(std::memory_order_relaxed);
}

Error SystemFonts::watch(Notify notify, void *arg) {
        if (notify == nullptr) return Error::Invalid;
        Mutex::Locker lock(watchLock(), WaitForever);
        for (Watcher &w : watchers)
                if (w.notify == nullptr) {
                        w.notify = notify;
                        w.arg = arg;
                        return Error::Ok;
                }
        return Error::Full;
}

void SystemFonts::unwatch(Notify notify, void *arg) {
        Mutex::Locker lock(watchLock(), WaitForever);
        for (Watcher &w : watchers)
                if (w.notify == notify && w.arg == arg) w = Watcher();
        return;
}

Error SystemFonts::rescan() {
        ensure();
        Mutex::Locker lock(initLock(), WaitForever);
        const Error   err = scanFolders();
        apply();
        return err;
}

BMDS_NAMESPACE_END
