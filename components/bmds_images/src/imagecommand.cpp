/**
 * @file      imagecommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `image` command (devplan/images.md §3g): what pictures are and how they would be decoded,
 * and pictures converted (decoded, scaled, written as PNG or JPEG), timed. Its output format is a
 * userspace interface: keep the lines and their fields, add new ones only at the end.
 */

#include <cstdlib>
#include <bmds/command.h>
#include <bmds/images.h>
#include <bmds/imagescaler.h>
#include <bmds/shell.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        const char *codecName(ImageCodec c) {
                switch (c) {
                        case ImageCodec::Png: return "png";
                        case ImageCodec::Jpeg: return "jpeg";
                        case ImageCodec::Bmp: return "bmp";
                        default: return "unknown";
                }
        }

        const char *samplingName(JpegHeader::Sampling s) {
                switch (s) {
                        case JpegHeader::Sampling::Gray: return "grey";
                        case JpegHeader::Sampling::Yuv444: return "yuv444";
                        case JpegHeader::Sampling::Yuv422: return "yuv422";
                        case JpegHeader::Sampling::Yuv420: return "yuv420";
                        default: return "other";
                }
        }

        const char *codingName(JpegHeader::Coding c) {
                switch (c) {
                        case JpegHeader::Coding::Baseline: return "baseline";
                        case JpegHeader::Coding::Extended: return "extended";
                        case JpegHeader::Coding::Progressive: return "progressive";
                        default: return "other";
                }
        }

        Error toFile(void *arg, ByteSpan data) {
                auto *file = static_cast<FileHandle *>(arg);
                while (!data.isEmpty()) {
                        auto [n, err] = file->write(data, WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::IoError;
                        data = ByteSpan(data.data() + n, data.size() - n);
                }
                return Error::Ok;
        }

        // "NAME: CODEC WxH [details] -> DECODER[ 1/N] WxH" a file.
        int info(CommandContext &ctx, Args args) {
                int status = Shell::ExitSuccess;
                for (size_t i = 2; i < args.size(); ++i) {
                        Images::FileBytes file;
                        Error             err = file.read(StringView(args[i]), ctx.vfs());
                        JpegHeader        h;
                        ImageInfo         in;
                        if (err.isOk()) {
                                auto [got, ierr] = Images::info(file.bytes(), &h);
                                in = got;
                                err = ierr;
                        }
                        if (err.isError()) {
                                ctx.eprintf("%s: %s\n", args[i], err.desc());
                                status = Shell::ExitFailure;
                                continue;
                        }
                        Images::Report r;
                        const Error    planned = Images::plan(file.bytes(), Images::Options{}, r);
                        ctx.printf("%s: %s %ux%u", args[i], codecName(in.codec), unsigned(in.width),
                                   unsigned(in.height));
                        if (in.codec == ImageCodec::Jpeg)
                                ctx.printf(" %s %s orientation %u", samplingName(h.sampling), codingName(h.coding),
                                           unsigned(h.orientation));
                        if ((in.flags & ImageInfo::HasAlpha) != 0) ctx.printf(" alpha");
                        if (in.codec == ImageCodec::Png && (in.flags & ImageInfo::Progressive) != 0)
                                ctx.printf(" interlaced");
                        if (planned.isError()) {
                                ctx.printf(" -> %s\n", planned.desc());
                                continue;
                        }
                        ctx.printf(" -> %s", Images::decoderName(r.plan.decoder));
                        if (r.plan.shift != 0) ctx.printf(" 1/%u", 1u << r.plan.shift);
                        ctx.printf(" %ux%u\n", unsigned(r.plan.width), unsigned(r.plan.height));
                }
                return status;
        }

        int convert(CommandContext &ctx, Args args) {
                uint32_t       boxW = 0, boxH = 0;
                Images::Encode how;
                size_t         i = 2;
                for (; i < args.size() && args[i][0] == '-'; ++i) {
                        const StringView o(args[i]);
                        if (o == "-s" && i + 2 < args.size()) {
                                boxW = uint32_t(std::strtoul(args[++i], nullptr, 10));
                                boxH = uint32_t(std::strtoul(args[++i], nullptr, 10));
                                if (boxW == 0 || boxH == 0) return ctx.usage();
                        } else if (o == "-q" && i + 1 < args.size()) {
                                const long q = std::strtol(args[++i], nullptr, 10);
                                if (q < 1 || q > 100) return ctx.usage();
                                how.quality = uint8_t(q);
                        } else if (o == "-l" && i + 1 < args.size()) {
                                const long l = std::strtol(args[++i], nullptr, 10);
                                if (l < 0 || l > 9) return ctx.usage();
                                how.level = int(l);
                        } else {
                                return ctx.usage();
                        }
                }
                if (args.size() != i + 2) return ctx.usage();
                const char *in = args[i], *out = args[i + 1];
                how.codec = Images::codecForName(StringView(out));
                if (how.codec != ImageCodec::Png && how.codec != ImageCodec::Jpeg) {
                        ctx.eprintf("image: %s: write .png, .jpg or .jpeg\n", out);
                        return Shell::ExitFailure;
                }
                const uint64_t    start = System::uptimeUs();
                Images::FileBytes file;
                Error             err = file.read(StringView(in), ctx.vfs());
                if (err.isError()) {
                        ctx.eprintf("image: %s: %s\n", in, err.desc());
                        return Shell::ExitFailure;
                }
                const uint64_t  read = System::uptimeUs();
                Images::Options o;
                o.orient = true;
                o.boxWidth = boxW;
                o.boxHeight = boxH;
                Images::Picture pic;
                Images::Report  r;
                err = Images::load(file.bytes(), o, pic, &r);
                if (err.isError()) {
                        ctx.eprintf("image: %s: %s\n", in, err.desc());
                        return Shell::ExitFailure;
                }
                const uint64_t  decoded = System::uptimeUs();
                Images::Picture scaled;
                ImageBuffer     final = pic.image();
                if (boxW != 0) {
                        const ImageScaler::Size s = ImageScaler::fit(final.width, final.height, boxW, boxH, false);
                        if (s.width != final.width || s.height != final.height) {
                                err = scaled.allocate(s.width, s.height, final.format, MemSpace::Psram);
                                if (err.isOk()) err = ImageScaler::scale(final, scaled.image(), MemSpace::Psram);
                                if (err.isError()) {
                                        ctx.eprintf("image: scaling: %s\n", err.desc());
                                        return Shell::ExitFailure;
                                }
                                final = scaled.image();
                        }
                }
                const uint64_t scaledAt = System::uptimeUs();
                auto [f, ferr] = Vfs::open(StringView(out), Vfs::Write | Vfs::Create | Vfs::Truncate, ctx.vfs());
                if (ferr.isError()) {
                        ctx.eprintf("image: %s: %s\n", out, ferr.desc());
                        return Shell::ExitFailure;
                }
                err = Images::encode(final, how, toFile, &*f);
                f.reset();
                if (err.isError()) {
                        ctx.eprintf("image: %s: %s\n", out, err.desc());
                        return Shell::ExitFailure;
                }
                const uint64_t done = System::uptimeUs();
                auto [st, serr] = Vfs::stat(StringView(out), ctx.vfs());
                const auto ms = [](uint64_t us) {
                        return static_cast<unsigned long>(us / 1000);
                };
                ctx.printf("image: %s, %ux%u, %lu bytes in %lu ms (read %lu, decode %lu %s", out, unsigned(final.width),
                           unsigned(final.height), static_cast<unsigned long>(serr.isOk() ? st.size : 0),
                           ms(done - start), ms(read - start), ms(decoded - read), Images::decoderName(r.plan.decoder));
                if (r.plan.shift != 0) ctx.printf(" 1/%u", 1u << r.plan.shift);
                if (r.zeroCopy) ctx.printf(" zero-copy");
                ctx.printf(", scale %lu, encode %lu)\n", ms(scaledAt - decoded), ms(done - scaledAt));
                return Shell::ExitSuccess;
        }

        int runImage(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() >= 3 && StringView(args[1]) == "info") return info(ctx, args);
                if (args.size() >= 4 && StringView(args[1]) == "convert") return convert(ctx, args);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(image, runImage, "info FILE... | convert [-s W H] [-q QUALITY] [-l LEVEL] IN OUT",
                   "Say what pictures are and how they decode, or convert one (OUT .png or .jpg; -s: to fit W x H)")

BMDS_NAMESPACE_END
