"""Task E1 command-parity verifier.

Runs sapi/build_x64/Release/dump_pipeline.exe (the real C++ text_pipeline
primitives the SAPI engine calls) against a corpus of voices x wpm x
Design-Voice parameter sets x texts, and compares every output field
byte-for-byte against values computed straight from the Python source of
truth, addon/synthDrivers/dectalkDtc01/protocol/commands.py.

commands.py imports only `re`, so it loads standalone -- no NVDA, no addon
package init, nothing else on sys.path needed beyond its own directory.

IMPORTANT parity ruling (see the E1 task brief): the C++ dv_command emits up
to 9 Design-Voice params (adds laryngealization/assertiveness beyond the
7 the NVDA driver's _designVoiceCommand currently exposes -- a deliberate
C++ superset, not a bug). So the expected [:dv ...] string here is built
straight from commands.py's primitives (scale_from_default +
design_voice_command) applied to all 9 params in the C++ emit order, with
the skip-when-50 / empty-when-none-differ rule -- NOT by calling the
driver's own _designVoiceCommand, which only knows about 7 of them.

Usage: python sapi/tools/verify_pipeline.py [path/to/dump_pipeline.exe]
Exit code 0 iff every field of every case matches.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PROTOCOL_DIR = REPO_ROOT / "addon" / "synthDrivers" / "dectalkDtc01" / "protocol"
DEFAULT_EXE = REPO_ROOT / "sapi" / "build_x64" / "Release" / "dump_pipeline.exe"

sys.path.insert(0, str(PROTOCOL_DIR))
import commands  # noqa: E402  (must follow sys.path mutation) -- verifies it imports standalone


# ---------------------------------------------------------------------------
# Escaping -- must match dump_pipeline.cpp's escape_control() exactly, so
# fields with embedded control characters compare as single clean lines.
# ---------------------------------------------------------------------------

def escape_control(s: str) -> str:
    return s.replace("\r", "\\r").replace("\n", "\\n")


ASCII_WHITESPACE = " \t\r\n\f\v"


def rstrip_ascii_whitespace(s: str) -> str:
    return s.rstrip(ASCII_WHITESPACE)


# ---------------------------------------------------------------------------
# Expected-value builders, straight from commands.py.
# ---------------------------------------------------------------------------

# DvParams field name (dump_pipeline CLI / C++ struct) -> DTC-01 abbreviation.
FIELD_TO_ABBR = {
    "pitch": "ap",
    "inflection": "pr",
    "head_size": "hs",
    "breathiness": "br",
    "richness": "ri",
    "smoothness": "sm",
    "loudness": "g5",
    "laryngealization": "la",
    "assertiveness": "as",
}

# Abbreviation -> the DV_PARAMS (commands.py) full key design_voice_command()
# and clamp() expect.
ABBR_TO_DV_PARAM_NAME = {
    "ap": "averagePitch",
    "pr": "pitchRange",
    "hs": "headSize",
    "br": "breathiness",
    "ri": "richness",
    "sm": "smoothness",
    "g5": "loudness",
    "la": "laryngealization",
    "as": "assertiveness",
}

# C++ dv_command's emit order (text_pipeline.cpp's `tokens` array) -- this,
# not dict-literal order above, is what determines the order design_voice_command
# assembles its space-joined tokens in.
EMIT_ORDER_ABBR = ["ap", "pr", "hs", "br", "ri", "sm", "g5", "la", "as"]


def expected_rate(wpm: int) -> str:
    return commands.rate_command(wpm)


def expected_voice(voice_key: str) -> str:
    return commands.voice_command(voice_key)


def expected_dv(voice_key: str, params: dict[str, int]) -> str:
    """Mirrors the E1 ruling: skip sliders == 50, scale_from_default() the
    rest (voice_key, abbr, slider) in the C++ emit order, then hand the
    scaled values to design_voice_command() (which applies clamp() and the
    abbr mapping) -- NOT the driver's 7-param _designVoiceCommand."""
    kwargs: dict[str, float] = {}
    for abbr in EMIT_ORDER_ABBR:
        field = next(f for f, a in FIELD_TO_ABBR.items() if a == abbr)
        slider = params[field]
        if slider == 50:
            continue
        scaled = commands.scale_from_default(voice_key, abbr, slider)
        kwargs[ABBR_TO_DV_PARAM_NAME[abbr]] = scaled
    if not kwargs:
        return ""
    return commands.design_voice_command(**kwargs)


def expected_sanitize(text: str) -> str:
    return escape_control(commands.sanitize_text(text))


def expected_flush(text: str) -> str:
    """Mirrors __init__.py::_terminate() + _speakLine's unconditional "\\r"."""
    sanitized = commands.sanitize_text(text)
    stripped = rstrip_ascii_whitespace(sanitized)
    if not stripped:
        suffix = "\r"
    elif stripped[-1] in ".!?,":
        suffix = "\r"
    else:
        suffix = ",\r"
    return escape_control(suffix)


# ---------------------------------------------------------------------------
# Corpus.
# ---------------------------------------------------------------------------

# The 10 base voice keys: every key in dtc01::VOICES that also exists in
# commands.VOICE_PARAM_DEFAULTS. Firmware doesn't affect command strings, so
# each key is tested once (not crossed with firmware).
VOICE_KEYS = ["paul", "betty", "harry", "frank", "dennis", "kit", "rita", "ursula", "wendy", "val"]

# Proves the RATE_MIN_WPM/RATE_MAX_WPM=120/350 clamp matches on both sides.
WPM_VALUES = [100, 180, 350, 500]

_BASE_PARAMS = {
    "inflection": 50, "head_size": 50, "breathiness": 50, "richness": 50,
    "smoothness": 50, "loudness": 50, "laryngealization": 50,
    "assertiveness": 50, "pitch": 50,
}


def _params(**overrides) -> dict[str, int]:
    d = dict(_BASE_PARAMS)
    d.update(overrides)
    return d


PARAM_SETS: list[tuple[str, dict[str, int]]] = [("all_50", _params())]
# One non-50 param at a time, for every one of the 9 sliders (incl. la/as),
# BOTH below 50 and above 50 -- scale_from_default() takes a different
# branch on each side (below 50 interpolates towards the voice's own `lo`,
# above 50 towards its own `hi`), so a slider only ever tested on one side
# would leave that param's `hi` (or `lo`) bound unverified for every voice
# in the cross. This is what closes that coverage gap.
for _field in _BASE_PARAMS:
    PARAM_SETS.append((f"only_{_field}_25", _params(**{_field: 25})))
    PARAM_SETS.append((f"only_{_field}_75", _params(**{_field: 75})))
# Extremes (0 and 100) on a couple of params.
PARAM_SETS.append(("head_size_0", _params(head_size=0)))
PARAM_SETS.append(("head_size_100", _params(head_size=100)))
PARAM_SETS.append(("pitch_0", _params(pitch=0)))
PARAM_SETS.append(("pitch_100", _params(pitch=100)))
# Several non-50 params at once.
PARAM_SETS.append(("combo1", _params(inflection=10, head_size=90, loudness=5)))
PARAM_SETS.append(("combo2", _params(breathiness=100, richness=0, laryngealization=80,
                                      assertiveness=20, pitch=75)))

TEXTS: list[tuple[str, str]] = [
    ("plain", "This is a plain sentence"),
    ("brackets", "Text with [brackets] inside"),
    ("isolated_paren", "an isolated ( paren ) here"),
    ("word_paren", "grouped (word) text"),
    ("exclaim", "Ends with an exclamation!"),
    ("no_punct", "no ending punctuation"),
    ("trailing_space", "trailing space "),
    ("empty", ""),
    # Exercises the \n -> \\n and \r -> \\r escaping on the SANITIZE field
    # symmetrically -- previously only FLUSH's trailing "\r" hit that path.
    ("embedded_newline", "line one\nline two"),
    ("embedded_cr", "line one\rline two"),
]


# ---------------------------------------------------------------------------
# Runner.
# ---------------------------------------------------------------------------

def run_dump(exe: Path, voice_key: str, wpm: int, params: dict[str, int], text: str) -> dict[str, str]:
    args = [
        str(exe), voice_key, str(wpm),
        str(params["inflection"]), str(params["head_size"]), str(params["breathiness"]),
        str(params["richness"]), str(params["smoothness"]), str(params["loudness"]),
        str(params["laryngealization"]), str(params["assertiveness"]), str(params["pitch"]),
        text,
    ]
    proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(
            f"dump_pipeline exited {proc.returncode} for args {args!r}\n"
            f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
        )
    fields: dict[str, str] = {}
    for line in proc.stdout.split("\n"):
        line = line.rstrip("\r")
        if not line:
            continue
        name, _, value = line.partition("=")
        fields[name] = value
    return fields


def main() -> int:
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_EXE
    if not exe.exists():
        print(f"ERROR: dump_pipeline.exe not found at {exe}", file=sys.stderr)
        return 2

    total = 0
    failures = 0

    for voice_key in VOICE_KEYS:
        for wpm in WPM_VALUES:
            for param_label, params in PARAM_SETS:
                for text_label, text in TEXTS:
                    total += 1
                    case_desc = (f"voice={voice_key} wpm={wpm} params={param_label} "
                                 f"text={text_label}")
                    try:
                        actual = run_dump(exe, voice_key, wpm, params, text)
                    except RuntimeError as e:
                        print(f"CASE FAILED (crash): {case_desc}\n  {e}")
                        failures += 1
                        continue

                    expected = {
                        "RATE": expected_rate(wpm),
                        "VOICE": expected_voice(voice_key),
                        "DV": expected_dv(voice_key, params),
                        "SANITIZE": expected_sanitize(text),
                        "FLUSH": expected_flush(text),
                    }

                    case_ok = True
                    for field in ("RATE", "VOICE", "DV", "SANITIZE", "FLUSH"):
                        exp = expected[field]
                        act = actual.get(field)
                        if act != exp:
                            if case_ok:
                                print(f"CASE FAILED: {case_desc}")
                                case_ok = False
                            print(f"  field={field} expected={exp!r} actual={act!r}")
                    if not case_ok:
                        failures += 1

    print(f"\n{total} cases run, {total - failures} passed, {failures} failed.")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
