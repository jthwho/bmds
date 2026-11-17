/**
 * @file      tlscommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `tls cert`: the board's own TLS certificate (devplan/network.md §37a S6), what TLS servers on
 * the board show their clients: its subject, dates and fingerprint (what a client asked to trust
 * it compares), as PEM, or replaced by a new one.
 */

#include <cstring>
#include <bmds/command.h>
#include <bmds/datetime.h>
#include <bmds/shell.h>
#include <bmds/tlsserverkeys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        void printDate(CommandContext &ctx, const char *label, int64_t seconds) {
                const DateTime t = DateTime::fromSeconds(seconds);
                ctx.printf("%-12s%04d-%02u-%02u %02u:%02u UTC\n", label, int(t.year), unsigned(t.month),
                           unsigned(t.day), unsigned(t.hour), unsigned(t.minute));
                return;
        }

        int showCertificate(CommandContext &ctx) {
                TlsServerKeys::Description d;
                String                     why(MemSpace::Psram);
                const Error                err = TlsServerKeys::describe(d, why);
                if (err.isError()) {
                        ctx.eprintf("tls: %s\n", why.isEmpty() ? err.desc() : why.cStr());
                        return Shell::ExitFailure;
                }
                ctx.printf("%-12s%s\n", "Subject", d.subject);
                printDate(ctx, "Valid from", d.notBefore);
                printDate(ctx, "Valid to", d.notAfter);
                ctx.printf("%-12s", "SHA-256");
                for (size_t i = 0; i < sizeof(d.sha256); ++i)
                        ctx.printf("%02X%s", unsigned(d.sha256[i]), i + 1 < sizeof(d.sha256) ? ":" : "\n");
                ctx.printf("%-12s%s\n", "Folder", d.folder);
                return Shell::ExitSuccess;
        }

        int runTls(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() < 2 || std::strcmp(args[1], "cert") != 0 || args.size() > 3) return ctx.usage();
                if (args.size() == 2) return showCertificate(ctx);
                String why(MemSpace::Psram);
                if (std::strcmp(args[2], "--pem") == 0) {
                        String      cert(MemSpace::Psram), key(MemSpace::Psram);
                        const Error err = TlsServerKeys::pem(cert, key, why);
                        if (!key.isEmpty()) std::memset(const_cast<char *>(key.cStr()), 0, key.size());
                        if (err.isError()) {
                                ctx.eprintf("tls: %s\n", why.isEmpty() ? err.desc() : why.cStr());
                                return Shell::ExitFailure;
                        }
                        ctx.printf("%s", cert.cStr());
                        return Shell::ExitSuccess;
                }
                if (std::strcmp(args[2], "--new") == 0) {
                        const Error err = TlsServerKeys::renew(why);
                        if (err.isError()) {
                                ctx.eprintf("tls: %s\n", why.isEmpty() ? err.desc() : why.cStr());
                                return Shell::ExitFailure;
                        }
                        ctx.printf("A new certificate (clients that trusted the old one will ask again):\n");
                        return showCertificate(ctx);
                }
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(tls, runTls, "cert [--pem | --new]",
                   "Show the board's own TLS certificate (what TLS servers on it present), print it as PEM, or "
                   "replace it with a new one")

BMDS_NAMESPACE_END
