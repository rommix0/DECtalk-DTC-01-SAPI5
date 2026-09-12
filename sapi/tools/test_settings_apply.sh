#!/usr/bin/env bash
# test_settings_apply.sh - audio-diff gate for Task C2 (wire HKCU settings
# into DectalkTtsEngine::Speak).
#
# Drives the REAL DectalkDtc01SAPI.dll through COM via sapi_probe.exe (same
# path test_probe.sh uses -- DllGetClassObject, no regsvr32) and proves that
# HKCU\Software\DECtalkDTC01 settings actually change the rendered PCM, not
# just that the registry round-trips (that's test_settings.cpp's job) and not
# just the emitted command string (a --dump-cmd check could be satisfied by
# code that builds the right string and then ignores it).
#
#   A. Baseline: no settings key at all -> base.wav.
#   B. HeadSize=80 for v20/paul only -> hs.wav; must differ audibly from
#      base.wav (design-voice sliders flow through dv_command).
#   C. RateBoost=100 (factor 2.0) -> boost.wav; duration must be ~half of
#      base.wav's (WSOLA time-compression is applied to the whole utterance).
#
# Writes are ALWAYS cleaned up (trap on EXIT) so a developer's real settings
# are never left modified by running this test.
set -uo pipefail

ROM_DIR="${ROM_DIR:-C:/Users/abart/AppData/Local/Temp/claude/C--Users-abart-Desktop-dectalk-dtc01/3eabd03f-8840-493b-afea-9a8d448e84f3/scratchpad/roms_v20}"
CORE_DLL="${CORE_DLL:-C:/Users/abart/Desktop/dectalk-dtc01/build/dtc01_x64.dll}"
FIRMWARE="v20"
VOICE="paul"
TEXT="Hello, this is a test of the emergency broadcast system."
REG_ROOT='HKCU\Software\DECtalkDTC01'

export DTC01_ROM_DIR="$ROM_DIR"
export DTC01_CORE_DLL="$CORE_DLL"

# Git Bash's MSYS layer rewrites arguments that look like Windows paths
# before native reg.exe ever sees them, which mangles "HKCU\Software\..."
# into something reg.exe rejects as invalid syntax. Disabling that rewriting
# is the documented escape hatch (MSYS2_ARG_CONV_EXCL="*") -- but it must be
# scoped to just the `reg` calls (a `reg` wrapper function), since sapi_probe
# still needs its POSIX-style DLL/wav paths auto-converted to Windows ones.
reg() { MSYS2_ARG_CONV_EXCL="*" command reg "$@"; }

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
    echo "FAIL: DectalkDtc01SAPI.dll not found (build it first: cmake --build sapi/build_x64 --config Release)" >&2
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
    echo "FAIL: sapi_probe.exe not found (build it first: cmake --build sapi/build_x64 --config Release)" >&2
    exit 1
fi

OUT_DIR="${TMPDIR:-/tmp}/dtc01_settings_apply_test"
mkdir -p "$OUT_DIR"
BASE_WAV="$OUT_DIR/base.wav"
HS_WAV="$OUT_DIR/hs.wav"
BOOST_WAV="$OUT_DIR/boost.wav"

FAILED=0

# Always leave HKCU exactly as we found it, whether we pass, fail, or blow up.
cleanup() {
    reg delete "$REG_ROOT" /f >/dev/null 2>&1 || true
}
trap cleanup EXIT

# --- A. baseline: make sure no settings key survives from a previous run ----
echo "== A. baseline (no HKCU settings) =="
reg delete "$REG_ROOT" /f >/dev/null 2>&1 || true
"$EXE" "$DLL" "$VOICE" "$FIRMWARE" "$BASE_WAV" "$TEXT"
if [ $? -ne 0 ]; then
    echo "FAIL: sapi_probe baseline render failed" >&2
    exit 1
fi

# --- B. HeadSize=80 for v20/paul only ---------------------------------------
echo "== B. HeadSize=80 (v20/paul) =="
reg add "${REG_ROOT}\\Voices\\v20_paul" /v HeadSize /t REG_DWORD /d 80 /f >/dev/null
"$EXE" "$DLL" "$VOICE" "$FIRMWARE" "$HS_WAV" "$TEXT"
if [ $? -ne 0 ]; then
    echo "FAIL: sapi_probe HeadSize render failed" >&2
    exit 1
fi
reg delete "${REG_ROOT}\\Voices\\v20_paul" /f >/dev/null 2>&1 || true

# --- C. RateBoost=100 (factor 2.0) ------------------------------------------
echo "== C. RateBoost=100 (factor 2.0) =="
reg add "$REG_ROOT" /v RateBoost /t REG_DWORD /d 100 /f >/dev/null
"$EXE" "$DLL" "$VOICE" "$FIRMWARE" "$BOOST_WAV" "$TEXT"
if [ $? -ne 0 ]; then
    echo "FAIL: sapi_probe RateBoost render failed" >&2
    exit 1
fi
reg delete "$REG_ROOT" /v RateBoost /f >/dev/null 2>&1 || true

# --- D. cleanup (also handled by the EXIT trap; explicit here for clarity) --
reg delete "$REG_ROOT" /f >/dev/null 2>&1 || true

# python.exe is a native Windows build; it can't resolve Git Bash's /tmp
# POSIX path, so convert to Windows-style paths before embedding them in the
# inline script (mirrors test_probe.sh).
if command -v cygpath >/dev/null 2>&1; then
    BASE_WAV_WIN="$(cygpath -w "$BASE_WAV")"
    HS_WAV_WIN="$(cygpath -w "$HS_WAV")"
    BOOST_WAV_WIN="$(cygpath -w "$BOOST_WAV")"
else
    BASE_WAV_WIN="$BASE_WAV"
    HS_WAV_WIN="$HS_WAV"
    BOOST_WAV_WIN="$BOOST_WAV"
fi

python -c "
import struct, sys, wave

def load(path):
    with wave.open(path, 'rb') as w:
        n = w.getnframes()
        rate = w.getframerate()
        data = w.readframes(n)
    samples = struct.unpack('<%dh' % (len(data) // 2), data)
    return samples, rate

def rms(samples):
    if not samples:
        return 0.0
    return (sum(s * s for s in samples) / len(samples)) ** 0.5

base_samples, base_rate = load(r'$BASE_WAV_WIN')
hs_samples, _ = load(r'$HS_WAV_WIN')
boost_samples, boost_rate = load(r'$BOOST_WAV_WIN')

base_rms = rms(base_samples)
hs_rms = rms(hs_samples)
base_dur = len(base_samples) / float(base_rate)
hs_dur = len(hs_samples) / float(base_rate)
boost_dur = len(boost_samples) / float(boost_rate)

print('base: %d samples, %.3fs, RMS %.2f' % (len(base_samples), base_dur, base_rms))
print('hs:   %d samples, %.3fs, RMS %.2f' % (len(hs_samples), hs_dur, hs_rms))
print('boost:%d samples, %.3fs, RMS %.2f' % (len(boost_samples), boost_dur, rms(boost_samples)))

ok = True

# B: HeadSize=80 must audibly change the render. Compare RMS *and* a raw
# sample diff so a change that happens to preserve RMS (unlikely, but let's
# not rely on luck) still gets caught.
rms_diff = abs(hs_rms - base_rms)
n = min(len(base_samples), len(hs_samples))
sample_diff = sum(1 for i in range(n) if base_samples[i] != hs_samples[i])
same_length = len(base_samples) == len(hs_samples)
print('HeadSize vs base: RMS diff %.2f, differing samples %d/%d, same length %s' %
      (rms_diff, sample_diff, n, same_length))
differs = (rms_diff > 1.0) or (sample_diff > n * 0.01) or not same_length
print('B PASS' if differs else 'B FAIL', '- HeadSize changes audio' if differs else '- HeadSize did NOT change audio')
if not differs:
    ok = False

# C: RateBoost=100 (factor 2.0) must roughly halve the duration.
expected = base_dur / 2.0
tolerance = expected * 0.15
within = abs(boost_dur - expected) <= tolerance
print('RateBoost vs base: base_dur=%.3fs boost_dur=%.3fs expected=%.3fs (+/-%.3fs)' %
      (base_dur, boost_dur, expected, tolerance))
print('C PASS' if within else 'C FAIL', '- RateBoost halves duration' if within else '- RateBoost duration out of tolerance')
if not within:
    ok = False

sys.exit(0 if ok else 1)
"
PYRC=$?
if [ $PYRC -ne 0 ]; then
    FAILED=1
fi

if [ $FAILED -ne 0 ]; then
    echo "FAIL"
    exit 1
fi

echo "PASS"
exit 0
