#!/usr/bin/env python3
import argparse
import math
import struct
import wave
from pathlib import Path


RATE = 11025


def build_song():
    # A short original chiptune-like melody: melody note, bass note, beats.
    melody = [
        (659.25, 2), (783.99, 2), (880.00, 4), (783.99, 2),
        (659.25, 2), (523.25, 4), (587.33, 2), (659.25, 2),
        (783.99, 4), (659.25, 2), (587.33, 2), (523.25, 4),
    ]
    beat = 0.18
    out = bytearray()
    phase_m = 0.0
    phase_b = 0.0
    for index, (freq, beats) in enumerate(melody):
        count = int(RATE * beat * beats)
        bass = [130.81, 146.83, 164.81, 196.00][(index // 3) % 4]
        for i in range(count):
            t = i / RATE
            attack = min(1.0, t / 0.018)
            release = min(1.0, (count - i) / (RATE * 0.045))
            env = attack * release
            phase_m += freq / RATE
            phase_b += bass / RATE
            lead = 1.0 if phase_m % 1.0 < 0.5 else -1.0
            low = math.sin(phase_b * math.tau)
            pulse = 1.0 if (i % max(1, RATE // 8)) < 45 else 0.0
            sample = 128 + int(env * (35 * lead + 18 * low) + 12 * pulse)
            out.append(max(0, min(255, sample)))
    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    path = Path(args.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = build_song()
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(1)
        wav.setframerate(RATE)
        wav.writeframes(samples)
    print(f"Generated {path} ({len(samples)} PCM bytes)")


if __name__ == "__main__":
    main()
