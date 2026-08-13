"""Dev-only helper: assemble linear ROM images from a local dump directory
for testing the emulator cores. Never run as part of addon packaging --
output goes to build/, which is never included in the .nvda-addon zip.

Usage: python tools/build_rom_images.py <rom_dir> [out_dir]

Honours $DTC01_ROM_VERSION (v20 default, v18 experimental). Non-default
versions get suffixed filenames so they can't quietly displace the v2.0
images that native/selftest.c defaults to.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "addon" / "synthDrivers" / "dectalkDtc01"))

from emu import rom_loader  # noqa: E402


def main() -> int:
	if len(sys.argv) < 2:
		print(__doc__)
		return 1
	rom_dir = sys.argv[1]
	out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "build"
	out_dir.mkdir(parents=True, exist_ok=True)

	try:
		version = rom_loader.resolve_version()
		main_image = rom_loader.build_main_cpu_image(rom_dir, version)
		dsp_image = rom_loader.build_dsp_image(rom_dir, version)
	except ValueError as e:
		print(e)
		return 1
	except rom_loader.RomValidationError as e:
		print("ROM validation FAILED:")
		print(e)
		return 1

	print(f"firmware: {rom_loader.ROM_SETS[version].description}")
	suffix = "" if version == rom_loader.DEFAULT_VERSION else f"_{version}"
	main_out = out_dir / f"maincpu{suffix}.bin"
	dsp_out = out_dir / f"dsp{suffix}.bin"
	main_out.write_bytes(main_image)
	dsp_out.write_bytes(dsp_image)
	print(f"OK: wrote {len(main_image)} bytes -> {main_out}")
	print(f"OK: wrote {len(dsp_image)} bytes -> {dsp_out}")

	# Sanity: 68000 reset vector is the first 8 bytes: initial SSP (4 bytes),
	# initial PC (4 bytes). PC should point somewhere inside the 0x40000 ROM
	# window (mirrored at 0, so a sane firmware entry point is < 0x40000).
	initial_ssp = int.from_bytes(main_image[0:4], "big")
	initial_pc = int.from_bytes(main_image[4:8], "big")
	print(f"68000 reset vector: SSP={initial_ssp:#010x} PC={initial_pc:#010x}")
	if not (0 <= initial_pc < 0x40000):
		print("WARNING: initial PC is outside the ROM window -- byte order or interleave is probably wrong.")
	else:
		print("Initial PC looks sane (inside ROM window).")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
