#!/usr/bin/env python3
"""
Generates Kiosk's original sound effects and background music as 16-bit mono WAVs.
Everything is synthesized from scratch here, so it can be tweaked and regenerated:

    python3 tools/gen_audio.py      # writes romfs/sfx/*.wav
"""
import os
import wave

import numpy as np

SR = 22050
OUT = os.path.join(os.path.dirname(__file__), "..", "romfs", "sfx")
rng = np.random.default_rng(7)


def midi(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def t_of(seconds):
    return np.arange(int(seconds * SR)) / SR


def env(t, attack=0.004, tau=0.3):
    a = np.clip(t / attack, 0, 1)
    return a * np.exp(-t / tau)


def fade_tail(x, ms=8):
    n = min(len(x), int(SR * ms / 1000))
    if n > 0:
        x[-n:] *= np.linspace(1, 0, n)
    return x


# --- Instruments -----------------------------------------------------------

def bell(f, dur, tau=0.35, bright=1.0):
    t = t_of(dur)
    x = (np.sin(2 * np.pi * f * t)
         + 0.35 * bright * np.sin(2 * np.pi * f * 2.0 * t) * np.exp(-t / (tau * 0.5))
         + 0.18 * bright * np.sin(2 * np.pi * f * 3.01 * t) * np.exp(-t / (tau * 0.3)))
    return fade_tail(x * env(t, 0.002, tau))


def marimba(f, dur, tau=0.18):
    t = t_of(dur)
    x = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(2 * np.pi * f * 4.0 * t) * np.exp(-t / 0.03)
    return fade_tail(x * env(t, 0.001, tau))


def epiano(f, dur, tau=0.9):
    """Two-operator FM electric piano."""
    t = t_of(dur)
    index = 0.4 + 1.8 * np.exp(-t / 0.25)
    mod = np.sin(2 * np.pi * f * t)
    x = np.sin(2 * np.pi * f * t + index * mod)
    x += 0.15 * np.sin(2 * np.pi * f * 14 * t) * np.exp(-t / 0.02)  # tine click
    return fade_tail(x * env(t, 0.003, tau), 30)


def vibes(f, dur, tau=1.1):
    t = t_of(dur)
    trem = 1.0 - 0.22 * (0.5 + 0.5 * np.sin(2 * np.pi * 5.2 * t))
    x = np.sin(2 * np.pi * f * t) + 0.12 * np.sin(2 * np.pi * f * 4 * t) * np.exp(-t / 0.15)
    return fade_tail(x * trem * env(t, 0.002, tau), 40)


def bass(f, dur, tau=0.4):
    t = t_of(dur)
    x = np.sin(2 * np.pi * f * t) + 0.3 * np.sin(2 * np.pi * f * 2 * t) + 0.08 * np.sin(2 * np.pi * f * 3 * t)
    return fade_tail(x * env(t, 0.006, tau), 25)


def noise_hit(dur, tau, highpass=True):
    t = t_of(dur)
    n = rng.standard_normal(len(t))
    if highpass:
        n = np.diff(n, prepend=0)
    else:
        n = np.convolve(n, np.ones(6) / 6, mode="same")
    return fade_tail(n * env(t, 0.001, tau))


def kick(dur=0.25):
    t = t_of(dur)
    f = 50 + 70 * np.exp(-t / 0.04)
    phase = 2 * np.pi * np.cumsum(f) / SR
    return fade_tail(np.sin(phase) * env(t, 0.001, 0.12))


# --- Mixing helpers --------------------------------------------------------

def place(buf, sig, at_seconds, gain=1.0, wrap=False):
    start = int(at_seconds * SR)
    end = start + len(sig)
    if end <= len(buf):
        buf[start:end] += sig * gain
    elif wrap:
        k = len(buf) - start
        buf[start:] += sig[:k] * gain
        buf[: len(sig) - k] += sig[k:] * gain
    else:
        buf[start:] += sig[: len(buf) - start] * gain


def write(name, x, peak=0.85):
    x = np.asarray(x, dtype=np.float64)
    m = np.max(np.abs(x)) or 1.0
    x = x / m * peak
    data = (x * 32767).astype("<i2").tobytes()
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data)
    print(f"{name}.wav  {len(x) / SR:.2f}s")


def seq(notes, total, inst, **kw):
    """notes: list of (start_seconds, midi, dur, gain)."""
    buf = np.zeros(int(total * SR))
    for start, n, dur, g in notes:
        place(buf, inst(midi(n), dur, **kw), start, g)
    return buf


# --- Sound effects ---------------------------------------------------------

def make_sfx():
    # Cursor tick: tiny, soft, high.
    write("move", marimba(midi(96), 0.05, tau=0.012) * 1.0, peak=0.35)

    # Select: quick upward pop.
    write("select", seq([(0, 84, 0.12, 1.0), (0.045, 91, 0.16, 0.9)], 0.22, marimba, tau=0.06), peak=0.6)

    # Back: soft downward pair.
    write("back", seq([(0, 86, 0.12, 0.9), (0.05, 79, 0.16, 0.8)], 0.22, marimba, tau=0.05), peak=0.5)

    # Tab switch: woody blip.
    write("tab", marimba(midi(88), 0.1, tau=0.04), peak=0.5)

    # Page open: three-note rise.
    write("open", seq([(0, 79, 0.15, 0.7), (0.035, 83, 0.15, 0.8), (0.07, 86, 0.25, 1.0)], 0.32, bell, tau=0.09), peak=0.55)

    # Download start: bright sparkly arpeggio.
    notes = [(0.00, 84, 0.5, 0.8), (0.06, 88, 0.5, 0.8), (0.12, 91, 0.5, 0.85), (0.18, 96, 0.7, 1.0)]
    x = seq(notes, 0.95, bell, tau=0.22)
    for i in range(10):
        place(x, bell(midi(100 + rng.integers(0, 8)), 0.15, tau=0.04), 0.2 + i * 0.045, 0.12)
    write("get", x, peak=0.7)

    # Download complete: a short original fanfare.
    mel = [(0.00, 79, 0.2, 0.8), (0.11, 84, 0.2, 0.85), (0.22, 88, 0.2, 0.9), (0.33, 91, 0.9, 1.0)]
    x = seq(mel, 1.8, vibes, tau=0.6)
    chord = [(0.33, n, 1.4, 0.35) for n in (60, 64, 67, 72)]
    x += seq(chord, 1.8, epiano, tau=0.8)
    for i in range(16):
        place(x, bell(midi(98 + rng.integers(0, 10)), 0.12, tau=0.03), 0.38 + i * 0.05, 0.1 * (1 - i / 16))
    write("complete", x, peak=0.75)

    # Confetti pop.
    p = noise_hit(0.12, 0.03) * 0.6 + marimba(midi(72), 0.12, tau=0.03) * 0.5
    write("pop", p, peak=0.6)

    # Error: gentle low minor pair.
    write("error", seq([(0, 64, 0.2, 1.0), (0.12, 60, 0.3, 1.0)], 0.45, marimba, tau=0.12), peak=0.5)


# --- Background music ------------------------------------------------------

def make_bgm():
    bpm = 120
    beat = 60 / bpm
    bars = 16
    total = bars * 4 * beat
    swing = 0.62  # Offbeat eighth position within the beat.

    def at(bar, b):
        whole = int(b)
        frac = b - whole
        if abs(frac - 0.5) < 1e-6:
            frac = swing
        return (bar * 4 + whole + frac) * beat

    # Progression: roots and chord tones (MIDI).
    prog = [
        ("C", 48, [52, 55, 59, 62]), ("Am", 45, [55, 60, 64, 67]), ("Dm", 50, [53, 57, 60, 64]), ("G7", 43, [53, 57, 59, 64]),
        ("C", 48, [52, 55, 59, 62]), ("A7", 45, [55, 57, 61, 64]), ("Dm", 50, [53, 57, 60, 64]), ("G7", 43, [53, 57, 59, 64]),
        ("F", 41, [52, 57, 60, 64]), ("Em", 40, [55, 59, 62, 64]), ("Dm", 50, [53, 57, 60, 64]), ("G7", 43, [53, 57, 59, 65]),
        ("Em", 40, [55, 59, 62, 64]), ("A7", 45, [55, 57, 61, 64]), ("Dm/G", 50, [53, 57, 60, 65]), ("C", 48, [52, 55, 59, 64]),
    ]

    melody = [
        [(0.5, .5, 76), (1, .5, 79), (1.5, 1, 83), (2.5, .5, 81), (3, 1, 79)],
        [(0, 1.5, 76), (1.5, .5, 72), (2, .5, 74), (2.5, .5, 76), (3, 1, 79)],
        [(0, 1, 77), (1, .5, 81), (1.5, .5, 84), (2, 1.5, 81), (3.5, .5, 77)],
        [(0, 1, 79), (1, .5, 77), (1.5, .5, 74), (2, 2, 71)],
        [(0.5, .5, 76), (1, .5, 79), (1.5, 1, 84), (2.5, .5, 83), (3, 1, 79)],
        [(0, 1, 76), (1, .5, 73), (1.5, .5, 76), (2, 1, 79), (3, 1, 81)],
        [(0, 1.5, 77), (1.5, .5, 76), (2, .5, 74), (2.5, 1.5, 72)],
        [(0, .5, 71), (.5, .5, 74), (1, .5, 77), (1.5, .5, 79), (2, 1, 83), (3, 1, 81)],
        [(0, 1.5, 81), (1.5, .5, 79), (2, .5, 77), (2.5, .5, 76), (3, 1, 77)],
        [(0, 1, 79), (1, .5, 83), (1.5, .5, 86), (2, 2, 83)],
        [(0, 1, 81), (1, .5, 77), (1.5, .5, 74), (2, 1, 77), (3, 1, 76)],
        [(0, 2, 74), (2, .5, 77), (2.5, .5, 79), (3, 1, 83)],
        [(0, 1, 84), (1, 1, 83), (2, 1, 79), (3, 1, 76)],
        [(0, 1, 81), (1, .5, 79), (1.5, .5, 76), (2, 2, 73)],
        [(0, 1, 74), (1, 1, 77), (2, 1, 79), (3, 1, 77)],
        [(0, 2, 76), (2, 2, 72)],
    ]

    buf = np.zeros(int(total * SR))

    for bar, (name, root, chord) in enumerate(prog):
        nxt = prog[(bar + 1) % bars][1]

        # Electric piano comping: on 1 and the "and" of 2.
        for b, length, g in ((0, 1.4, 0.16), (1.5, 1.6, 0.13)):
            for n in chord:
                place(buf, epiano(midi(n), length * beat + 0.4), at(bar, b), g, wrap=True)

        # Walking bass.
        third = 3 if name in ("Am", "Dm", "Em") else 4
        line = [root, root + third, root + 7, nxt + (1 if nxt < root + 7 else -1)]
        for b, n in enumerate(line):
            place(buf, bass(midi(n), beat * 0.95), at(bar, b), 0.38, wrap=True)

        # Melody on vibes.
        for b, length, n in melody[bar]:
            place(buf, vibes(midi(n), length * beat + 0.5), at(bar, b), 0.22, wrap=True)

        # Light kit: soft kick on 1 & 3, brush on 2 & 4, swung hats.
        for b in (0, 2):
            place(buf, kick(), at(bar, b), 0.22, wrap=True)
        for b in (1, 3):
            place(buf, noise_hit(0.2, 0.07, highpass=False), at(bar, b), 0.07, wrap=True)
        for b in (0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5):
            place(buf, noise_hit(0.06, 0.018), at(bar, b), 0.035 if b % 1 else 0.05, wrap=True)

    write("bgm", buf, peak=0.7)


if __name__ == "__main__":
    make_sfx()
    make_bgm()
