#!/usr/bin/env bash
# test_speak.sh - Phase-A deliverable check for dtc01_speak.exe.
#
# Speaks the same line through two distinct v20 voices (Perfect Paul, Huge
# Harry) and asserts:
#   1. each produced WAV is bigger than 20 KB (i.e. it isn't just a stub of
#      silence/header), and
#   2. the two WAVs' RMS levels differ (the voices are audibly distinct, not
#      accidentally rendering identical audio).
#
# WAV output goes to a temp dir, never into version control.
set -euo pipefail

ROM_DIR="${ROM_DIR:-C:/Users/abart/AppData/Local/Temp/claude/C--Users-abart-Desktop-dectalk-dtc01/3eabd03f-8840-493b-afea-9a8d448e84f3/scratchpad/roms_v20}"
DLL="${DLL:-C:/Users/abart/Desktop/dectalk-dtc01/build/dtc01_x64.dll}"
FIRMWARE="v20"
TEXT="Hello, this is a test of the emergency broadcast system."

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

EXE=""
for candidate in \
    "$REPO_ROOT/sapi/build_x64/Release/dtc01_speak.exe" \
    "$REPO_ROOT/sapi/build_x64/dtc01_speak.exe" \
    "$REPO_ROOT/sapi/build_x86/Release/dtc01_speak.exe" \
    "$REPO_ROOT/sapi/build_x86/dtc01_speak.exe"; do
    if [ -f "$candidate" ]; then
        EXE="$candidate"
        break
    fi
done

if [ -z "$EXE" ]; then
    echo "FAIL: dtc01_speak.exe not found (build it first: cmake --build sapi/build_x64 --target dtc01_speak --config Release)" >&2
    exit 1
fi

OUT_DIR="${TMPDIR:-/tmp}/dtc01_speak_test"
mkdir -p "$OUT_DIR"
PAUL_WAV="$OUT_DIR/paul.wav"
HARRY_WAV="$OUT_DIR/harry.wav"

echo "== speaking paul (v20) =="
"$EXE" "$ROM_DIR" "$DLL" "$FIRMWARE" "paul" "$TEXT" "$PAUL_WAV"

echo "== speaking harry (v20) =="
"$EXE" "$ROM_DIR" "$DLL" "$FIRMWARE" "harry" "$TEXT" "$HARRY_WAV"

PAUL_SIZE=$(stat -c%s "$PAUL_WAV" 2>/dev/null || stat -f%z "$PAUL_WAV")
HARRY_SIZE=$(stat -c%s "$HARRY_WAV" 2>/dev/null || stat -f%z "$HARRY_WAV")

echo "paul.wav size: $PAUL_SIZE bytes"
echo "harry.wav size: $HARRY_SIZE bytes"

if [ "$PAUL_SIZE" -le 20000 ]; then
    echo "FAIL: paul.wav is only $PAUL_SIZE bytes (expected > 20000)" >&2
    exit 1
fi
if [ "$HARRY_SIZE" -le 20000 ]; then
    echo "FAIL: harry.wav is only $HARRY_SIZE bytes (expected > 20000)" >&2
    exit 1
fi

# python.exe is a native Windows build; it can't resolve Git Bash's /tmp
# POSIX path, so convert to a Windows-style path before embedding it in the
# inline script (CLI args to dtc01_speak.exe itself are auto-translated by
# MSYS since they look like bare POSIX paths, but text baked into a -c
# script string is not).
if command -v cygpath >/dev/null 2>&1; then
    PAUL_WAV_WIN="$(cygpath -w "$PAUL_WAV")"
    HARRY_WAV_WIN="$(cygpath -w "$HARRY_WAV")"
else
    PAUL_WAV_WIN="$PAUL_WAV"
    HARRY_WAV_WIN="$HARRY_WAV"
fi

python -c "
import struct, sys, wave

def rms(path):
    with wave.open(path, 'rb') as w:
        n = w.getnframes()
        data = w.readframes(n)
    samples = struct.unpack('<%dh' % (len(data) // 2), data)
    if not samples:
        return 0.0
    return (sum(s * s for s in samples) / len(samples)) ** 0.5

paul_rms = rms(r'$PAUL_WAV_WIN')
harry_rms = rms(r'$HARRY_WAV_WIN')
print('paul RMS: %.2f' % paul_rms)
print('harry RMS: %.2f' % harry_rms)
differ = abs(paul_rms - harry_rms) > 1.0
print('voices differ: %s' % differ)
sys.exit(0 if differ else 1)
"

echo "PASS"
