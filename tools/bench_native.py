"""A/B benchmark two builds of the DTC-01 core DLL.

    python tools\\bench_native.py build\\dtc01_x64.dll build\\pgo\\dtc01_x64.dll

Trials are *interleaved* rather than run in two blocks. Throughput on this
class of machine drifts with thermal and scheduler noise -- a single run of A
followed by a single run of B measures that drift as much as the code, and
back-to-back measurements of the same DLL have differed by 25% here. Reported
figures are the median and best of the interleaved trials.

It also checks the two builds produce bit-identical audio. An optimisation
that changes output is a bug, not a speedup, and timing alone will not say so.
"""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLE_RATE = 10000
PHRASE = (b"[:np][:ra 200] The DECtalk DTC-01 was introduced in 1984. "
          b"It used a Motorola 68000 and a signal processor to turn text into speech. ")


def load(dllPath, romDir):
	sys.path.insert(0, str(ROOT / "addon" / "synthDrivers" / "dectalkDtc01"))
	from emu.native import NativeMachine
	os.add_dll_directory(str(Path(dllPath).resolve().parent))
	# Pinned to v2.0 rather than left to $DTC01_ROM_VERSION: this benchmarks
	# the shipping default, and an env var set for unrelated reasons must not
	# silently change what is measured -- or fail outright, which is what
	# happens when it names a firmware the ROM dir does not hold.
	return NativeMachine(romDir, dll_path=str(dllPath), rom_version="v20")


def render(m, samples):
	m.feed_text(PHRASE + b"\r")
	return m.run_samples(samples)


def main() -> int:
	ap = argparse.ArgumentParser()
	ap.add_argument("baseline")
	ap.add_argument("candidate")
	ap.add_argument("--trials", type=int, default=7)
	ap.add_argument("--samples", type=int, default=120000, help="10k = 1s of audio")
	ap.add_argument("--allow-diff", action="store_true",
					help="time the builds even though their output differs. Only "
						 "for a change whose output SHOULD differ, and only once "
						 "you have established why -- see tools/test_scheduler_exact.py")
	args = ap.parse_args()

	romDir = os.path.expandvars(r"%APPDATA%\nvda\dectalkDtc01\roms")
	if not os.path.isdir(romDir):
		print(f"ERROR: no ROMs at {romDir}")
		return 1
	for p in (args.baseline, args.candidate):
		if not Path(p).is_file():
			print(f"ERROR: no such DLL: {p}")
			return 1

	a = load(args.baseline, romDir)
	b = load(args.candidate, romDir)
	print(f"  A = {a.dll_path}")
	print(f"  B = {b.dll_path}")
	for m in (a, b):
		m.run_samples(6000)          # boot past the power-on announcement

	# Correctness before speed: identical input must give identical output.
	sa, sb = render(a, args.samples), render(b, args.samples)
	if list(sa) != list(sb):
		diff = sum(1 for x, y in zip(sa, sb) if x != y)
		msg = (f"OUTPUT DIFFERS: {diff}/{min(len(sa), len(sb))} samples, "
			   f"lengths {len(sa)} vs {len(sb)}")
		if not args.allow_diff:
			print(f"\n  *** {msg} -- not a valid speedup")
			return 1
		print(f"  *** {msg}")
		print("      proceeding under --allow-diff; the timings below say nothing "
			  "about whether that difference is correct")
	else:
		print(f"  output identical over {len(sa)/SAMPLE_RATE:.1f}s of audio")

	def timed(m):
		t = time.perf_counter()
		s = render(m, args.samples)
		return (len(s) / SAMPLE_RATE) / (time.perf_counter() - t)

	timed(a), timed(b)               # warm up, discarded
	ra, rb = [], []
	for i in range(args.trials):
		# Alternate which runs first so neither consistently gets a cold cache.
		if i % 2 == 0:
			ra.append(timed(a)); rb.append(timed(b))
		else:
			rb.append(timed(b)); ra.append(timed(a))
		print(f"    trial {i+1}: A {ra[-1]:5.2f}x   B {rb[-1]:5.2f}x")

	ma, mb = statistics.median(ra), statistics.median(rb)
	print(f"\n  A  median {ma:5.2f}x   best {max(ra):5.2f}x")
	print(f"  B  median {mb:5.2f}x   best {max(rb):5.2f}x")
	delta = (mb / ma - 1) * 100
	print(f"  -> B is {delta:+.1f}% on the median")
	spread = (max(ra) - min(ra)) / ma * 100
	if abs(delta) < spread:
		print(f"     NOTE: run-to-run spread on A alone is {spread:.1f}%; a "
			  f"difference this size is not distinguishable from noise")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
