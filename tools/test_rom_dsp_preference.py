"""Regression test: the v2.0 DSP ROM has two revisions in the wild -- the
older 204/205 pair (MAME's) and the later, cleaner 409/410 pair. When a dump
holds both, the loader must prefer 409/410 (it removes an audible crackle,
confirmed by listening -- see the Phase-0 crackle investigation); when a dump
holds only one, that one must still work.

Firmware is never committed, so this builds its test directories from a real
dump. Point it at one:

    python tools/test_rom_dsp_preference.py <rom-source-dir>

where <rom-source-dir> contains, anywhere beneath it, the 16 v2.0 main-CPU
chips and both DSP pairs (204/205 and 409/410). $DTC01_ROM_SRC is used if no
argument is given.
"""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "addon" / "synthDrivers" / "dectalkDtc01"))

from emu import rom_loader  # noqa: E402

# Hashes of the two v2.0 DSP pairs, so the test can pick the right files out
# of the source dump by content rather than by filename.
DSP_204 = {"9409f90f7a397b041e4440341f2d7934cb479285",  # 23-204f4 @E69
           "3136bae243ef48721e21c66fde70dab5fc3c21d0"}  # 23-205f4 @E70
DSP_409 = {"9a13426c92f879f2953f180f805990a91c37ac43",  # 23-409f4 @E69
           "3fabe018d0e0b478093951cb20501853358faa18"}  # 23-410f4 @E70
MAIN_V20 = {c.sha1 for c in rom_loader.MAIN_CPU_ROMS_V20}


def _find_by_sha1(src: Path) -> dict[str, Path]:
    import hashlib
    out: dict[str, Path] = {}
    for p in src.rglob("*"):
        if p.is_file():
            h = hashlib.sha1(p.read_bytes()).hexdigest()
            out.setdefault(h, p)
    return out


def _stage(tmp: Path, files: dict[str, Path], wanted: set[str]) -> Path:
    tmp.mkdir(parents=True, exist_ok=True)
    for i, h in enumerate(wanted):
        (tmp / f"{i:02d}.rom").write_bytes(files[h].read_bytes())
    return tmp


def main() -> int:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DTC01_ROM_SRC", ""))
    if not src.is_dir():
        print(f"ERROR: give a ROM source dir (got {src!r})")
        return 2

    files = _find_by_sha1(src)
    for need, label in ((MAIN_V20, "v2.0 main CPU"), (DSP_204, "DSP 204/205"), (DSP_409, "DSP 409/410")):
        missing = need - files.keys()
        if missing:
            print(f"ERROR: source dump is missing {label} chips: {missing}")
            return 2

    expected_409 = None
    ok = True
    with tempfile.TemporaryDirectory() as td:
        base = Path(td)
        d_both = _stage(base / "both", files, MAIN_V20 | DSP_204 | DSP_409)
        d_204 = _stage(base / "only204", files, MAIN_V20 | DSP_204)
        d_409 = _stage(base / "only409", files, MAIN_V20 | DSP_409)

        # The 409/410 words we expect the loader to produce (410 hi, 409 lo).
        _, expected_409 = rom_loader.build_images(str(d_409), "v20")

        # 1. Both present -> prefer 409/410.
        _, words_both = rom_loader.build_images(str(d_both), "v20")
        if words_both == expected_409:
            print("PASS: dump with both pairs uses 409/410")
        else:
            print("FAIL: dump with both pairs did NOT use 409/410 (crackle would remain)")
            ok = False

        # 2. Only 204/205 present -> still loads (backward compatible).
        try:
            rom_loader.build_images(str(d_204), "v20")
            rom_loader.validate_rom_dir(str(d_204), "v20")
            print("PASS: dump with only 204/205 still loads")
        except rom_loader.RomValidationError as e:
            print(f"FAIL: dump with only 204/205 rejected: {e}")
            ok = False

        # 3. Only 409/410 present -> loads and is recognised as v2.0.
        try:
            rom_loader.validate_rom_dir(str(d_409), "v20")
            avail = rom_loader.available_versions(str(d_409))
            if "v20" in avail:
                print("PASS: dump with only 409/410 loads and reports v20 available")
            else:
                print(f"FAIL: only-409/410 dump did not report v20 available: {avail}")
                ok = False
        except rom_loader.RomValidationError as e:
            print(f"FAIL: dump with only 409/410 rejected: {e}")
            ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
