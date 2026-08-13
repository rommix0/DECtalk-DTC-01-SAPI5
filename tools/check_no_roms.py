"""Refuse to let copyrighted firmware into the repository.

The DTC-01 ROMs are Digital Equipment Corp / Fonix property and must never
be committed or shipped (DESIGN.md §0). .gitignore patterns are a
convenience, not a guarantee -- a renamed dump, a zip, or an assembled
image would slip straight past them. This checks the actual *content* of
everything git is tracking against the known ROM hashes, so the rule holds
regardless of filename.

Run before committing, or wire it in as a pre-commit hook:

    python tools/check_no_roms.py
"""

from __future__ import annotations

import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "addon" / "synthDrivers" / "dectalkDtc01"))

from emu import rom_loader  # noqa: E402

# Every chip the loader knows about, by content hash -- both firmware
# versions, since v1.8 is no less copyrighted than v2.0.
ROM_SHA1 = {c.sha1: c.label for c in rom_loader.ALL_ROM_CHUNKS}

# A dump that is not one of the known-good chips is still firmware. Files of
# exactly these sizes get a closer look.
SUSPICIOUS_SIZES = {0x4000, 0x800, 0x40000, 0x1000}
TEXT_SUFFIXES = {".py", ".md", ".c", ".h", ".txt", ".ini", ".bat", ".json",
                 ".yml", ".yaml", ".cfg", ".toml", ".gitignore"}


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("not a git repository (or git unavailable)")
    return [ROOT / p for p in out.stdout.split("\0") if p]


def staged_files():
    out = subprocess.run(["git", "diff", "--cached", "--name-only", "-z"],
                         cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        return []
    return [ROOT / p for p in out.stdout.split("\0") if p]


def scan(paths):
    problems = []
    for path in paths:
        if not path.is_file():
            continue
        data = path.read_bytes()
        digest = hashlib.sha1(data).hexdigest()
        if digest in ROM_SHA1:
            problems.append(f"{path.relative_to(ROOT)}: IS the ROM chip "
                            f"{ROM_SHA1[digest]} (sha1 {digest})")
            continue
        # An archive could carry the ROMs inside it.
        if zipfile.is_zipfile(path):
            try:
                with zipfile.ZipFile(path) as z:
                    for info in z.infolist():
                        inner = hashlib.sha1(z.read(info)).hexdigest()
                        if inner in ROM_SHA1:
                            problems.append(
                                f"{path.relative_to(ROOT)} contains ROM chip "
                                f"{ROM_SHA1[inner]} as {info.filename}")
            except Exception:
                pass
            continue
        if path.suffix.lower() in TEXT_SUFFIXES:
            continue
        if len(data) in SUSPICIOUS_SIZES:
            problems.append(f"{path.relative_to(ROOT)}: {len(data)} bytes, the "
                            f"size of a ROM image -- check this is not firmware")
    return problems


def main() -> int:
    paths = set(tracked_files()) | set(staged_files())
    print(f"checking {len(paths)} tracked/staged files for firmware content")
    problems = scan(paths)
    if problems:
        print("\nREFUSING: copyrighted firmware would enter the repository")
        for p in problems:
            print("  ", p)
        print("\nThe DTC-01 ROMs are DEC/Fonix property (DESIGN.md §0).")
        return 1
    print("clean: no ROM content is tracked or staged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
