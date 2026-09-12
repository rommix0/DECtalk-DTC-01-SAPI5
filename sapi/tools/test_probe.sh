#!/usr/bin/env bash
# test_probe.sh - Phase-B deliverable check for sapi_probe.exe (Task B4).
#
# Drives the real DectalkDtc01SAPI.dll through COM (DllGetClassObject, no
# regsvr32) for two distinct v20 voices (Perfect Paul, Beautiful Betty) and
# asserts:
#   1. each produced WAV is bigger than 20 KB (i.e. it isn't just a stub of
#      silence/header), and
#   2. the two WAVs' RMS levels differ (the voices are audibly distinct, not
#      accidentally rendering identical audio).
#
# WAV output goes to a temp dir, never into version control.
set -euo pipefail

ROM_DIR="${ROM_DIR:-C:/Users/abart/AppData/Local/Temp/claude/C--Users-abart-Desktop-dectalk-dtc01/3eabd03f-8840-493b-afea-9a8d448e84f3/scratchpad/roms_v20}"
CORE_DLL="${CORE_DLL:-C:/Users/abart/Desktop/dectalk-dtc01/build/dtc01_x64.dll}"
FIRMWARE="v20"
TEXT="Hello, this is a test of the emergency broadcast system."

export DTC01_ROM_DIR="$ROM_DIR"
export DTC01_CORE_DLL="$CORE_DLL"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

DLL=""
for candidate in \
    "$REPO_ROOT/sapi/build_x64/Release/DectalkDtc01SAPI.dll" \
    "$REPO_ROOT/sapi/build_x64/DectalkDtc01SAPI.dll"; do
    if [ -f "$candidate" ]; then
        DLL="$candidate"
        break
    fi
done
if [ -z "$DLL" ]; then
    echo "FAIL: DectalkDtc01SAPI.dll not found (build it first: cmake --build sapi/build_x64 --target DectalkDtc01SAPI --config Release)" >&2
    exit 1
fi

EXE=""
for candidate in \
    "$REPO_ROOT/sapi/build_x64/Release/sapi_probe.exe" \
    "$REPO_ROOT/sapi/build_x64/sapi_probe.exe"; do
    if [ -f "$candidate" ]; then
        EXE="$candidate"
        break
    fi
done
if [ -z "$EXE" ]; then
    echo "FAIL: sapi_probe.exe not found (build it first: cmake --build sapi/build_x64 --target sapi_probe --config Release)" >&2
    exit 1
fi

OUT_DIR="${TMPDIR:-/tmp}/dtc01_sapi_probe_test"
mkdir -p "$OUT_DIR"
PAUL_WAV="$OUT_DIR/paul.wav"
BETTY_WAV="$OUT_DIR/betty.wav"

echo "== probing paul (v20) via COM =="
"$EXE" "$DLL" "paul" "$FIRMWARE" "$PAUL_WAV" "$TEXT"

echo "== probing betty (v20) via COM =="
"$EXE" "$DLL" "betty" "$FIRMWARE" "$BETTY_WAV" "$TEXT"

PAUL_SIZE=$(stat -c%s "$PAUL_WAV" 2>/dev/null || stat -f%z "$PAUL_WAV")
BETTY_SIZE=$(stat -c%s "$BETTY_WAV" 2>/dev/null || stat -f%z "$BETTY_WAV")

echo "paul.wav size: $PAUL_SIZE bytes"
echo "betty.wav size: $BETTY_SIZE bytes"

if [ "$PAUL_SIZE" -le 20000 ]; then
    echo "FAIL: paul.wav is only $PAUL_SIZE bytes (expected > 20000)" >&2
    exit 1
fi
if [ "$BETTY_SIZE" -le 20000 ]; then
    echo "FAIL: betty.wav is only $BETTY_SIZE bytes (expected > 20000)" >&2
    exit 1
fi

# python.exe is a native Windows build; it can't resolve Git Bash's /tmp
# POSIX path, so convert to a Windows-style path before embedding it in the
# inline script (CLI args to sapi_probe.exe itself are auto-translated by
# MSYS since they look like bare POSIX paths, but text baked into a -c
# script string is not).
if command -v cygpath >/dev/null 2>&1; then
    PAUL_WAV_WIN="$(cygpath -w "$PAUL_WAV")"
    BETTY_WAV_WIN="$(cygpath -w "$BETTY_WAV")"
else
    PAUL_WAV_WIN="$PAUL_WAV"
    BETTY_WAV_WIN="$BETTY_WAV"
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
betty_rms = rms(r'$BETTY_WAV_WIN')
print('paul RMS: %.2f' % paul_rms)
print('betty RMS: %.2f' % betty_rms)
differ = abs(paul_rms - betty_rms) > 1.0
print('voices differ: %s' % differ)
sys.exit(0 if differ else 1)
"

echo "PASS"
