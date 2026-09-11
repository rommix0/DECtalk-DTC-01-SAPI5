"""Locate, validate, and assemble the DTC-01 ROM images.

The DTC-01 main-CPU and DSP ROMs are Digital Equipment Corp / Fonix
copyrighted firmware. This module never bundles ROM bytes -- it only knows
how to recognise a user-supplied dump (by content hash, not filename, so it
tolerates whatever naming convention the user's dump happens to use) and
assemble it into the linear images the emulator cores need.

See DESIGN.md sections 1 and 2 for the byte-interleave layout this mirrors,
taken directly from src/mame/dec/dectalk.cpp's ROM_START(dectalk).

Two firmware versions exist, matching MAME's two ROM_SYSTEM_BIOS entries
(`mame dectalk -bios v20|v18`). Selecting one swaps *both* the 16 main-CPU
EPROMs and the DSP PROM pair together -- they are not independently
mixable, see RomSet below. v2.0 is the default; v1.8 speaks correctly as of
2026-08-13 (it needed a scheduler fix -- DESIGN.md section 21) but stays
behind $DTC01_ROM_VERSION until the driver-level constants above the
emulator are re-measured against it.
"""

from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass


class RomValidationError(Exception):
	"""Raised when required ROM chips are missing or fail checksum validation."""


@dataclass(frozen=True)
class RomChunk:
	sha1: str
	size: int
	offset: int  # starting byte offset within the linear image
	label: str  # human-readable chip identity, for error messages only


@dataclass(frozen=True)
class RomSet:
	"""One firmware version: main-CPU chips and the DSP pair that goes with it.

	The pairing is not a convenience -- it is a hardware fact, and measured
	on this emulator, not just inherited from MAME's comments. Speaking one
	sentence through each of the four combinations (DESIGN.md section 21):

	    v1.8 + 165/166   peak 12544  rms  474  clipped 0
	    v1.8 + 204/205   peak 32768  rms 10390 clipped 61   <- rails
	    v2.0 + 204/205   peak 13536  rms 1157  clipped 0
	    v2.0 + 165/166   peak  9232  rms  193  clipped 0    <- quiet

	Keeping them in one record makes the wrong combination unrepresentable.

	`dsp` is a tuple of *candidate* DSP pairs in preference order, not a
	single pair: v2.0 shipped two DSP revisions -- the later 409/410 pair and
	the older 204/205 pair -- and a dump may hold either or both. The loader
	uses the first candidate a dump can satisfy, so 409/410 is preferred when
	present (it removes an audible crackle 204/205 has -- confirmed by
	listening) while a 204/205-only dump still works. v1.8 has just one
	candidate pair.
	"""
	name: str  # "v20" / "v18", matching MAME's -bios names
	description: str
	main_cpu: tuple[RomChunk, ...]
	dsp: tuple[tuple[RomChunk, ...], ...]  # candidate DSP pairs, preferred first


MAIN_CPU_IMAGE_SIZE = 0x40000
DSP_IMAGE_SIZE = 0x1000  # 0x800 words, 2 bytes/word

# 68000 main-CPU program ROM: 16 x 0x4000-byte chips, byte-interleaved in
# pairs (stride 2) into a linear 0x40000-byte big-endian word image.
# v2.0 firmware (first-half tag 23Jul84 / second-half tag 02Jul84).
MAIN_CPU_ROMS_V20: tuple[RomChunk, ...] = (
	RomChunk("e586de03e113683c2534fca1f3f40ba391193044", 0x4000, 0x00000, "23-123e5 @E8"),
	RomChunk("7954bb56b7591f8954403a22d34de31c7d5441ac", 0x4000, 0x00001, "23-119e5 @E22"),
	RomChunk("7724babf4ae5d77c0b4200f608d599058d04b25c", 0x4000, 0x08000, "23-124e5 @E7"),
	RomChunk("af5e4ea0b3631f7d6f16c22e86a33fa2cb520ee0", 0x4000, 0x08001, "23-120e5 @E21"),
	RomChunk("1b60cd71dfa83408b17e13f683b6bf3198c905cc", 0x4000, 0x10000, "23-125e5 @E6"),
	RomChunk("4ad0b00628a90085cd7c78a354256c39fd14db6c", 0x4000, 0x10001, "23-121e5 @E20"),
	RomChunk("e2b2415eec838ddd46094f2fea93fd289dd0caa2", 0x4000, 0x18000, "23-126e5 @E5"),
	RomChunk("92ab22a24484ad0d0f5c8a07347105509999f3ee", 0x4000, 0x18001, "23-122e5 @E19"),
	RomChunk("b5aec0bf37a176ff4d66d6a10357715957662ebd", 0x4000, 0x20000, "23-103e5 @E4"),
	RomChunk("891f3a3b4ce75ef14001257bc8f1f60463a9a7cb", 0x4000, 0x20001, "23-095e5 @E18"),
	RomChunk("4d6808f67cbdd316df23adc8ddf701df57aa854a", 0x4000, 0x28000, "23-104e5 @E3"),
	RomChunk("496c69e52cfa013173f7b9c500ce544a03ad01f7", 0x4000, 0x28001, "23-096e5 @E17"),
	RomChunk("de0c25687bab3ff0c88c98622092e0b58331aa16", 0x4000, 0x30000, "23-105e5 @E2"),
	RomChunk("c450abae0ccf372d7eb87370b8a8c97a45e164d3", 0x4000, 0x30001, "23-097e5 @E16"),
	RomChunk("355348bfc96a04193136cdde3418366e6476c3ca", 0x4000, 0x38000, "23-106e5 @E1"),
	RomChunk("01921e77b46c2d4845023605239c45ffa4a35872", 0x4000, 0x38001, "23-098e5 @E15"),
)

# v1.8 firmware (first-half tag 05Dec83 / second-half tag 11Oct83). Same
# chip positions, different part numbers; hashes from MAME's ROM_BIOS(1)
# entries and verified against the local dump.
MAIN_CPU_ROMS_V18: tuple[RomChunk, ...] = (
	RomChunk("1b1b9c1e092c44329b385fb04001e13422eb8d39", 0x4000, 0x00000, "23-063e5 @E8"),
	RomChunk("84bbe9ff303ea6ce7b1c0b1ad05421edd18fae49", 0x4000, 0x00001, "23-059e5 @E22"),
	RomChunk("fdd91e4d2ef92608a08b2e78b6108e31ff53a1f9", 0x4000, 0x08000, "23-064e5 @E7"),
	RomChunk("c95662d0d40499af01cdc23f05936762ab54081a", 0x4000, 0x08001, "23-060e5 @E21"),
	RomChunk("232b622cef6d69a493db1ed02e5236235c68daba", 0x4000, 0x10000, "23-065e5 @E6"),
	RomChunk("81daa4abae273c7f0aead902b5c3c842f7e7f116", 0x4000, 0x10001, "23-061e5 @E20"),
	RomChunk("5f9f916b99867d1adbafd58d411feb630f6e4b6d", 0x4000, 0x18000, "23-066e5 @E5"),
	RomChunk("46ee22a295b8709b6f829751aca5f92e4f459a9f", 0x4000, 0x18001, "23-062e5 @E19"),
	RomChunk("1d8008e30a448358224364fd8237dbb08907b219", 0x4000, 0x20000, "23-032e5 @E4"),
	RomChunk("55c759b3fb927d2dfc9d77e8e080748866bea854", 0x4000, 0x20001, "23-031e5 @E18"),
	RomChunk("738337c5b6acd3f30c3c4be2457370d2ce9313f9", 0x4000, 0x28000, "23-034e5 @E3"),
	RomChunk("5946ccd367d88a484bb1549d0cc990b9b7d88f0c", 0x4000, 0x28001, "23-033e5 @E17"),
	RomChunk("30f95e5383c4f71bc700346e2d49e8ad70b94c8c", 0x4000, 0x30000, "23-036e5 @E2"),
	RomChunk("7b3b68e61b421dedaad88b5600c739943a316c9e", 0x4000, 0x30001, "23-035e5 @E16"),
	RomChunk("abd6af442690e981a9089f19febffc8f3fb52717", 0x4000, 0x38000, "23-038e5 @E1"),
	RomChunk("a743a23625feadf6e46ef889e2bb04af88589992", 0x4000, 0x38001, "23-037e5 @E15"),
)

# TMS32010 DSP program ROM. Each chip supplies the high (offset 0x000) or low
# (offset 0x001) byte of every big-endian word.
#
# v2.0 has two DSP revisions in the wild. The later "409/410" pair is
# preferred: 204/205 emulates with an audible crackle on v2.0 that 409/410
# does not (confirmed by listening -- Phase-0 crackle investigation), and
# dumps that carry both file 409/410 as the primary set with 204/205 demoted
# to an "older" folder. "204/205" is MAME's pair and is kept as a fallback so
# a dump holding only it still works. "165/166" is the v1.8-era pair.
DSP_ROMS_V20_409: tuple[RomChunk, ...] = (
	RomChunk("3fabe018d0e0b478093951cb20501853358faa18", 0x800, 0x000, "23-410f4 @E70"),
	RomChunk("9a13426c92f879f2953f180f805990a91c37ac43", 0x800, 0x001, "23-409f4 @E69"),
)
DSP_ROMS_V20_204: tuple[RomChunk, ...] = (
	RomChunk("3136bae243ef48721e21c66fde70dab5fc3c21d0", 0x800, 0x000, "23-205f4 @E70"),
	RomChunk("9409f90f7a397b041e4440341f2d7934cb479285", 0x800, 0x001, "23-204f4 @E69"),
)
# Preference order: 409/410 first, 204/205 fallback.
DSP_ROMS_V20: tuple[tuple[RomChunk, ...], ...] = (DSP_ROMS_V20_409, DSP_ROMS_V20_204)
DSP_ROMS_V18: tuple[tuple[RomChunk, ...], ...] = ((
	RomChunk("e8c25ca092dde2dc0aec73921af806026bdfbbc3", 0x800, 0x000, "23-166f4 @E70"),
	RomChunk("249f269c38f7f44edb6d025bcc867c8ca0de3e9c", 0x800, 0x001, "23-165f4 @E69"),
),)

ROM_SETS: dict[str, RomSet] = {
	"v20": RomSet("v20", "DTC-01 Version 2.0", MAIN_CPU_ROMS_V20, DSP_ROMS_V20),
	"v18": RomSet("v18", "DTC-01 Version 1.8", MAIN_CPU_ROMS_V18, DSP_ROMS_V18),
}
DEFAULT_VERSION = "v20"
VERSION_ENV = "DTC01_ROM_VERSION"

# Back-compat aliases: plenty of callers pre-date the version split and mean
# "the normal firmware". DSP_ROMS is the preferred v2.0 pair (a flat tuple of
# chunks, as before), not the candidate list.
MAIN_CPU_ROMS = MAIN_CPU_ROMS_V20
DSP_ROMS = DSP_ROMS_V20_409

# Every chip of every version and every DSP revision, for the "never ship
# firmware" content check -- both DSP pairs are copyrighted firmware.
ALL_ROM_CHUNKS: tuple[RomChunk, ...] = tuple(
	c for s in ROM_SETS.values()
	for c in (s.main_cpu + tuple(chip for pair in s.dsp for chip in pair))
)

# Spellings a human might plausibly type into the env var.
_VERSION_ALIASES = {
	"v20": "v20", "20": "v20", "2.0": "v20", "v2.0": "v20", "2": "v20",
	"v18": "v18", "18": "v18", "1.8": "v18", "v1.8": "v18",
}


def resolve_version(version: str | None = None) -> str:
	"""Normalise a version selector to a ROM_SETS key.

	None consults $DTC01_ROM_VERSION, then falls back to DEFAULT_VERSION.
	Raises ValueError on anything unrecognised rather than silently
	falling back, so a typo'd env var is reported instead of quietly
	giving the user the other firmware.
	"""
	if version is None:
		version = os.environ.get(VERSION_ENV) or DEFAULT_VERSION
	key = _VERSION_ALIASES.get(str(version).strip().lower())
	if key is None:
		raise ValueError(
			f"unknown ROM version {version!r}; expected one of "
			f"{', '.join(sorted(ROM_SETS))}"
		)
	return key


def rom_set(version: str | None = None) -> RomSet:
	return ROM_SETS[resolve_version(version)]


def _index_dir_by_sha1(rom_dir: str) -> dict[str, bytes]:
	index: dict[str, bytes] = {}
	for name in os.listdir(rom_dir):
		path = os.path.join(rom_dir, name)
		if not os.path.isfile(path):
			continue
		with open(path, "rb") as f:
			data = f.read()
		index[hashlib.sha1(data).hexdigest()] = data
	return index


def _assemble(files_by_sha1: dict[str, bytes], chunks: tuple[RomChunk, ...],
              image_size: int) -> bytes:
	image = bytearray(image_size)
	missing: list[str] = []
	for chunk in chunks:
		data = files_by_sha1.get(chunk.sha1)
		if data is None:
			missing.append(f"{chunk.label} (sha1 {chunk.sha1})")
			continue
		if len(data) != chunk.size:
			missing.append(f"{chunk.label}: found matching hash but size {len(data)} != expected {chunk.size}")
			continue
		for i, b in enumerate(data):
			image[chunk.offset + i * 2] = b
	if missing:
		raise RomValidationError(
			"ROM validation failed, missing/invalid chips:\n  " + "\n  ".join(missing)
		)
	return bytes(image)


def _assemble_dsp(files_by_sha1: dict[str, bytes],
                  candidates: tuple[tuple[RomChunk, ...], ...]) -> bytes:
	"""Assemble the first DSP candidate pair the dump can satisfy.

	Candidates are tried in preference order (409/410 before 204/205 for
	v2.0). Only if *every* candidate is incomplete is an error raised, and it
	reports each attempt so the message names which pair(s) were missing what.
	"""
	failures: list[str] = []
	for pair in candidates:
		try:
			return _assemble(files_by_sha1, pair, DSP_IMAGE_SIZE)
		except RomValidationError as e:
			failures.append(str(e))
	raise RomValidationError(
		"no complete DSP ROM pair found; tried:\n" + "\n".join(failures)
	)


def build_main_cpu_image(rom_dir: str, version: str | None = None) -> bytes:
	"""Assemble the 0x40000-byte 68000 program ROM image from rom_dir."""
	return _assemble(_index_dir_by_sha1(rom_dir), rom_set(version).main_cpu,
	                 MAIN_CPU_IMAGE_SIZE)


def build_dsp_image(rom_dir: str, version: str | None = None) -> bytes:
	"""Assemble the DSP program ROM image (0x800 big-endian words) from rom_dir.

	NOTE: which chip supplies the high vs. low byte of each word is inferred
	from the same even/odd offset convention used for the (confirmed-correct)
	main CPU ROMs, not independently verified for the DSP region -- see
	DESIGN.md section 8. If the TMS32010 core fails to boot sensibly, check
	this byte order first.

	The preferred DSP revision present in rom_dir is used (see RomSet.dsp).
	"""
	return _assemble_dsp(_index_dir_by_sha1(rom_dir), rom_set(version).dsp)


def dsp_words(rom_dir: str, version: str | None = None) -> list[int]:
	image = build_dsp_image(rom_dir, version)
	return [
		(image[i] << 8) | image[i + 1]
		for i in range(0, len(image), 2)
	]


def build_images(rom_dir: str, version: str | None = None) -> tuple[bytes, list[int]]:
	"""Both images from a single scan of rom_dir -- what the emulators want."""
	files = _index_dir_by_sha1(rom_dir)
	rs = rom_set(version)
	main = _assemble(files, rs.main_cpu, MAIN_CPU_IMAGE_SIZE)
	dsp = _assemble_dsp(files, rs.dsp)
	return main, [(dsp[i] << 8) | dsp[i + 1] for i in range(0, len(dsp), 2)]


def available_versions(rom_dir: str) -> list[str]:
	"""Which firmware versions rom_dir holds a complete, valid set for.

	One directory scan for all versions; a dump like MAME's `dectalk.zip`
	holds every chip of both, and selection is by content hash, so having
	them side by side is normal rather than ambiguous.
	"""
	files = _index_dir_by_sha1(rom_dir)
	found = []
	for name, rs in ROM_SETS.items():
		try:
			_assemble(files, rs.main_cpu, MAIN_CPU_IMAGE_SIZE)
			_assemble_dsp(files, rs.dsp)
		except RomValidationError:
			continue
		found.append(name)
	return found


def validate_rom_dir(rom_dir: str, version: str | None = None) -> None:
	"""Raise RomValidationError with a full report if rom_dir is unusable.

	Checks only the selected version: a dump holding just one firmware is a
	perfectly good dump, and demanding both would reject it.
	"""
	rs = rom_set(version)
	files = _index_dir_by_sha1(rom_dir)
	errors: list[str] = []
	try:
		_assemble(files, rs.main_cpu, MAIN_CPU_IMAGE_SIZE)
	except RomValidationError as e:
		errors.append(f"[{rs.description} main CPU] {e}")
	try:
		_assemble_dsp(files, rs.dsp)
	except RomValidationError as e:
		errors.append(f"[{rs.description} DSP] {e}")
	if errors:
		raise RomValidationError("\n".join(errors))
