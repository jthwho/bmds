#!/usr/bin/env python3
# make-midi-seeds.py DIR: Standard MIDI Files for the synth's tests and fuzzing (devplan/audio.md §22):
# scale.mid, format 1 at 480 ticks a beat and 120 bpm: a C major scale of quarter notes on piano
# (0.5 s each, 4 s), a chord, drums on channel 10, a program change, a pitch bend, a tempo change
# to 60 bpm, a running status, and a SysEx (GM System On).
import os, struct, sys

def vlq(n):
    out = [n & 0x7f]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7f) | 0x80)
        n >>= 7
    return bytes(out)

def track(events):
    data = b''.join(vlq(dt) + ev for dt, ev in events) + vlq(0) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(data)) + data

out = sys.argv[1] if len(sys.argv) > 1 else sys.exit('usage: make-midi-seeds.py DIR')
os.makedirs(out, exist_ok=True)
q = 480
conductor = [(0, b'\xff\x51\x03' + (500000).to_bytes(3, 'big')),  # 120 bpm
             (0, b'\xf0\x05\x7e\x7f\x09\x01\xf7'),                 # GM System On
             (10 * q, b'\xff\x51\x03' + (1000000).to_bytes(3, 'big'))]  # 60 bpm after 10 beats
piano = [(0, b'\xc0\x00')]
for i, key in enumerate([60, 62, 64, 65, 67, 69, 71, 72]):
    piano += [(0, bytes([0x90, key, 100])), (q, bytes([0x80, key, 0]))]
piano += [(0, b'\x90\x3c\x50'), (0, b'\x40\x50'), (0, b'\x43\x50'),  # a chord, with running status
          (q, b'\xe0\x00\x50'), (q, b'\x80\x3c\x00'), (0, b'\x80\x40\x00'), (0, b'\x80\x43\x00')]
strings = [(0, b'\xc1\x30'), (8 * q, b'\x91\x30\x60'), (2 * q, b'\x81\x30\x00')]
drums = [(0, b'\x99\x24\x70'), (q, b'\x89\x24\x00'), (0, b'\x99\x26\x70'), (q, b'\x89\x26\x00'),
         (0, b'\x99\x2a\x50'), (q // 2, b'\x89\x2a\x00')]
smf = b'MThd' + struct.pack('>IHHH', 6, 1, 4, q) + track(conductor) + track(piano) + track(strings) + track(drums)
open(os.path.join(out, 'scale.mid'), 'wb').write(smf)
