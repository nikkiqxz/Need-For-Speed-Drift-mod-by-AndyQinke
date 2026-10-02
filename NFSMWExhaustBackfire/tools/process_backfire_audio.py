from __future__ import annotations

import math
import sys
import wave
from pathlib import Path

import numpy as np


OUTPUT_NAMES = [
    "g1_01.wav", "g1_02.wav", "g1_03.wav",
    "g2_01.wav", "g2_02.wav", "g2_03.wav",
    "g3_01.wav", "g3_02.wav", "g3_03.wav",
    "g4_01.wav", "g4_02.wav", "g4_03.wav",
    "g5_01.wav", "g5_02.wav", "g5_03.wav", "g5_04.wav",
]


def process(source: Path, destination: Path) -> None:
    with wave.open(str(source), "rb") as reader:
        if reader.getsampwidth() != 2 or reader.getnchannels() != 2:
            raise ValueError(f"{source.name}: expected stereo 16-bit PCM")
        sample_rate = reader.getframerate()
        frames = reader.readframes(reader.getnframes())

    stereo = np.frombuffer(frames, dtype="<i2").reshape(-1, 2).astype(np.float64)
    mono = stereo.mean(axis=1)
    mono -= mono.mean()

    fade_in = min(len(mono), round(sample_rate * 0.010))
    fade_out = min(len(mono), round(sample_rate * 0.120))
    if fade_in:
        mono[:fade_in] *= np.sin(np.linspace(0.0, math.pi / 2.0, fade_in))
    if fade_out:
        mono[-fade_out:] *= np.sin(np.linspace(math.pi / 2.0, 0.0, fade_out))

    peak = float(np.max(np.abs(mono)))
    target = 32767.0 * 10.0 ** (-14.0 / 20.0)
    if peak > 0.0:
        mono *= target / peak
    pcm = np.clip(np.rint(mono), -32768, 32767).astype("<i2")

    destination.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(destination), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(sample_rate)
        writer.writeframes(pcm.tobytes())


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: process_backfire_audio.py SOURCE_DIR OUTPUT_DIR")
    source_dir = Path(sys.argv[1])
    output_dir = Path(sys.argv[2])
    for index, output_name in enumerate(OUTPUT_NAMES, start=1):
        process(source_dir / f"{index}.wav", output_dir / output_name)


if __name__ == "__main__":
    main()
