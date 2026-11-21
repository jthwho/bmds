/**
 * @file      iopin.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * One pad as people are shown it (devplan/io.md §3.10, §3.11).
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <bmds/iopin.h>

BMDS_NAMESPACE_BEGIN

namespace {

        void copyText(char *out, size_t size, const char *text) {
                if (size == 0) return;
                const size_t n = std::strlen(text);
                const size_t k = n < size - 1 ? n : size - 1;
                std::memcpy(out, text, k);
                out[k] = '\0';
                return;
        }

        // A line built a piece at a time, cut at its room.
        class Line {
                public:
                        explicit Line(char (&out)[IoPinDetail::TextBytes]) : _out(out) { _out[0] = '\0'; }

                        [[gnu::format(printf, 2, 3)]] void add(const char *format, ...) {
                                if (_length + 1 >= sizeof(_out)) return;
                                va_list args;
                                va_start(args, format);
                                const int n = std::vsnprintf(_out + _length, sizeof(_out) - _length, format, args);
                                va_end(args);
                                if (n > 0) _length += static_cast<size_t>(n);
                                if (_length >= sizeof(_out)) _length = sizeof(_out) - 1;
                                return;
                        }

                        bool isEmpty() const { return _length == 0; }

                private:
                        char (&_out)[IoPinDetail::TextBytes];
                        size_t _length = 0;
        };

} // namespace

char ioLevelChar(const IoPadState &state) {
        if (state.lpIo) return '?';
        if (state.inputEnabled) return state.level ? '1' : '0';
        if (state.outputEnabled) return state.outputLevel ? '1' : '0';
        return '-';
}

size_t ioPadInputs(Span<const int16_t> sources, unsigned pad, Span<uint16_t> out) {
        size_t n = 0;
        for (size_t i = 0; i < sources.size() && n < out.size(); ++i) {
                if (sources[i] == static_cast<int16_t>(pad)) out[n++] = static_cast<uint16_t>(i);
        }
        return n;
}

void ioPinRow(const IoChip &chip, unsigned pad, const IoPadState &state, Span<const uint16_t> inputs,
              const IoHolder &holder, const IoPinBoard &board, IoPinRow &out) {
        out = IoPinRow();
        out.pad = static_cast<uint8_t>(pad);
        out.resourceClass = holder.resourceClass;
        out.level = ioLevelChar(state);
        (void)chip.resourceName(static_cast<IoResource>(pad), out.name, sizeof(out.name));
        copyText(out.owner, sizeof(out.owner), holder.owner);
        copyText(out.function, sizeof(out.function),
                 holder.isHeld() && holder.role[0] != '\0' ? holder.role : board.label);
        (void)ioRouteText(chip, pad, state, inputs, out.route, sizeof(out.route));
        out.direction = ioDirectionText(chip, pad, state);
        out.pull = ioPullText(state);
        copyText(out.where, sizeof(out.where), board.where);
        out.state = state;
        out.strapping = pad < chip.padCount() && chip.pads[pad].is(IoPadInfo::Strapping);
        out.forced = holder.isForced();
        return;
}

void ioPinDetail(const IoChip &chip, unsigned pad, const IoPadState &state, Span<const uint16_t> inputs,
                 const IoHolder &holder, const IoPinBoard &board, IoPinDetail &out) {
        out.pad = static_cast<uint8_t>(pad);
        (void)chip.resourceName(static_cast<IoResource>(pad), out.name, sizeof(out.name));
        const auto line = [&out](IoPinField f) -> char(&)[IoPinDetail::TextBytes] {
                return out.text[static_cast<size_t>(f)];
        };
        const IoPadInfo *info = pad < chip.padCount() ? &chip.pads[pad] : nullptr;

        Line b(line(IoPinField::Board));
        b.add("%s", board.label[0] != '\0' ? board.label : "-");
        if (board.where[0] != '\0') b.add(" (%s)", board.where);

        Line(line(IoPinField::Note)).add("%s", board.note);
        Line(line(IoPinField::Class)).add("%s", ioClassName(holder.resourceClass));

        Line o(line(IoPinField::Owner));
        if (!holder.isHeld())
                o.add("-");
        else if (holder.role[0] != '\0')
                o.add("%s as %s", holder.owner, holder.role);
        else
                o.add("%s", holder.owner);

        Line f(line(IoPinField::Forced));
        if (holder.isForced()) {
                f.add("from %s", holder.lender);
                if (holder.lenderRole[0] != '\0') f.add(" (%s)", holder.lenderRole);
        }

        Line c(line(IoPinField::Chip));
        if (info != nullptr) {
                if (info->is(IoPadInfo::Strapping)) c.add("strapping");
                if (info->lpIo >= 0) c.add("%sLP IO %d", c.isEmpty() ? "" : ", ", info->lpIo);
                if (info->analog[0] != '\0') c.add("%s%s", c.isEmpty() ? "" : ", ", info->analog);
                if (info->note[0] != '\0') c.add("%s%s", c.isEmpty() ? "" : "; ", info->note);
        }
        if (c.isEmpty()) c.add("-");

        Line fs(line(IoPinField::Functions));
        for (const IoMuxFunction &fn : chip.padFunctions(pad))
                fs.add("%s%s%u %s", fs.isEmpty() ? "" : " ", fn.index == state.function ? "*" : "",
                       static_cast<unsigned>(fn.index), fn.name);
        if (fs.isEmpty()) fs.add("-");

        (void)ioRouteText(chip, pad, state, inputs, line(IoPinField::Route), IoPinDetail::TextBytes);
        Line(line(IoPinField::Direction)).add("%s", ioDirectionText(chip, pad, state));

        const char *signal = chip.outputName(state.outputSignal);
        Line(line(IoPinField::Output))
                .add("signal %u (%s), enable %u, by peripheral %u, latch %u, open drain %u",
                     static_cast<unsigned>(state.outputSignal), signal != nullptr ? signal : "?",
                     state.outputEnabled ? 1u : 0u, state.outputByPeripheral ? 1u : 0u, state.outputLevel ? 1u : 0u,
                     state.openDrain ? 1u : 0u);

        Line in(line(IoPinField::Input));
        in.add("enable %u, level %u, signals", state.inputEnabled ? 1u : 0u, state.level ? 1u : 0u);
        if (inputs.isEmpty()) in.add(" -");
        for (uint16_t s : inputs) {
                const char *name = chip.inputName(s);
                in.add(" %s", name != nullptr ? name : "?");
        }

        Line(line(IoPinField::Pull)).add("%s", ioPullText(state));
        Line(line(IoPinField::Drive)).add("%u", static_cast<unsigned>(state.drive));
        Line(line(IoPinField::LpIo)).add("%s", state.lpIo ? "yes" : "no");
        return;
}

const char *ioPinFieldName(IoPinField field) {
        switch (field) {
                case IoPinField::Board: return "Board";
                case IoPinField::Note: return "Note";
                case IoPinField::Class: return "Class";
                case IoPinField::Owner: return "Owner";
                case IoPinField::Forced: return "Forced";
                case IoPinField::Chip: return "Chip";
                case IoPinField::Functions: return "Functions";
                case IoPinField::Route: return "Route";
                case IoPinField::Direction: return "Direction";
                case IoPinField::Output: return "Output";
                case IoPinField::Input: return "Input";
                case IoPinField::Pull: return "Pull";
                case IoPinField::Drive: return "Drive";
                case IoPinField::LpIo: return "LP IO";
        }
        return "?";
}

BMDS_NAMESPACE_END
