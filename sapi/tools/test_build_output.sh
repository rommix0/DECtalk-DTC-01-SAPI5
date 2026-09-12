#!/usr/bin/env bash
# test_build_output.sh - Task D2 deliverable check for sapi/build_all.bat.
#
# Runs build_all.bat (which builds both native cores, both bitnesses of the
# SAPI project, and stages everything into sapi/output/) and asserts the
# staged layout the Inno installer (Task D3) will consume:
#
#   sapi/output/DectalkDtc01SAPI.dll        (32-bit)
#   sapi/output/dtc01_x86.dll
#   sapi/output/DectalkConfig.exe
#   sapi/output/DectalkDiagnostics.exe
#   sapi/output/x64/DectalkDtc01SAPI.dll    (64-bit)
#   sapi/output/x64/dtc01_x64.dll
#   sapi/output/x64/DectalkDiagnostics.exe
#   sapi/output/roms/v20/*.rom              (only if the E: ROM source exists)
#   sapi/output/roms/v18/*.rom              (only if the E: ROM source exists)
#
# build_all.bat is invoked with `cmd //c` (double-slash) rather than `cmd /c`
# because MSYS/Git Bash rewrites a single-slash /c into a bogus filesystem
# path before cmd.exe ever sees it -- this project has hit that bug before.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="$REPO_ROOT/sapi/output"

ROM_V20_SRC="/e/Text-to-Speech Repo/TTS Hosts/DECTalk/Software/DECTalk Drivers/DECTalk DTC-01/ROMs/v2.0"
ROM_V18_SRC="/e/Text-to-Speech Repo/TTS Hosts/DECTalk/Software/DECTalk Drivers/DECTalk DTC-01/ROMs/v1.8"

cd "$REPO_ROOT"

echo "== running sapi\\build_all.bat =="
cmd //c "sapi\\build_all.bat"
BUILD_RC=$?
if [ $BUILD_RC -ne 0 ]; then
    echo "FAIL: build_all.bat exited $BUILD_RC" >&2
    exit 1
fi

FAILED=0

check_file() {
    local path="$1"
    if [ -f "$path" ]; then
        echo "PASS: $path exists"
    else
        echo "FAIL: $path is missing" >&2
        FAILED=1
    fi
}

check_file "$OUT_DIR/DectalkDtc01SAPI.dll"
check_file "$OUT_DIR/dtc01_x86.dll"
check_file "$OUT_DIR/DectalkConfig.exe"
check_file "$OUT_DIR/DectalkDiagnostics.exe"
check_file "$OUT_DIR/x64/DectalkDtc01SAPI.dll"
check_file "$OUT_DIR/x64/dtc01_x64.dll"
check_file "$OUT_DIR/x64/DectalkDiagnostics.exe"

check_roms() {
    local label="$1" dir="$2"
    local count
    count=$(find "$dir" -maxdepth 1 -name '*.rom' 2>/dev/null | wc -l)
    if [ "$count" -ge 15 ]; then
        echo "PASS: $label has $count *.rom files"
    else
        echo "FAIL: $label has only $count *.rom files (expected >= 15)" >&2
        FAILED=1
    fi
}

if [ -d "$ROM_V20_SRC" ] && [ -d "$ROM_V18_SRC" ]; then
    check_roms "$OUT_DIR/roms/v20" "$OUT_DIR/roms/v20"
    check_roms "$OUT_DIR/roms/v18" "$OUT_DIR/roms/v18"
else
    echo "SKIP: ROM source dirs not present on this machine; skipping ROM staging assertions"
fi

if [ $FAILED -ne 0 ]; then
    echo "FAIL"
    exit 1
fi

echo "PASS"
exit 0
