/**
 * @file      cfg.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /cfg (devplan/cfg.md): the card's configuration, or /flash's without a card; chosen at boot,
 * never synced. Also the random seed (devplan/random.md) and the `cfg` command.
 */

#include <esp_system.h>
#include <bmds/cfg.h>
#include <bmds/command.h>
#include <bmds/log.h>
#include <bmds/random.h>
#include <bmds/randompool.h>
#include <bmds/shell.h>
#include <bmds/storage.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

#include "flashdefaults.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // The card is looked for this long at boot: its first check usually ends ~150 ms after the
        // storage service starts (an empty slot sooner); the bound is for a card that hangs.
        constexpr unsigned FirstCheckMs = 2000;
        // Directories copied for a new card, at most this deep (cfg/ssh/..., cfg/icons/48x48/...).
        constexpr int MaxDepth = 4;

        constexpr StringView CardMount("/sd0/p0");

        constinit StringView leads; // CardPath or FlashPath once chosen

        bool isDirectory(StringView path) {
                Vfs::Context context;
                auto [info, err] = Vfs::stat(path, context);
                return err.isOk() && info.isDirectory();
        }

        Error join(StringView dir, StringView name, String &out) {
                out.clear();
                Error err = out.append(dir);
                if (err.isOk()) err = out.append('/');
                if (err.isOk()) err = out.append(name);
                return err;
        }

        // @p from's files and directories into @p to (made), as they are.
        Error copyTree(StringView from, StringView to, int depth) {
                Vfs::Context context;
                Error        err = Vfs::mkdir(to, context);
                if (err.isError() && err != Error::Exists) return err;
                if (depth >= MaxDepth) return Error::Ok;
                auto [dir, derr] = Vfs::openDirectory(from, context);
                if (derr.isError()) return derr;
                for (auto entry = dir.next(); entry.isOk(); entry = dir.next()) {
                        const FileInfo &info = entry.value();
                        String          a, b;
                        err = join(from, info.name.view(), a);
                        if (err.isOk()) err = join(to, info.name.view(), b);
                        if (err.isError()) return err;
                        if (info.isDirectory()) {
                                err = copyTree(a.view(), b.view(), depth + 1);
                        } else if (info.type == FileType::File) {
                                err = Vfs::copyFile(a.view(), b.view(), context);
                        }
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // The card's directory, made if it isn't there: a copy of /flash's, when it has one.
        Error prepareCard() {
                if (isDirectory(Cfg::CardPath)) return Error::Ok;
                if (!isDirectory(Cfg::FlashPath)) {
                        Vfs::Context context;
                        bmdsInfo("Cfg: made %.*s (new)", int(Cfg::CardPath.size()), Cfg::CardPath.data());
                        return Vfs::mkdir(Cfg::CardPath, context);
                }
                bmdsInfo("Cfg: made %.*s from %.*s", int(Cfg::CardPath.size()), Cfg::CardPath.data(),
                         int(Cfg::FlashPath.size()), Cfg::FlashPath.data());
                return copyTree(Cfg::FlashPath, Cfg::CardPath, 0);
        }

        // The random seed kept across boots (devplan/random.md).
        constexpr const char SeedPath[] = "/cfg/random-seed";
        constexpr size_t     SeedBytes = 64;

        constinit bool seedable = false; // /cfg is chosen (set at boot)

        // A new seed from the pool over the old one.
        Error writeSeed() {
                uint8_t seed[SeedBytes];
                Random::fill(seed, sizeof(seed));
                Vfs::Context context;
                auto [file, err] = Vfs::open(SeedPath, Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                if (err.isOk()) {
                        auto [n, werr] = file->write(ByteSpan(seed, sizeof(seed)), WaitForever);
                        err = werr.isOk() && n != sizeof(seed) ? Error(Error::IoError) : werr;
                }
                RandomPool::wipe(seed, sizeof(seed));
                return err;
        }

        // Mixes the last boot's seed in. A new one is written a few seconds later
        // (refreshRandomSeed()), so a crash soon after boot can reuse it; the boot's own noise
        // (Random::init()) is mixed in regardless.
        void seedRandom() {
                uint8_t      seed[SeedBytes];
                size_t       got = 0;
                Vfs::Context context;
                auto [file, err] = Vfs::open(SeedPath, Vfs::Read, context);
                while (err.isOk() && got < sizeof(seed)) {
                        auto [n, rerr] = file->read(WritableByteSpan(seed + got, sizeof(seed) - got), WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                file.reset();
                if (got != 0) Random::mix(seed, got);
                RandomPool::wipe(seed, sizeof(seed));
                seedable = true;
                bmdsInfo("Random: %s %s", got != 0 ? "seeded from" : "no seed yet in", SeedPath);
                return;
        }

        void saveSeedAtShutdown() {
                Cfg::refreshRandomSeed();
                return;
        }

} // namespace

Error Cfg::init() {
        const Error waited = Storage::waitForFirstMount(FirstCheckMs);
        if (waited == Error::Timeout) bmdsWarn("Cfg: the card's first check took over %u ms", FirstCheckMs);
        StringView target = FlashPath;
        if (Vfs::isMounted(CardMount)) {
                const Error err = prepareCard();
                if (err.isOk()) {
                        target = CardPath;
                } else {
                        bmdsErr("Cfg: %.*s: %s: using %.*s", int(CardPath.size()), CardPath.data(), err.desc(),
                                int(FlashPath.size()), FlashPath.data());
                }
        }
        if (target == FlashPath) {
                Vfs::Context context;
                const Error  err = Vfs::mkdir(FlashPath, context);
                if (err.isError() && err != Error::Exists)
                        bmdsErr("Cfg: %.*s: %s", int(FlashPath.size()), FlashPath.data(), err.desc());
        }
        const Error err = Vfs::setCfg(target);
        if (err.isError()) {
                bmdsErr("Cfg: /cfg: %s", err.desc());
                return err;
        }
        leads = target;
        bmdsInfo("Cfg: /cfg leads to %.*s", int(target.size()), target.data());
        FlashDefaults::install("cfg/", Vfs::CfgPath);
        seedRandom();
        const esp_err_t herr = esp_register_shutdown_handler(saveSeedAtShutdown);
        if (herr != ESP_OK) bmdsWarn("Cfg: no shutdown handler for %s: %s", SeedPath, Error::fromEspErr(herr).desc());
        return Error::Ok;
}

StringView Cfg::leadsTo() {
        return leads;
}

void Cfg::refreshRandomSeed() {
        if (!seedable) return;
        const Error err = writeSeed();
        if (err.isError()) bmdsWarn("Cfg: %s: %s", SeedPath, err.desc());
        return;
}

namespace {

        int runCfg(CommandContext &ctx) {
                if (ctx.args().size() != 1) return ctx.usage();
                const StringView to = Cfg::leadsTo();
                if (to.isEmpty()) {
                        ctx.printf("/cfg: none\n");
                        return Shell::ExitFailure;
                }
                ctx.printf("/cfg leads to %.*s (%s)\n", int(to.size()), to.data(),
                           to == Cfg::CardPath ? "the card's" : "the board's flash");
                if (!isDirectory(to)) {
                        ctx.printf("It isn't there (the card was taken out): settings can't be saved until "
                                   "the card is back, or the next restart uses %.*s.\n",
                                   int(Cfg::FlashPath.size()), Cfg::FlashPath.data());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

} // namespace

BMDS_SHELL_COMMAND(cfg, runCfg, "",
                   "Show where /cfg, the configuration, leads: the card's cfg/ when there was a card at boot, "
                   "else /flash/cfg")

BMDS_NAMESPACE_END
