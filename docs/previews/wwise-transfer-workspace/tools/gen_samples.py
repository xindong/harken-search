"""Generate synthetic demo samples for the Wwise transfer workspace preview.

Preview-only material: these clips exist to show steps, splits and padding in
the interaction prototype. They are not library content and are not evidence
for real audio behaviour.
"""

from __future__ import annotations

import base64
import io
import json
import math
import os
import random
import wave

RATE = 22050
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.dirname(HERE)


def write_wav(samples):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for value in samples:
            clipped = max(-1.0, min(1.0, value))
            frames += int(clipped * 32767).to_bytes(2, "little", signed=True)
        w.writeframes(bytes(frames))
    return buf.getvalue()


def silence(seconds, floor, rng):
    return [rng.uniform(-floor, floor) for _ in range(int(seconds * RATE))]


def thump(seconds, amp, freq, rng):
    out = []
    count = int(seconds * RATE)
    for i in range(count):
        t = i / RATE
        env = math.exp(-t * 18.0)
        tone = math.sin(2 * math.pi * freq * t) * 0.6 + math.sin(2 * math.pi * freq * 2.4 * t) * 0.25
        noise = rng.uniform(-1.0, 1.0) * math.exp(-t * 40.0) * 0.5
        out.append((tone + noise) * env * amp)
    return out


def metal(seconds, amp, base, rng):
    out = []
    count = int(seconds * RATE)
    for i in range(count):
        t = i / RATE
        env = math.exp(-t * 7.5)
        tone = (
            math.sin(2 * math.pi * base * t) * 0.4
            + math.sin(2 * math.pi * base * 1.51 * t) * 0.25
            + math.sin(2 * math.pi * base * 2.33 * t) * 0.18
        )
        noise = rng.uniform(-1.0, 1.0) * math.exp(-t * 55.0) * 0.35
        out.append((tone + noise) * env * amp)
    return out


def main():
    rng = random.Random(20261119)
    files = {}

    footsteps = list(silence(0.30, 0.0012, rng))
    for index, gap in enumerate((0.55, 0.42, 0.61, 0.48)):
        footsteps.extend(thump(0.16, 0.55, 95.0 + index * 6, rng))
        footsteps.extend(silence(gap, 0.0012, rng))
    footsteps.extend(thump(0.18, 0.50, 118.0, rng))
    footsteps.extend(silence(0.25, 0.0012, rng))
    files["AC_Footsteps_Concrete_01.wav"] = write_wav(footsteps)

    doors = list(silence(0.20, 0.0010, rng))
    for gap in (0.80, 0.65):
        doors.extend(metal(0.45, 0.60, 180.0, rng))
        doors.extend(silence(gap, 0.0010, rng))
    doors.extend(metal(0.52, 0.55, 210.0, rng))
    doors.extend(silence(0.30, 0.0010, rng))
    files["METAL_Door_Impact_02.wav"] = write_wav(doors)

    ui = list(silence(0.10, 0.0006, rng))
    ui.extend(thump(0.07, 0.30, 620.0, rng))
    ui.extend(silence(0.15, 0.0006, rng))
    files["UI_Click_Soft_03.wav"] = write_wav(ui)

    quiet = list(silence(0.30, 0.0009, rng))
    for gap in (0.50, 0.70, 0.45):
        quiet.extend(thump(0.15, 0.085, 92.0, rng))
        quiet.extend(silence(gap, 0.0009, rng))
    quiet.extend(thump(0.16, 0.075, 104.0, rng))
    quiet.extend(silence(0.25, 0.0009, rng))
    files["AC_Footsteps_Quiet_Take_04.wav"] = write_wav(quiet)

    sample_dir = os.path.join(OUT_DIR, "samples")
    os.makedirs(sample_dir, exist_ok=True)
    for name, data in files.items():
        with open(os.path.join(sample_dir, name), "wb") as handle:
            handle.write(data)

    payload = {name: base64.b64encode(data).decode("ascii") for name, data in files.items()}
    with open(os.path.join(OUT_DIR, "samples.js"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("window.HARKEN_PREVIEW_SAMPLES = ")
        json.dump(payload, handle, ensure_ascii=False, separators=(",", ":"))
        handle.write(";\n")

    for name, data in files.items():
        print(name, len(data), "bytes")


if __name__ == "__main__":
    main()
