"""verify_audio.py - Task E3: audio-identity + boot-announcement verification.

Proves two things about the SAPI engine's rendered audio, per voice:

  1. Audio identity: sapi_probe.exe's C++ SAPI render matches an independent
     reference render of the *identical command bytes* through the Python
     oracle (emu.native.NativeMachine, which drives the SAME native core),
     within the statistical tolerance tools/compare_native.py uses (envelope
     + RMS, NOT bit-exact -- see that module's docstring for why bit-exact
     isn't the right bar even when both sides drive the same core).
  2. No boot announcement: the first SAPI utterance contains no power-on
     "DECtalk ... is running" announcement -- verified by the render's
     speech envelope matching the boot-free oracle (a leaked announcement
     would add ~1-2s of extra speech at the start: an earlier first-active
     block and/or many more active blocks than the oracle has).

The envelope()/rms() helpers and their pass/fail thresholds below are
copied verbatim from tools/compare_native.py; do not loosen them.

Usage:
    verify_audio.py <SAPI-engine.dll> <sapi_probe.exe> <dump_pipeline.exe> \
                     <rom_dir> <core_dll> [text]

<rom_dir> must be a FLAT directory holding BOTH firmwares' ROM chips (see
rom_loader.py: files are identified by content hash, not name, so v20 and
v18 chips can coexist in one directory). <core_dll> is dtc01_x64.dll, handed
straight to emu.native.NativeMachine; <SAPI-engine.dll> is
DectalkDtc01SAPI.dll, handed to sapi_probe.exe (which resolves its own ROM
dir / core DLL from the DTC01_ROM_DIR / DTC01_CORE_DLL environment variables
-- the caller, test_audio_identity.sh, sets both before running this).

Exit 0 iff every test voice passes every criterion; non-zero otherwise,
naming the first failure.
"""
from __future__ import annotations

import subprocess
import struct
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "addon" / "synthDrivers" / "dectalkDtc01"))

from emu.native import NativeMachine  # noqa: E402

SAMPLE_RATE = 10000
DEFAULT_WPM = 180  # DectalkTtsEngine.cpp's DEFAULT_WPM; what SAPI RateAdj=0 feeds.

TEST_VOICES = [("paul", "v20"), ("harry", "v20"), ("betty", "v18")]
TEXT = "The quick brown fox jumps over the lazy dog."


# ---------------------------------------------------------------------------
# envelope()/rms() -- copied verbatim from tools/compare_native.py, along
# with its tolerance logic (first active block within 2, last within 3, RMS
# ratio 0.8-1.25). Do not loosen these.
# ---------------------------------------------------------------------------

def envelope(samples: list[int], block: int = 1000, thresh: int = 200):
    """Return list of (block_index, peak) for blocks with real signal, plus
    the first/last active block -- i.e. where speech actually happens."""
    active = []
    for i in range(0, len(samples), block):
        chunk = samples[i:i + block]
        peak = max((abs(x) for x in chunk), default=0)
        if peak > thresh:
            active.append((i // block, peak))
    first = active[0][0] if active else None
    last = active[-1][0] if active else None
    return active, first, last


def rms(samples: list[int]) -> float:
    if not samples:
        return 0.0
    return (sum(x * x for x in samples) / len(samples)) ** 0.5


def is_flat(samples) -> bool:
    """True if every sample is identical -- the DAC holding its last value
    between utterances (real hardware behaviour), not speech. Mirrors
    NativeMachine.is_flat / dtc01_core.cpp's own flat check."""
    return len(samples) == 0 or min(samples) == max(samples)


# ---------------------------------------------------------------------------
# Oracle byte reconstruction: exactly what DectalkTtsEngine::Speak() feeds
# the core for a fresh utterance (sapi/src/DectalkTtsEngine.cpp, ~line
# 545-554):
#
#     std::string fed;
#     if (current_mnemonic != mnemonic_) {       // always true for the very
#         fed += dtc01::voice_command(...);      // first fragment of a fresh
#         fed += " ";                            // Speak() call --
#         current_mnemonic = mnemonic_;          // current_mnemonic starts
#     }                                           // empty
#     fed += dtc01::rate_command(wpm);
#     fed += dtc01::dv_command(voice_key_, dvp);
#     fed += sanitized;                          // sanitize_text(), THEN
#                                                 // rstrip()'d in place
#     fed += dtc01::flush_suffix(sanitized);      // of that same rstripped
#                                                 // string
#
# dump_pipeline's SANITIZE field is deliberately the PRE-rstrip
# sanitize_text() output (it exists to verify sanitize_text() in isolation --
# Task E1); it must be rstripped here (ASCII whitespace, matching
# DectalkTtsEngine.cpp's own rstrip()) to match what Speak() actually
# appends. The FLUSH field is already correct as-is: dump_pipeline computes
# it from that same rstripped ("trimmed") string.
# ---------------------------------------------------------------------------

ASCII_WHITESPACE = " \t\r\n\f\v"


def _unescape(s: str) -> str:
    """Undoes dump_pipeline.cpp's escape_control(): \\r -> real CR, \\n -> real LF."""
    return s.replace("\\r", "\r").replace("\\n", "\n")


def _rstrip_ascii(s: str) -> str:
    return s.rstrip(ASCII_WHITESPACE)


def run_dump_pipeline(dump_pipeline_exe: Path, voice_key: str, text: str) -> dict[str, str]:
    # All 9 sliders at 50 -> empty DV (this voice's own defaults); wpm 180 ==
    # DEFAULT_WPM, what SAPI RateAdj=0 feeds.
    args = [str(dump_pipeline_exe), voice_key, str(DEFAULT_WPM),
             "50", "50", "50", "50", "50", "50", "50", "50", "50", text]
    proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(
            f"dump_pipeline failed for voice={voice_key!r}: rc={proc.returncode}\n"
            f"stdout={proc.stdout!r}\nstderr={proc.stderr!r}")
    fields: dict[str, str] = {}
    for line in proc.stdout.split("\n"):
        line = line.rstrip("\r")
        if not line:
            continue
        name, _, value = line.partition("=")
        fields[name] = value
    for required in ("VOICE", "RATE", "DV", "SANITIZE", "FLUSH"):
        if required not in fields:
            raise RuntimeError(f"dump_pipeline output missing {required}= field: {proc.stdout!r}")
    return fields


def build_fed_bytes(fields: dict[str, str]) -> bytes:
    voice = _unescape(fields["VOICE"])
    rate = _unescape(fields["RATE"])
    dv = _unescape(fields["DV"])
    sanitized = _rstrip_ascii(_unescape(fields["SANITIZE"]))
    flush = _unescape(fields["FLUSH"])
    fed = voice + " " + rate + dv + sanitized + flush
    return fed.encode("ascii")


# ---------------------------------------------------------------------------
# Oracle boot drain -- mirrors dtc01_core.cpp's Machine::consume_boot_
# announcement() block-for-block (same 0.2s chunk size, same silence
# threshold and block-count constants), so the oracle reaches exactly the
# same boot-free state ensure_machine() leaves the real engine's Machine in.
# A naive "run one 2000-sample block, check is_idle()" loop is NOT safe here:
# right after reset the core is briefly idle *before* the announcement's
# speech starts, and a check that doesn't first wait for actual speech would
# declare "done" immediately, leaving the announcement undrained -- exactly
# the two-phase guard the real C++ implementation exists for.
# ---------------------------------------------------------------------------

BOOT_CHUNK_SAMPLES = 2000          # 0.2s @ 10kHz, matches kBootChunkSamples
BOOT_GIVEUP_BLOCKS = 30            # ~6s waiting for speech to start
BOOT_MAX_BLOCKS = 125              # ~25s hard ceiling regardless
BOOT_SILENCE_BLOCKS_NEEDED = 4     # ~0.8s of sustained silence after speech
BOOT_SILENCE_THRESHOLD = 120       # matches native.py's SILENCE_THRESHOLD


def drain_boot_announcement(machine: NativeMachine) -> None:
    heard = False
    quiet_blocks = 0
    lead_in_blocks = 0
    for _ in range(BOOT_MAX_BLOCKS):
        samples = machine.run_samples(BOOT_CHUNK_SAMPLES)
        if not samples:
            return  # stalled; nothing more will come
        peak = max(abs(s) for s in samples)
        speech = peak >= BOOT_SILENCE_THRESHOLD and not is_flat(samples)
        if speech:
            heard = True
            quiet_blocks = 0
            continue
        if not heard:
            lead_in_blocks += 1
            if lead_in_blocks >= BOOT_GIVEUP_BLOCKS and machine.is_idle:
                return  # nothing to announce (or it never started)
            continue
        if machine.is_idle:
            quiet_blocks += 1
            if quiet_blocks >= BOOT_SILENCE_BLOCKS_NEEDED:
                return  # sustained silence after speech: announcement is over
        else:
            quiet_blocks = 0
    # Hit the hard ceiling -- give up regardless, matching the C++ side.


# ---------------------------------------------------------------------------
# Speech collection -- mirrors DectalkTtsEngine::Speak()'s own per-fragment
# pump loop (sapi/src/DectalkTtsEngine.cpp ~line 583-636): same 0.1s block
# size, same "idle only ends the utterance once speech has actually
# started" guard (v1.8's longer letter-to-sound lead-in can look
# momentarily idle before any real audio comes out -- ending on idle there
# would truncate the utterance), same idle_runs>3 hysteresis. A capped
# byte count (well under the engine's own 120s per-fragment safety cap)
# guards against a stall hanging the test.
# ---------------------------------------------------------------------------

SPEAK_BLOCK_SAMPLES = 1000          # 0.1s @ 10kHz, matches kBlockSamples
SPEAK_CAP_SAMPLES = 200_000         # 20s -- generous for one short sentence
SPEECH_PEAK_THRESHOLD = 256         # matches the engine's speech_started check


def collect_speech(machine: NativeMachine, fed: bytes) -> list[int]:
    machine.feed_text(fed)
    out: list[int] = []
    speech_started = False
    idle_runs = 0
    produced = 0
    while produced < SPEAK_CAP_SAMPLES:
        block = machine.run_samples(SPEAK_BLOCK_SAMPLES)
        if not block:
            break
        out.extend(block)
        produced += len(block)
        if not speech_started:
            peak = max(abs(s) for s in block)
            if peak > SPEECH_PEAK_THRESHOLD and not is_flat(block):
                speech_started = True
        if speech_started and machine.is_idle:
            idle_runs += 1
            if idle_runs > 3:
                break
        else:
            idle_runs = 0
    return out


# ---------------------------------------------------------------------------
# SAPI render (separate process -- sapi_probe.exe loads DectalkDtc01SAPI.dll
# and its own core DLL via COM; NativeMachine loads the core DLL directly in
# THIS process via ctypes. Never load the core twice in one process.)
# ---------------------------------------------------------------------------

def run_sapi_probe(sapi_probe_exe: Path, engine_dll: Path, voice_key: str,
                    firmware: str, text: str, out_wav: Path) -> list[int]:
    args = [str(sapi_probe_exe), str(engine_dll), voice_key, firmware, str(out_wav), text]
    proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(
            f"sapi_probe failed for voice={voice_key!r} firmware={firmware!r}: "
            f"rc={proc.returncode}\nstdout={proc.stdout!r}\nstderr={proc.stderr!r}")
    with wave.open(str(out_wav), "rb") as w:
        params = w.getparams()
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (1, 2, SAMPLE_RATE):
            raise RuntimeError(f"sapi_probe wrote an unexpected wav format: {params}")
        n = w.getnframes()
        data = w.readframes(n)
    return list(struct.unpack("<%dh" % (len(data) // 2), data))


# ---------------------------------------------------------------------------
# Per-voice check.
# ---------------------------------------------------------------------------

def check_voice(voice_key: str, firmware: str, dump_pipeline_exe: Path,
                 sapi_probe_exe: Path, engine_dll: Path, rom_dir: str, core_dll: str,
                 tmp_dir: Path, text: str) -> tuple[bool, str]:
    lines = [f"== {voice_key}/{firmware} =="]

    fields = run_dump_pipeline(dump_pipeline_exe, voice_key, text)
    fed = build_fed_bytes(fields)
    lines.append(f"  fed bytes: {fed!r}")

    machine = NativeMachine(rom_dir, core_dll, rom_version=firmware)
    try:
        drain_boot_announcement(machine)
        oracle_samples = collect_speech(machine, fed)
    finally:
        machine.close()

    out_wav = tmp_dir / f"{voice_key}_{firmware}.wav"
    sapi_samples = run_sapi_probe(sapi_probe_exe, engine_dll, voice_key, firmware, text, out_wav)

    o_act, o_first, o_last = envelope(oracle_samples)
    s_act, s_first, s_last = envelope(sapi_samples)
    o_rms = rms(oracle_samples)
    s_rms = rms(sapi_samples)

    lines.append(f"  oracle: {len(oracle_samples)} samples, {len(o_act)} active blocks, "
                 f"first={o_first} last={o_last} rms={o_rms:.1f}")
    lines.append(f"  sapi:   {len(sapi_samples)} samples, {len(s_act)} active blocks, "
                 f"first={s_first} last={s_last} rms={s_rms:.1f}")

    if o_first is None or s_first is None:
        lines.append("  FAIL: one side produced no speech at all")
        return False, "\n".join(lines)

    ok = True

    first_ok = abs(s_first - o_first) <= 2
    last_ok = abs(s_last - o_last) <= 3
    lines.append(f"  envelope first: |{s_first}-{o_first}|={abs(s_first - o_first)} <=2 "
                 f"-> {'PASS' if first_ok else 'FAIL'}")
    lines.append(f"  envelope last:  |{s_last}-{o_last}|={abs(s_last - o_last)} <=3 "
                 f"-> {'PASS' if last_ok else 'FAIL'}")
    ok = ok and first_ok and last_ok

    ratio = (s_rms / o_rms) if o_rms else 0.0
    ratio_ok = 0.8 <= ratio <= 1.25
    lines.append(f"  rms ratio sapi/oracle = {ratio:.3f} -> {'PASS' if ratio_ok else 'FAIL'}")
    ok = ok and ratio_ok

    # Boot-announcement guard (explicit): a leaked boot announcement would
    # add many active 100ms blocks at the start and pull the first active
    # block materially earlier -- check both.
    boot_first_ok = s_first <= o_first + 2
    boot_count_ok = len(s_act) <= len(o_act) + 3
    lines.append(f"  boot guard (first not earlier): sapi_first={s_first} <= "
                 f"oracle_first+2={o_first + 2} -> {'PASS' if boot_first_ok else 'FAIL'}")
    lines.append(f"  boot guard (block count):       sapi_blocks={len(s_act)} <= "
                 f"oracle_blocks+3={len(o_act) + 3} -> {'PASS' if boot_count_ok else 'FAIL'}")
    ok = ok and boot_first_ok and boot_count_ok

    lines.append(f"  RESULT: {'PASS' if ok else 'FAIL'}")
    return ok, "\n".join(lines)


def main() -> int:
    if len(sys.argv) < 6:
        print("usage: verify_audio.py <SAPI-engine.dll> <sapi_probe.exe> "
              "<dump_pipeline.exe> <rom_dir> <core_dll> [text]", file=sys.stderr)
        return 2

    engine_dll = Path(sys.argv[1])
    sapi_probe_exe = Path(sys.argv[2])
    dump_pipeline_exe = Path(sys.argv[3])
    rom_dir = sys.argv[4]
    core_dll = sys.argv[5]
    text = sys.argv[6] if len(sys.argv) > 6 else TEXT

    for label, p in (("engine dll", engine_dll), ("sapi_probe.exe", sapi_probe_exe),
                      ("dump_pipeline.exe", dump_pipeline_exe)):
        if not p.is_file():
            print(f"ERROR: {label} not found: {p}", file=sys.stderr)
            return 2

    tmp_dir = Path(tempfile.mkdtemp(prefix="dtc01_verify_audio_"))

    overall_ok = True
    first_failure = None
    for voice_key, firmware in TEST_VOICES:
        try:
            ok, report = check_voice(voice_key, firmware, dump_pipeline_exe, sapi_probe_exe,
                                      engine_dll, rom_dir, core_dll, tmp_dir, text)
        except Exception as e:  # noqa: BLE001 -- report and keep going
            ok = False
            report = f"== {voice_key}/{firmware} ==\n  ERROR: {e}"
        print(report)
        print()
        if not ok and overall_ok:
            overall_ok = False
            first_failure = f"{voice_key}/{firmware}"

    print("=" * 62)
    if overall_ok:
        print("RESULT: PASS")
    else:
        print(f"RESULT: FAIL (first failure: {first_failure})")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
