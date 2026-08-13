"""Dev-only helper: render the same speech through both firmware versions
so they can be compared by ear.

Measurements (peak/rms/clipping) established that v1.8 boots and produces
non-clipping audio with its own DSP pair -- see DESIGN.md section 21 -- but
"not clipping" is not the same as "sounds right", and nothing in the test
suite can judge that. This writes .wav files for a listening test.

Output goes to build/ (never packaged). Usage:

    python tools/compare_rom_versions.py [rom_dir] [out_dir]
"""

from __future__ import annotations

import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "addon" / "synthDrivers" / "dectalkDtc01"))

from emu import rom_loader  # noqa: E402
from emu.native import NativeMachine  # noqa: E402

SAMPLE_RATE = 10000

# Chosen for phonetic coverage rather than charm: a pangram, a sibilant
# torture test, question/exclamation intonation, number and currency
# expansion (where the two firmwares' text processing is most likely to
# differ), and a plosive run. Each stays under FIRMWARE_LINE_BYTES once the
# command prefix and terminator are added -- see DESIGN.md section 19.
SENTENCES = (
	"The quick brown fox jumps over the lazy dog.",
	"She sells sea shells by the sea shore.",
	"Is this thing working? Yes, it certainly is!",
	"The year was 1984, and the price was 25 dollars.",
	"Peter Piper picked a peck of pickled peppers.",
)

# Generous fixed window per sentence rather than polling is_idle: is_idle
# means "line accepted", not "finished speaking", and the firmware pauses
# mid-utterance (DESIGN.md sections 17 and 20). Trimming the tail afterwards
# is the reliable way to get a tight clip.
SECONDS_PER_SENTENCE = 9.0
# v1.8's power-on announcement is longer than v2.0's and ran past a 5s
# window; trimming means over-capturing costs nothing but time.
BOOT_SECONDS = 14.0
SILENCE_THRESHOLD = 200  # |sample| below this counts as silence for trimming
TAIL_PAD = 1500  # samples of silence to keep after the last real audio


def trim_tail(samples: list[int]) -> list[int]:
	"""Drop trailing silence only. Internal pauses are real speech timing."""
	last = -1
	for i in range(len(samples) - 1, -1, -1):
		if abs(samples[i]) > SILENCE_THRESHOLD:
			last = i
			break
	if last < 0:
		return []
	return samples[: min(len(samples), last + TAIL_PAD)]


def write_wav(path: Path, samples: list[int]) -> None:
	import array

	with wave.open(str(path), "wb") as w:
		w.setnchannels(1)
		w.setsampwidth(2)
		w.setframerate(SAMPLE_RATE)
		w.writeframes(array.array("h", samples).tobytes())


def describe(samples: list[int]) -> str:
	if not samples:
		return "silent"
	peak = max(max(samples), -min(samples))
	rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
	clipped = sum(1 for s in samples if s >= 32700 or s <= -32700)
	return (f"{len(samples) / SAMPLE_RATE:5.2f}s  peak={peak:6d}  "
	        f"rms={rms:6.0f}  clipped={clipped}")


def render(rom_dir: str, version: str) -> tuple[list[int], list[list[int]]]:
	"""Boot audio and one clip per sentence, for one firmware version."""
	m = NativeMachine(rom_dir, rom_version=version)
	try:
		boot = trim_tail(m.run_seconds(BOOT_SECONDS))
		m.read_host_tx()  # drain the '>' prompt so it can't confuse later reads
		clips = []
		for text in SENTENCES:
			line = f"[:np] {text}\r".encode("ascii")
			assert len(line) <= 120, f"line too long for the firmware: {len(line)}B"
			m.feed_text(line)
			clips.append(trim_tail(m.run_seconds(SECONDS_PER_SENTENCE)))
		return boot, clips
	finally:
		m.close()


def main() -> int:
	rom_dir = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "roms_extracted")
	out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "build" / "romcmp"
	out_dir.mkdir(parents=True, exist_ok=True)

	versions = rom_loader.available_versions(rom_dir)
	missing = [v for v in ("v20", "v18") if v not in versions]
	if missing:
		print(f"ROM set(s) not present in {rom_dir}: {', '.join(missing)}")
		return 1

	gap = [0] * int(0.5 * SAMPLE_RATE)
	rendered = {}
	for version in ("v20", "v18"):
		desc = rom_loader.ROM_SETS[version].description
		print(f"\n=== {desc}")
		boot, clips = render(rom_dir, version)
		rendered[version] = clips

		write_wav(out_dir / f"boot_{version}.wav", boot)
		print(f"  boot announcement   {describe(boot)}")
		joined = []
		for text, clip in zip(SENTENCES, clips):
			print(f"  {text[:34]:<36}{describe(clip)}")
			joined.extend(clip + gap)
		write_wav(out_dir / f"{version}_all.wav", joined)

	# The useful one for a listening test: same sentence back to back, v2.0
	# first, so the difference is audible without switching files.
	ab: list[int] = []
	for i in range(len(SENTENCES)):
		ab.extend(rendered["v20"][i] + gap)
		ab.extend(rendered["v18"][i] + gap + gap)
	write_wav(out_dir / "ab_by_sentence.wav", ab)

	print(f"\nwrote to {out_dir}:")
	for p in sorted(out_dir.glob("*.wav")):
		print(f"  {p.name:<24}{p.stat().st_size / 1024:6.0f} KB")
	print("\nab_by_sentence.wav plays each sentence v2.0 then v1.8.")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
