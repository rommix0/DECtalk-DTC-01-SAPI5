#!/bin/bash
# test_installer_build.sh - Task D3: proves sapi/installer/DectalkDtc01SAPI.iss
# actually compiles into a working setup exe, not just that the .iss file
# parses in someone's head.
#
# Run from a git-bash/MSYS shell (this repo's environment mangles /c, /Q,
# /flag-style args passed straight to a native exe -- see the header comment
# in sapi/build_all.bat -- so ISCC is invoked here with the "//" escape,
# same as this file's own callers are told to use).
#
#   bash sapi/tools/test_installer_build.sh
#
# Locates ISCC.exe in the usual install locations; if it isn't found this
# prints SKIP and exits 0, since a build machine without Inno Setup
# shouldn't hard-fail a run of the whole test suite over a missing optional
# tool. If sapi/output/ isn't already staged, this stages it first via
# build_all.bat (which also builds the two engine DLLs the .iss packages).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SAPI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SAPI_DIR/.." && pwd)"

ISS="$SAPI_DIR/installer/DectalkDtc01SAPI.iss"
OUTPUT_DIR="$SAPI_DIR/output"
SETUP_EXE="$OUTPUT_DIR/DectalkDtc01_SAPI_Setup.exe"

# --- locate ISCC.exe ---------------------------------------------------
ISCC=""
for candidate in \
    "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" \
    "C:/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "C:/Program Files/Inno Setup 6/ISCC.exe"
do
    if [ -f "$candidate" ]; then
        ISCC="$candidate"
        break
    fi
done

if [ -z "$ISCC" ]; then
    echo "SKIP: Inno Setup 6 (ISCC.exe) not found on this machine."
    exit 0
fi
echo "Using ISCC: $ISCC"

if [ ! -f "$ISS" ]; then
    echo "FAIL: installer script not found: $ISS"
    exit 1
fi

# --- stage sapi/output/ if it isn't already there -----------------------
if [ ! -f "$OUTPUT_DIR/DectalkDtc01SAPI.dll" ] || [ ! -f "$OUTPUT_DIR/x64/DectalkDtc01SAPI.dll" ]; then
    echo "output/ not fully staged -- running build_all.bat first"
    ( cd "$REPO_ROOT" && cmd //c "sapi\\build_all.bat" )
    if [ $? -ne 0 ]; then
        echo "FAIL: build_all.bat failed"
        exit 1
    fi
else
    echo "output/ already staged; skipping rebuild"
fi

if [ ! -f "$OUTPUT_DIR/DectalkDtc01SAPI.dll" ]; then
    echo "FAIL: $OUTPUT_DIR/DectalkDtc01SAPI.dll still missing after staging"
    exit 1
fi

# --- compile the installer -----------------------------------------------
rm -f "$SETUP_EXE"

"$ISCC" //Q "$SAPI_DIR/installer/DectalkDtc01SAPI.iss"
ISCC_RC=$?

if [ $ISCC_RC -ne 0 ]; then
    echo "FAIL: ISCC exited $ISCC_RC"
    exit 1
fi

if [ ! -f "$SETUP_EXE" ]; then
    echo "FAIL: ISCC reported success but $SETUP_EXE was not produced"
    exit 1
fi

echo "PASS: $SETUP_EXE produced ($(stat -c%s "$SETUP_EXE" 2>/dev/null || stat -f%z "$SETUP_EXE") bytes)"
exit 0
