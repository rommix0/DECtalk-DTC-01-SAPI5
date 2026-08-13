"""Package the DECtalk DTC-01 synth driver as an installable .nvda-addon.

Ships **no ROM bytes** -- the DTC-01 firmware is DEC/Fonix copyright
(DESIGN.md §0). The packaged addon refuses to build if anything ROM-shaped
sneaks in. At runtime the driver looks for a user-supplied dump in, in
order: $DTC01_ROM_DIR, <NVDA config>/dectalkDtc01/roms, <addon>/roms.

Usage:  python tools/make_addon.py [--arch x64] [--version X.Y.Z]
Output: build/dectalkDtc01-<version>-<arch>.nvda-addon
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

NVDA_DIR = Path(r"C:\Program Files\NVDA")
VERSION_FILE = Path(__file__).resolve().parent.parent / "VERSION"


def read_version():
	"""The single source of truth for the add-on version.

	The manifest, the package filename and the running add-on all have to
	agree: the auto-updater compares a GitHub release tag against the
	installed manifest, so a version living in two places means it either
	misses updates or offers the same one forever.
	"""
	if not VERSION_FILE.is_file():
		raise SystemExit(f"missing {VERSION_FILE} -- it holds the add-on version")
	return VERSION_FILE.read_text(encoding="utf-8").strip()


def git_tag():
	"""Latest git tag, so a mismatch with VERSION can be pointed out."""
	try:
		out = subprocess.run(["git", "describe", "--tags", "--abbrev=0"],
							 cwd=ROOT, capture_output=True, text=True)
		return out.stdout.strip() if out.returncode == 0 else ""
	except Exception:
		return ""

ROOT = Path(__file__).resolve().parent.parent
ADDON = ROOT / "addon"
BUILD = ROOT / "build"


def newest_native_source():
	"""mtime of the most recently edited file the DLLs are built from."""
	newest = 0.0
	for p in (ROOT / "native").rglob("*"):
		if p.is_file() and p.suffix.lower() in (".c", ".h"):
			newest = max(newest, p.stat().st_mtime)
	return newest


def select_dll(arch):
	"""Pick build/pgo/dtc01_<arch>.dll over build/dtc01_<arch>.dll, but only
	when it is newer than every native source.

	A PGO build is produced by a separate three-step cycle, so it goes stale
	the moment anyone edits the emulator and runs the ordinary build. Shipping
	a stale one would mean releasing a DLL built from code that no longer
	exists, and nothing about the package would look wrong. This project has
	already shipped one stale artifact behind a reassuring "built" banner.

	Returns (path or None, is_pgo).
	"""
	plain = BUILD / f"dtc01_{arch}.dll"
	pgo = BUILD / "pgo" / f"dtc01_{arch}.dll"
	if pgo.is_file():
		srcTime = newest_native_source()
		if pgo.stat().st_mtime >= srcTime:
			print(f"  {arch}: PGO build ({pgo})")
			return pgo, True
		print(f"  {arch}: WARNING - PGO build is older than native/ sources; "
			  f"ignoring it and using the ordinary build. Re-run "
			  f"tools\\build_pgo.bat to refresh it.")
	if plain.is_file():
		print(f"  {arch}: ordinary build ({plain})")
		return plain, False
	return None, False


def resolve_rom_dir(spec):
	"""Locate and validate a ROM set for a --with-roms private build.

	Validated with the driver's own loader rather than by counting files: a
	package that ships an incomplete or altered set would fail at runtime on
	the very machines the build exists to test, with the ROMs looking present.
	"""
	if spec == "auto":
		appdata = os.environ.get("APPDATA")
		if not appdata:
			print("ERROR: APPDATA is unset; pass --with-roms DIR explicitly")
			return None
		path = Path(appdata) / "nvda" / "dectalkDtc01" / "roms"
	else:
		path = Path(spec)
	if not path.is_dir():
		print(f"ERROR: no ROM directory at {path}")
		return None
	sys.path.insert(0, str(ADDON / PKG_REL))
	try:
		from emu import rom_loader
		rom_loader.validate_rom_dir(str(path))
	except Exception as e:
		print(f"ERROR: ROMs in {path} are not a valid set: {e}")
		return None
	finally:
		sys.path.pop(0)
	n = len([p for p in path.iterdir() if p.is_file()])
	print(f"PRIVATE BUILD: bundling {n} validated ROM files from {path}")
	return path

PKG_REL = Path("synthDrivers/dectalkDtc01")

MANIFEST = """name = dectalkDtc01
summary = {summary}
description = \"\"\"Speech synthesizer driver for the 1984 DEC DECtalk DTC-01. The original 68000 and TMS32010 firmware runs in an emulator inside NVDA, so the voice is the real hardware's, not a recreation.

{roms_note}

Includes both 64-bit and 32-bit emulator cores; the matching one is selected automatically.\"\"\"
author = "dtc-01 project"
version = {version}
minimumNVDAVersion = 2025.1
lastTestedNVDAVersion = 2026.1
"""

ROMS_NOTE_DIST = ('Requires your own dump of the DTC-01 firmware ROMs; none are '
                  'included. Place the ROM files in the "dectalkDtc01\\\\roms" folder '
                  'inside your NVDA user configuration directory.')

# --with-roms only. The firmware is DEC/Fonix property, so a package carrying
# it is the builder's own dump moved between their own machines and nothing
# else -- it must never be uploaded, released or shared (DESIGN.md §0).
ROMS_NOTE_LOCAL = ('*** PRIVATE BUILD -- DO NOT DISTRIBUTE. *** This package '
                   'contains DTC-01 firmware ROMs, which are Digital Equipment '
                   'Corporation / Fonix property. It exists only to move one '
                   "person's own ROM dump onto their own test machines. Do not "
                   'upload, release or share it.')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="x64", choices=["x64", "x86"],
                    help="DLL to bundle; NVDA 2026 is x64 (DESIGN.md §14)")
    ap.add_argument("--version", default=None,
                    help="override the VERSION file (not normally needed)")
    ap.add_argument("--require-pgo", action="store_true",
                    help="fail unless the --arch DLL is a PGO build (intended "
                         "for release packaging; x86 always ships ordinary)")
    ap.add_argument("--with-roms", nargs="?", const="auto", default=None,
                    metavar="DIR",
                    help="PRIVATE BUILD: bundle firmware ROMs from DIR (default: "
                         "your NVDA config ROM dir). Never distribute the result.")
    args = ap.parse_args()

    version = args.version or read_version()

    romDir = None
    if args.with_roms is not None:
        romDir = resolve_rom_dir(args.with_roms)
        if romDir is None:
            return 1
    tag = git_tag()
    if tag and tag.lstrip("vV") != version:
        print(f"NOTE: VERSION is {version} but the latest git tag is {tag} -- "
              f"tag the release v{version} so the updater matches.")

    # Gate: every NVDA symbol we import must exist in the real NVDA install.
    # The offline harness uses stubs, so only this catches a wrong import
    # path -- which is exactly how a bad `driverHandler` import once shipped
    # and stopped the synth loading at all.
    # Only a missing NVDA install is tolerated. Anything else -- including a
    # bug in the gate itself -- must fail loudly: a check that silently
    # skips is worse than no check, and this one skipped on a NameError the
    # first time it ran.
    checker = ROOT / "tools" / "check_nvda_api.py"
    if not NVDA_DIR.is_dir():
        print(f"(NVDA API check skipped: no NVDA install at {NVDA_DIR})")
    else:
        rc = subprocess.call([sys.executable, str(checker)])
        if rc != 0:
            print("ERROR: NVDA API check failed; refusing to package.")
            return 1

    # Bundle every architecture that has been built. emu/native.py picks the
    # matching DLL at load time, so one package serves both 64- and 32-bit
    # NVDA. Shipping x64 alone produced an add-on that installed happily on
    # 32-bit NVDA and then failed to load its DLL, with nothing saying why.
    dlls, pgoUsed = [], {}
    for a in ("x64", "x86"):
        chosen, isPgo = select_dll(a)
        if chosen:
            dlls.append(chosen)
            pgoUsed[a] = isPgo
    if not dlls:
        print(f"ERROR: no dtc01_*.dll in {BUILD}. Run  tools\\build_native.bat")
        return 1
    if args.arch not in [d.stem.split("_")[1] for d in dlls]:
        print(f"ERROR: dtc01_{args.arch}.dll not built")
        return 1
    # Only the release architecture is held to this. x86 cannot be PGO'd here
    # at all: training has to run the instrumented DLL in a matching-
    # architecture process and there is no 32-bit Python on this machine, so
    # requiring it everywhere would make the flag permanently unusable.
    if args.require_pgo and not pgoUsed.get(args.arch):
        print(f"ERROR: --require-pgo, but {args.arch} is not a PGO build. Run "
              f"tools\\build_pgo.bat instrument, tools\\pgo_train.py, "
              f"tools\\build_pgo.bat optimize.")
        return 1

    stage = BUILD / f"_stage_addon_{args.arch}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    (stage / "manifest.ini").write_text(
        MANIFEST.format(
            version=version,
            summary='"DECtalk DTC-01 (private ROM build)"' if romDir else '"DECtalk DTC-01"',
            roms_note=ROMS_NOTE_LOCAL if romDir else ROMS_NOTE_DIST,
        ),
        encoding="utf-8")

    dst_pkg = stage / PKG_REL
    shutil.copytree(
        ADDON / PKG_REL, dst_pkg,
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.dll", "*.lib",
                                      "*.exp", "roms"),
    )
    (stage / "synthDrivers" / "__init__.py").write_text("", encoding="utf-8")
    for d in dlls:
        shutil.copy2(d, dst_pkg / "emu" / d.name)

    # <addon>/roms is the driver's last-resort ROM location (__init__.py
    # _candidateRomDirs), below the config dir -- so a bundled set is used
    # only where the machine has no dump of its own.
    if romDir:
        shutil.copytree(romDir, dst_pkg / "roms")

    # An allowlist, not a glob: the update checker and the ROM-folder settings
    # panel ship, the native smoke test does not -- it is a development tool
    # that announces itself on every NVDA start.
    plugins = stage / "globalPlugins"
    plugins.mkdir(parents=True, exist_ok=True)
    for name in ("dtc01Updater.py", "dtc01Settings.py"):
        shutil.copy2(ADDON / "globalPlugins" / name, plugins / name)

    # The filename is the main thing standing between a private build and an
    # accidental upload, so make it impossible to confuse with a release.
    suffix = "-PRIVATE-WITH-ROMS-DO-NOT-DISTRIBUTE" if romDir else ""
    out = BUILD / f"dectalkDtc01-{version}{suffix}.nvda-addon"
    if out.exists():
        out.unlink()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(stage).as_posix())

    with zipfile.ZipFile(out) as z:
        names = z.namelist()
        total = sum(i.file_size for i in z.infolist())

    romFiles = [n for n in names
                if n.lower().endswith((".bin", ".rom", ".e1", ".e2", ".u21"))
                or "rom" in Path(n).parent.name.lower()]
    if romDir:
        # Inverted guard: the whole point of this build is the ROMs, so verify
        # they actually made it in rather than handing over a package that
        # silently behaves like the distributable one.
        if not romFiles:
            print("ERROR: --with-roms was given but no ROM files are in the package")
            out.unlink()
            return 1
    elif romFiles:
        # Hard guard: never ship firmware.
        print("ERROR: refusing to ship, ROM-like files present:", romFiles)
        out.unlink()
        return 1

    print(f"built {out}")
    if romDir:
        print(f"  {len(romFiles)} ROM files bundled")
        print("  *** PRIVATE BUILD -- contains DEC/Fonix firmware. Do not upload,")
        print("      release, or attach to a GitHub release. Local testing only.")
    print(f"  {len(names)} files, {out.stat().st_size/1024:.0f} KB packed "
          f"/ {total/1024:.0f} KB unpacked")
    for n in names:
        print("   ", n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
