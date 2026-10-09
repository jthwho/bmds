#!/usr/bin/env python3
# make-font-seeds.py DIR: the synth's fuzzing seeds (devplan/audio.md §22), small SoundFonts made
# here: one preset on a looped sine, and one whose zones use most generators (key and velocity
# ranges, envelopes, LFOs, the filter, loop modes, tuning, pan) over two samples, with a drum bank.
import math, os, struct, sys

def chunk(cid, data):
    return cid + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')

def lst(kind, data):
    return chunk(b'LIST', kind + data)

def name(n):
    return n.encode()[:19].ljust(20, b'\0')

def font(presets, samples):
    """presets: [(name, bank, program, [preset zone gens], [instrument zones: [gens]])]"""
    smpl, shdr, at = b'', b'', 0
    for (sname, data, rate, key, loop) in samples:
        smpl += struct.pack('<%dh' % len(data), *data) + b'\0' * 92
        start, end = at, at + len(data)
        shdr += name(sname) + struct.pack('<IIIIIBbHH', start, end, start + loop[0], start + loop[1], rate, key, 0, 0, 1)
        at = end + 46
    shdr += name('EOS') + struct.pack('<IIIIIBbHH', 0, 0, 0, 0, 0, 0, 0, 0, 0)
    phdr = pbag = pgen = inst = ibag = igen = b''
    pb = pg = ib = ig = 0
    for i, (pname, bank, program, pgens, zones) in enumerate(presets):
        phdr += name(pname) + struct.pack('<HHHIII', program, bank, pb, 0, 0, 0)
        pbag += struct.pack('<HH', pg, 0); pb += 1
        for g in pgens + [(41, i)]:
            pgen += struct.pack('<HH', *g); pg += 1
        inst += name(pname) + struct.pack('<H', ib)
        for gens in zones:
            ibag += struct.pack('<HH', ig, 0); ib += 1
            for g in gens:
                igen += struct.pack('<Hh' if g[1] < 0 else '<HH', *g); ig += 1
    phdr += name('EOP') + struct.pack('<HHHIII', 0, 0, pb, 0, 0, 0)
    pbag += struct.pack('<HH', pg, 0); pgen += struct.pack('<HH', 0, 0)
    inst += name('EOI') + struct.pack('<H', ib)
    ibag += struct.pack('<HH', ig, 0); igen += struct.pack('<HH', 0, 0)
    pdta = (chunk(b'phdr', phdr) + chunk(b'pbag', pbag) + chunk(b'pmod', b'\0' * 10) + chunk(b'pgen', pgen) +
            chunk(b'inst', inst) + chunk(b'ibag', ibag) + chunk(b'imod', b'\0' * 10) + chunk(b'igen', igen) +
            chunk(b'shdr', shdr))
    body = b'sfbk' + lst(b'INFO', chunk(b'ifil', struct.pack('<HH', 2, 1))) + lst(b'sdta', chunk(b'smpl', smpl)) + lst(b'pdta', pdta)
    return chunk(b'RIFF', body)

def sine(n, period, amp=30000):
    return [int(math.sin(2 * math.pi * i / period) * amp) for i in range(n)]

def noise(n):
    x, out = 12345, []
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out.append((x >> 15) - 32768)
    return out

out = sys.argv[1] if len(sys.argv) > 1 else sys.exit('usage: make-font-seeds.py DIR')
os.makedirs(out, exist_ok=True)
sine_s = ('Sine', sine(2000, 100), 44000, 69, (1000, 1900))
noise_s = ('Noise', noise(1500), 22050, 60, (0, 0))
open(os.path.join(out, 'sine.sf2'), 'wb').write(font([('Sine', 0, 0, [], [[(54, 1), (53, 0)]])], [sine_s]))
rich = [
    ('Lead', 0, 0, [(48, 20)], [
        [(43, 0x3c00), (44, 0x7f40), (8, 2000), (9, 300), (34, -2000), (36, 1000), (38, 2400),
         (21, -800), (22, 100), (24, 50), (25, 1000), (26, 1200), (27, 900), (28, 1000), (30, 2000),
         (5, 50), (7, 200), (10, 100), (11, 20), (13, 30), (17, 250), (51, -3), (52, 25), (54, 3), (53, 0)],
        [(43, 0x7f3d), (6, 30), (15, 600), (17, -500), (56, 50), (58, 60), (54, 1), (53, 0)]]),
    ('Drums', 128, 0, [], [[(43, 0x5123), (57, 1), (37, 500), (54, 0), (53, 1)]]),
]
open(os.path.join(out, 'rich.sf2'), 'wb').write(font(rich, [sine_s, noise_s]))
