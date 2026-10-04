/**
 * @file      colorfilter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/colorfilter.h>

BMDS_NAMESPACE_BEGIN

void ColorFilter::csiDispatch(const VtParams &params, StringView intermediates, uint8_t final) {
        if (final != 'm' || !intermediates.isEmpty() || params.privateMarker != 0) return;
        Sgr::apply(params, _in);
        _sgr = true;
        return;
}

void ColorFilter::process(ByteSpan data, Sink sink, void *context) {
        size_t runStart = 0; // start of plain text being passed on as a run
        for (size_t i = 0; i < data.size(); ++i) {
                const uint8_t b = data[i];
                if (_parser.isGround() && b != 0x1B) {
                        _parser.feed(b); // keeps UTF-8 state; plain text passes as is
                        continue;
                }
                // A sequence starts or continues: flush the text before it.
                if (runStart < i && _heldLength == 0 && !_passThrough) sink(context, data.mid(runStart, i - runStart));
                runStart = i + 1;
                if (_passThrough) {
                        _parser.feed(b);
                        sink(context, data.mid(i, 1));
                        if (_parser.isGround()) _passThrough = false;
                        continue;
                }
                if (_heldLength == MaxHeld) {
                        // Too long to be an SGR worth rewriting: send it on as it is.
                        sink(context, ByteSpan(_held, _heldLength));
                        _heldLength = 0;
                        _passThrough = true;
                        _parser.feed(b);
                        sink(context, data.mid(i, 1));
                        if (_parser.isGround()) _passThrough = false;
                        continue;
                }
                _held[_heldLength++] = b;
                _sgr = false;
                _parser.feed(b);
                if (!_parser.isGround()) continue;
                if (_sgr) {
                        char         seq[Sgr::MaxSequence];
                        const size_t n = Sgr::transition(_out, _in, _depth, seq);
                        if (n > 0) sink(context, ByteSpan(reinterpret_cast<const uint8_t *>(seq), n));
                        _out = _in;
                } else {
                        sink(context, ByteSpan(_held, _heldLength));
                }
                _heldLength = 0;
        }
        if (runStart < data.size() && _heldLength == 0) sink(context, data.mid(runStart));
        return;
}

BMDS_NAMESPACE_END
