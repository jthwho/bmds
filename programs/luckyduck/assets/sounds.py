# @file      sounds.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Lucky Duck's sounds (devplan/luckyduck.md §3), synthesized: Solitaire's card sounds, quacks (a
# buzzing source through a duck's two formants, its pitch falling), a click, a fanfare and a sad
# trombone.

import math
import os
import random
import struct
import wave

RATE = 22050


def write_wav(path, samples):
    peak = max(1e-9, max(abs(v) for v in samples))
    gain = min(1.0, 0.95 / peak)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v * gain * 32767)) for v in samples))


def snap(seconds, level, decay, bright, seed, rise=0.0005):
    """A card's noise: filtered noise with a sharp attack, @p bright the filter (0-1, higher brighter)."""
    rnd = random.Random(seed)
    out = []
    low = 0.0
    band = 0.0
    for i in range(int(RATE * seconds)):
        t = i / RATE
        n = rnd.random() * 2 - 1
        low += bright * (n - low)
        band += 0.5 * (low - band)
        env = min(1.0, t / rise) * math.exp(-t * decay)
        out.append((low - 0.6 * band) * env * level)
    return out


def mix_at(out, samples, at):
    start = int(at * RATE)
    if len(out) < start + len(samples):
        out.extend([0.0] * (start + len(samples) - len(out)))
    for i, v in enumerate(samples):
        out[start + i] += v


def tone(seconds, freq, level, decay):
    out = []
    for i in range(int(RATE * seconds)):
        t = i / RATE
        v = math.sin(2 * math.pi * freq * t) + 0.3 * math.sin(4 * math.pi * freq * t)
        out.append(v * level * math.exp(-t * decay) * min(1.0, t / 0.003))
    return out


def hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


class Bandpass:
    """A resonant band-pass (RBJ's biquad)."""

    def __init__(self, freq, q):
        w0 = 2 * math.pi * freq / RATE
        alpha = math.sin(w0) / (2 * q)
        a0 = 1 + alpha
        self.b = (alpha / a0, 0.0, -alpha / a0)
        self.a = (-2 * math.cos(w0) / a0, (1 - alpha) / a0)
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def __call__(self, x):
        y = self.b[0] * x + self.b[2] * self.x2 - self.a[0] * self.y1 - self.a[1] * self.y2
        self.x2, self.x1 = self.x1, x
        self.y2, self.y1 = self.y1, y
        return y


def quack(seconds, f0, f1, seed):
    """A quack: a buzz (a sawtooth with a rasp) falling from @p f0 to @p f1, through formants."""
    rnd = random.Random(seed)
    formants = [(Bandpass(1050, 5), 1.0), (Bandpass(2400, 6), 0.55), (Bandpass(3600, 8), 0.2)]
    out = []
    phase = 0.0
    for i in range(int(RATE * seconds)):
        t = i / RATE
        u = t / seconds
        f = f0 + (f1 - f0) * u ** 0.7
        phase = (phase + f / RATE) % 1.0
        src = 2 * phase - 1 + 0.25 * (rnd.random() * 2 - 1)
        src *= 1.0 - 0.35 * (0.5 + 0.5 * math.sin(2 * math.pi * 55 * t))  # the rasp
        v = sum(bp(src) * g for bp, g in formants)
        env = min(1.0, t / 0.012) * (1.0 if u < 0.6 else max(0.0, (1.0 - u) / 0.4))
        out.append(v * env)
    return out


def trombone(notes, length):
    """A muted brass slide through @p notes (MIDI), each @p length seconds, sliding down."""
    out = []
    phase = 0.0
    low = 0.0
    for k, n in enumerate(notes):
        last = k == len(notes) - 1
        dur = length * (2.2 if last else 1.0)
        for i in range(int(RATE * dur)):
            t = i / RATE
            f = hz(n) * (1.0 - (0.06 * t / dur if last else 0.0))
            if last:
                f *= 1.0 + 0.012 * math.sin(2 * math.pi * 6 * t)
            phase = (phase + f / RATE) % 1.0
            low += 0.18 * ((2 * phase - 1) - low)
            env = min(1.0, t / 0.03) * min(1.0, (dur - t) / 0.06)
            out.append(low * env * 0.8)
    return out


def write_all(out):
    write_wav(os.path.join(out, "place.wav"), snap(0.06, 0.9, 70, 0.55, 1))
    write_wav(os.path.join(out, "flip.wav"), snap(0.07, 0.6, 45, 0.85, 2, rise=0.01))
    deal = []
    rnd = random.Random(3)
    for k in range(18):  # a riffle
        mix_at(deal, snap(0.03, 0.5 + 0.3 * rnd.random(), 120, 0.7, 10 + k), 0.03 * k + 0.008 * rnd.random())
    write_wav(os.path.join(out, "deal.wav"), deal)
    win = []
    for k, n in enumerate((72, 76, 79, 84, 79, 84, 88)):
        mix_at(win, tone(0.6, hz(n), 0.22, 5), 0.11 * k)
    mix_at(win, tone(1.2, hz(60), 0.2, 2.5), 0.66)
    mix_at(win, tone(1.2, hz(67), 0.15, 2.5), 0.66)
    write_wav(os.path.join(out, "win.wav"), win)
    write_wav(os.path.join(out, "quack.wav"), quack(0.24, 330, 230, 4))
    write_wav(os.path.join(out, "quack2.wav"), quack(0.18, 420, 300, 5))
    two = []
    mix_at(two, quack(0.15, 380, 300, 6), 0.0)
    mix_at(two, quack(0.2, 360, 250, 7), 0.2)
    write_wav(os.path.join(out, "quacks.wav"), two)
    write_wav(os.path.join(out, "click.wav"), snap(0.025, 0.7, 180, 0.9, 8))
    write_wav(os.path.join(out, "sad.wav"), trombone((55, 54, 53, 52), 0.32))
