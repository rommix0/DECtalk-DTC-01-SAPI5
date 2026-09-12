#!/usr/bin/env bash
# test_audio_identity.sh - Task E3 deliverable check.
#
# Proves the C++ SAPI render (sapi_probe.exe, driving the real
# DectalkDtc01SAPI.dll through COM) is statistically identical to an
# independent Python oracle (emu.native.NativeMachine) driving the SAME
# native core with the IDENTICAL command bytes DectalkTtsEngine::Speak
# assembles -- and that neither leaks the DTC-01's power-on boot
# announcement. See sapi/tools/verify_audio.py and the task-E3 brief for
# the full method; this script only does discovery/plumbing.
#
# Mirrors test_probe.sh's discovery of ROM dir / core DLL / built exes.
set -euo pipefail

ROM_DIR="${ROM_DIR:-C:/Users/abart/AppData/Local/Temp/claude/C--Users-abart-Desktop-dectalk-dtc01/3eabd03f-8840-493b-afea-9a8d448e84f3/scratchpad/roms_all}"
CORE_DLL="${CORE_DLL:-C:/Users/abart/Desktop/dectalk-dtc01/build/dtc01_x64.dll}"

export DTC01_ROM_DIR="$ROM_DIR"
export DTC01_CORE_DLL="$CORE_DLL"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# A flat dir holding BOTH firmwares' ROM chips is required -- v20 and v18
# test voices both need to resolve out of the same rom_dir (rom_loader.py
# identifies chips by content hash, not name/location, so both firmwares'
# files can coexist flat in one directory). Reuse an existing one if
# present; otherwise assemble it from sapi/output/roms/{v20,v18}/.
if [ ! -d "$ROM_DIR" ]; then
    echo "== $ROM_DIR missing; assembling a flat ROM dir from sapi/output/roms =="
    if [ ! -d "$REPO_ROOT/sapi/output/roms/v20" ] || [ ! -d "$REPO_ROOT/sapi/output/roms/v18" ]; then
        echo "FAIL: sapi/output/roms/{v20,v18} not found -- run build_all.bat first" >&2
        exit 1
    fi
    mkdir -p "$ROM_DIR"
    cp "$REPO_ROOT/sapi/output/roms/v20/"*.rom "$ROM_DIR/"
    cp "$REPO_ROOT/sapi/output/roms/v18/"*.rom "$ROM_DIR/"
fi

find_exe() {
    local name="$1"
    local candidate
    for candidate in \
        "$REPO_ROOT/sapi/build_x64/Release/$name" \
        "$REPO_ROOT/sapi/build_x64/$name"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

DLL="$(find_exe DectalkDtc01SAPI.dll)" || {
    echo "FAIL: DectalkDtc01SAPI.dll not found (build it first: cmake --build sapi/build_x64 --target DectalkDtc01SAPI --config Release)" >&2
    exit 1
}
SAPI_PROBE="$(find_exe sapi_probe.exe)" || {
    echo "FAIL: sapi_probe.exe not found (build it first: cmake --build sapi/build_x64 --target sapi_probe --config Release)" >&2
    exit 1
}
DUMP_PIPELINE="$(find_exe dump_pipeline.exe)" || {
    echo "FAIL: dump_pipeline.exe not found (build it first: cmake --build sapi/build_x64 --target dump_pipeline --config Release)" >&2
    exit 1
}

echo "DLL:           $DLL"
echo "sapi_probe:    $SAPI_PROBE"
echo "dump_pipeline: $DUMP_PIPELINE"
echo "ROM dir:       $ROM_DIR"
echo "core DLL:      $CORE_DLL"
echo

# Native Windows python -- do NOT let git-bash reinterpret the Windows-style
# ("C:/...") paths above; they're already in the form python.exe/CreateProcess
# expects, matching test_probe.sh's convention.
if timeout 300 python "$REPO_ROOT/sapi/tools/verify_audio.py" \
    "$DLL" "$SAPI_PROBE" "$DUMP_PIPELINE" "$ROM_DIR" "$CORE_DLL"; then
    echo "PASS"
    exit 0
else
    STATUS=$?
    echo "FAIL (verify_audio.py exit $STATUS)" >&2
    exit "$STATUS"
fi
