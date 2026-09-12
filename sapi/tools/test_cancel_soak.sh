#!/usr/bin/env bash
# test_cancel_soak.sh - Task E2 deliverable check for cancel_probe.exe.
#
# Drives the real DectalkDtc01SAPI.dll through COM (DllGetClassObject, no
# regsvr32) for 60 utterances against one persistent engine instance, most of
# them cancelled mid-stream via SPVES_ABORT, asserting:
#   1. no Speak() call ever hangs (a per-call watchdog inside cancel_probe.exe
#      itself is the primary guard; the `timeout` below is a backstop), and
#   2. every full/recovery utterance interleaved between aborts still comes
#      out full-length -- i.e. an abort never leaves residual emulator state
#      that truncates or corrupts the next utterance.
#
# Mirrors test_probe.sh's ROM_DIR/CORE_DLL defaults and DLL/exe discovery.
set -euo pipefail

ROM_DIR="${ROM_DIR:-C:/Users/abart/AppData/Local/Temp/claude/C--Users-abart-Desktop-dectalk-dtc01/3eabd03f-8840-493b-afea-9a8d448e84f3/scratchpad/roms_v20}"
CORE_DLL="${CORE_DLL:-C:/Users/abart/Desktop/dectalk-dtc01/build/dtc01_x64.dll}"

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
    "$REPO_ROOT/sapi/build_x64/Release/cancel_probe.exe" \
    "$REPO_ROOT/sapi/build_x64/cancel_probe.exe"; do
    if [ -f "$candidate" ]; then
        EXE="$candidate"
        break
    fi
done
if [ -z "$EXE" ]; then
    echo "FAIL: cancel_probe.exe not found (build it first: cmake --build sapi/build_x64 --target cancel_probe --config Release)" >&2
    exit 1
fi

echo "== cancel soak: 60 utterances against one persistent engine instance =="
echo "ROM_DIR=$ROM_DIR"
echo "CORE_DLL=$CORE_DLL"

# The per-Speak watchdog inside cancel_probe.exe (30s/call) is the primary
# hang guard; this overall timeout is just a backstop in case the watchdog
# itself somehow doesn't fire (e.g. the process wedges before spawning the
# worker thread). set -e is suspended around the call so a non-zero exit is
# reported with a clear FAIL message instead of silently aborting the script.
set +e
if command -v timeout >/dev/null 2>&1; then
    timeout 600 "$EXE" "$DLL"
else
    "$EXE" "$DLL"
fi
STATUS=$?
set -e

if [ "$STATUS" -ne 0 ]; then
    echo "FAIL: cancel_probe exited $STATUS" >&2
    exit 1
fi

echo "PASS"
