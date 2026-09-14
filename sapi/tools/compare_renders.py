"""compare_renders.py - proves that a change to the engine removed only silence.

Renders the same first utterance through two builds of DectalkDtc01SAPI.dll,
a baseline and a candidate, for every voice of both firmware versions, and
requires the candidate's audio to be an exact, contiguous slice of the
baseline's with no audible baseline sample outside that slice: the speech is
bit-identical, only the dead air around it may differ (DESIGN.md s23).

    python sapi/tools/compare_renders.py <baseline dir> <candidate dir> <rom dir>

Each build dir must hold DectalkDtc01SAPI.dll, sapi_probe.exe and
dtc01_x64.dll -- for instance a copy of sapi/build_x64/Release with
build/dtc01_x64.dll dropped in beside it. <rom dir> is one flat directory
holding both firmwares' chips (they are matched by content hash). Every
utterance runs in a fresh sapi_probe process, so both builds start from the
same freshly booted machine, and every text is short enough that neither
build splits it for the firmware's line limit.
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

VOICES = [(key, "v20") for key in
          ("paul", "betty", "harry", "frank", "dennis", "kit", "rita", "ursula", "wendy", "val")] + \
         [(key, "v18") for key in
          ("paul", "betty", "harry", "frank", "kit", "rita", "ursula", "val")]

TEXTS = [
    "Hello, this is a test of the emergency broadcast system.",
    "Desktop  list",
    "o",
]

AUDIBLE = 256  # the engine's own speech level (speech_shaper.hpp)


def render(build: Path, rom_dir: str, voice: str, firmware: str, text: str, out: Path) -> bytes:
    env = dict(os.environ, DTC01_ROM_DIR=rom_dir, DTC01_CORE_DLL=str(build / "dtc01_x64.dll"))
    proc = subprocess.run(
        [str(build / "sapi_probe.exe"), str(build / "DectalkDtc01SAPI.dll"), voice, firmware, str(out), text],
        env=env, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"sapi_probe failed ({build}, {voice}/{firmware}): {proc.stdout}{proc.stderr}")
    with wave.open(str(out), "rb") as w:
        return w.readframes(w.getnframes())


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    baseline, candidate, rom_dir = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    for build in (baseline, candidate):
        for name in ("DectalkDtc01SAPI.dll", "sapi_probe.exe", "dtc01_x64.dll"):
            if not (build / name).is_file():
                print(f"ERROR: {build / name} not found")
                return 2

    tmp = Path(tempfile.mkdtemp(prefix="dtc01_compare_"))
    failures = 0
    removed_before = removed_after = 0.0
    for voice, firmware in VOICES:
        for n, text in enumerate(TEXTS):
            base = render(baseline, rom_dir, voice, firmware, text, tmp / "baseline.wav")
            cand = render(candidate, rom_dir, voice, firmware, text, tmp / "candidate.wav")
            samples = struct.unpack("<%dh" % (len(base) // 2), base)

            at = base.find(cand) if cand else -1
            while at >= 0 and at % 2:
                at = base.find(cand, at + 1)
            label = f"{voice:6}/{firmware} text {n}"
            if at < 0:
                print(f"FAIL {label}: candidate audio is not a slice of the baseline")
                failures += 1
                continue

            start, end = at // 2, at // 2 + len(cand) // 2
            dropped = sum(1 for s in samples[:start] if abs(s) > AUDIBLE) + \
                      sum(1 for s in samples[end:] if abs(s) > AUDIBLE)
            removed_before += start / 10
            removed_after += (len(samples) - end) / 10
            status = "ok  " if dropped == 0 else "FAIL"
            failures += dropped != 0
            print(f"{status} {label}: {(end - start) / 10:8.1f} ms identical; "
                  f"{start / 10:6.1f} ms removed before, {(len(samples) - end) / 10:8.1f} ms after"
                  + ("" if dropped == 0 else f"; {dropped} audible samples missing"))

    cases = len(VOICES) * len(TEXTS)
    print(f"mean removed: {removed_before / cases:.0f} ms before the first sound, "
          f"{removed_after / cases:.0f} ms after the last")
    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
