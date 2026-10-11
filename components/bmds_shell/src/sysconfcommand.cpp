/**
 * @file      sysconfcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `sysconf`: the system configuration from the shell (devplan/sysconf.md §6).
 * Its output is a userspace interface: keep the lines and their order, and
 * add new ones only at the end.
 */

#include <cstring>
#include <bmds/command.h>
#include <bmds/shell.h>
#include <bmds/sysconf.h>
#include <bmds/sysconfcommand.h>

BMDS_NAMESPACE_BEGIN

namespace {

        int fail(CommandContext &ctx, const char *key, Error err) {
                ctx.eprintf("sysconf: %s: %s\n", key, err.desc());
                return Shell::ExitFailure;
        }

        // A value as shown: a secret's says only whether it is set.
        const char *shownValue(const SysConf::Info &info) {
                if (info.param->type == Config::Type::Secret && info.value.isEmpty()) return "(not set)";
                return info.value.cStr();
        }

        void printLine(void *arg, const SysConf::Info &info) {
                auto &ctx = *static_cast<CommandContext *>(arg);
                ctx.printf("%s %s %s %s\n", info.param->key, Config::typeName(info.param->type), info.stateName(),
                           shownValue(info));
                return;
        }

        int showInfo(CommandContext &ctx, const char *key) {
                SysConf::Info info;
                const Error   err = SysConf::info(key, info);
                if (err.isError()) return fail(ctx, key, err);
                const Config::Param &p = *info.param;
                ctx.printf("key: %s\n", p.key);
                ctx.printf("type: %s\n", Config::typeName(p.type));
                ctx.printf("value: %s\n", shownValue(info));
                ctx.printf("default: %s\n", p.defaultValue);
                ctx.printf("saved: %s\n", info.hasSaved ? info.saved.cStr() : "(none)");
                ctx.printf("state: %s\n", info.stateName());
                switch (p.type) {
                        case Config::Type::Int:
                                if (p.min < p.max) ctx.printf("limits: %ld..%ld\n", long(p.min), long(p.max));
                                break;
                        case Config::Type::Enum:
                                ctx.printf("limits: %s\n", p.choices != nullptr ? p.choices : "");
                                break;
                        case Config::Type::String:
                        case Config::Type::Secret:
                        case Config::Type::Path:
                        case Config::Type::Paths:
                                ctx.printf("limits: at most %lu bytes\n",
                                           static_cast<unsigned long>(p.max > 0 ? size_t(p.max) : Config::MaxValue));
                                break;
                        default: break;
                }
                ctx.printf("description: %s\n", p.help);
                return Shell::ExitSuccess;
        }

} // namespace

int SysConfCommand::run(CommandContext &ctx) {
        const Span<char *const> args = ctx.args();
        if (args.size() < 2) return ctx.usage();
        const char *verb = args[1];
        const char *key = args.size() > 2 ? args[2] : nullptr;
        if (std::strcmp(verb, "get") == 0 && args.size() == 3) {
                SysConf::Info i;
                const Error   err = SysConf::info(key, i);
                if (err.isError()) return fail(ctx, key, err);
                ctx.printf("%s\n", shownValue(i));
                return Shell::ExitSuccess;
        }
        if (std::strcmp(verb, "set") == 0 && args.size() == 4) {
                const Error err = SysConf::set(key, args[3]);
                return err.isError() ? fail(ctx, key, err) : Shell::ExitSuccess;
        }
        if (std::strcmp(verb, "setdef") == 0 && args.size() == 3) {
                const Error err = SysConf::setDefault(key);
                return err.isError() ? fail(ctx, key, err) : Shell::ExitSuccess;
        }
        if (std::strcmp(verb, "revert") == 0 && args.size() == 3) {
                const Error err = SysConf::revert(key);
                return err.isError() ? fail(ctx, key, err) : Shell::ExitSuccess;
        }
        if (std::strcmp(verb, "save") == 0 && args.size() <= 3) {
                const Error err = key != nullptr ? SysConf::save(key) : SysConf::saveAll();
                return err.isError() ? fail(ctx, key != nullptr ? key : SysConf::StorePath, err) : Shell::ExitSuccess;
        }
        if (std::strcmp(verb, "list") == 0 && args.size() <= 3) {
                const Error err = SysConf::list(key != nullptr ? key : "", printLine, &ctx);
                return err.isError() ? fail(ctx, key != nullptr ? key : "list", err) : Shell::ExitSuccess;
        }
        if (std::strcmp(verb, "info") == 0 && args.size() == 3) return showInfo(ctx, key);
        return ctx.usage();
}

BMDS_SHELL_COMMAND(sysconf, SysConfCommand::run,
                   "get KEY | set KEY VALUE | setdef KEY | revert KEY | save [KEY] | list [PREFIX] | info KEY",
                   "Show and change the system configuration; changes last until reboot unless saved")

BMDS_NAMESPACE_END
