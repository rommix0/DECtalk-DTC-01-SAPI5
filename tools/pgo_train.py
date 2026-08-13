"""Exercise the instrumented DTC-01 DLL to produce PGO profile data.

Run between `build_pgo.bat instrument` and `build_pgo.bat optimize`.

The workload deliberately mirrors how NVDA actually drives the synth, because
a profile only helps where it matches reality. Real use is mostly *short*
utterances -- key and word echo, control labels -- punctuated by occasional
long say-all passages, with voice and rate changes in between. Training only
on one long passage would optimise for the rarer case and leave the
latency-sensitive path cold.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PGODIR = ROOT / "build" / "pgo"


def main() -> int:
	dll = PGODIR / "dtc01_x64.dll"
	if not dll.is_file():
		print(f"ERROR: {dll} missing -- run tools\\build_pgo.bat instrument")
		return 1
	# The instrumented DLL imports pgort140.dll, which sits beside it rather
	# than on PATH. Dependent-DLL resolution does not search the loaded DLL's
	# own directory, so add it explicitly.
	os.add_dll_directory(str(PGODIR))

	sys.path.insert(0, str(ROOT / "addon" / "synthDrivers" / "dectalkDtc01"))
	from emu.native import NativeMachine

	romDir = os.path.expandvars(r"%APPDATA%\nvda\dectalkDtc01\roms")
	if not os.path.isdir(romDir):
		print(f"ERROR: no ROMs at {romDir}")
		return 1

	# Must be explicit: NativeMachine's own search finds the ordinary build in
	# build\, so omitting dll_path silently trains against the wrong binary and
	# produces no profile at all.
	t0 = time.perf_counter()
	# Pinned to v2.0, not left to $DTC01_ROM_VERSION: the profile has to
	# describe the firmware releases actually run. An env var set for
	# unrelated reasons would otherwise train against the wrong ROM, or fail
	# here when the config dir holds no set for the version it names.
	m = NativeMachine(romDir, dll_path=str(dll), rom_version="v20")
	assert Path(m.dll_path).resolve() == dll.resolve(), m.dll_path
	print(f"training against {m.dll_path} (instrumented -- expect it to be slow)")
	m.run_samples(6000)                       # boot + power-on announcement
	audio = 0

	# 1. Short utterances: the dominant real-world case (key/word echo).
	shorts = [b"a", b"the", b"button", b"is", b"test", b"Edit", b"checked",
			  b"menu bar", b"list", b"5 of 12", b"Documents", b"comma"]
	for _ in range(3):
		for w in shorts:
			m.feed_text(b"[:np] " + w + b",\r")
			audio += len(m.run_samples(9000))

	# 2. Voice switching, which re-sends the full parameter prefix.
	for voice in (b"np", b"nh", b"nf", b"nd", b"nb", b"nu", b"nr", b"nv"):
		m.feed_text(b"[:" + voice + b"] Voice check one two three.\r")
		audio += len(m.run_samples(28000))

	# 3. Rate changes across the firmware's range.
	for rate in (b"120", b"180", b"250", b"350"):
		m.feed_text(b"[:np][:ra " + rate + b"] Rate " + rate + b" words per minute.\r")
		audio += len(m.run_samples(30000))

	# 4. Design Voice parameter changes -- the sliders.
	m.feed_text(b"[:np][:dv hs 110 br 40 ri 70 sm 30] Parameter sweep active.\r")
	audio += len(m.run_samples(30000))

	# 5. A long passage: the say-all path.
	passage = (b"The DECtalk DTC-01 was introduced in 1984. "
			   b"It used a Motorola 68000 and a Texas Instruments signal processor "
			   b"to turn written text into speech, and its voice became familiar to "
			   b"a generation of screen reader users. ")
	m.feed_text(b"[:np][:ra 200] " + passage * 3 + b"\r")
	audio += len(m.run_samples(240000))

	el = time.perf_counter() - t0
	print(f"  generated {audio/10000:.1f}s of audio in {el:.1f}s wall")
	print("  profile data (.pgc) is written as the DLL unloads at exit")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
