"""DECtalk DTC-01 v2.0 in-line command language: builds the exact ASCII
command tokens the firmware's serial parser expects. All syntax and ranges
below are verified against the actual 1984 DTC-01 v2.0 Owner's Manual
(EK-DTC01-OM-002), not the later "DECtalk Software" SDK docs -- see
DESIGN.md sections 6 and 6b for sourcing and caveats.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

RATE_MIN_WPM = 120
RATE_MAX_WPM = 350
RATE_DEFAULT_WPM = 180

VOICES = {
	"paul": "np",  # Perfect Paul -- standard male
	"betty": "nb",  # Beautiful Betty -- standard female
	"harry": "nh",  # Huge Harry -- deep male
	"frank": "nf",  # Frail Frank -- older male
	"dennis": "nd",  # Doctor Dennis -- v2.0 only (see V20_ONLY_VOICES)
	"kit": "nk",  # Kit the Kid -- child's voice (10yo)
	"rita": "nr",  # Rough Rita -- deep female
	"ursula": "nu",  # Uppity Ursula -- light female
	"wendy": "nw",  # Whispery Wendy -- v2.0 only (see V20_ONLY_VOICES)
	"val": "nv",  # Variable Val -- user-definable (last [:dv ... save])
}

# Doctor Dennis and Whispery Wendy exist only in the v2.0 firmware. The v1.8
# ROM contains none of the ten voice-name strings, and feeding it [:nd] or
# [:nw] produces output indistinguishable from Paul -- an unrecognised code
# leaves the current voice in place -- while [:nb] stays clearly distinct, so
# v1.8 does have voices, just not these two (DESIGN.md §6b, measured).
#
# Ordering above follows the ROM's own table at main-CPU 0x179AA, which is
# also the manual's order.
V20_ONLY_VOICES = frozenset({"dennis", "wendy"})


def voices_for(firmware: str) -> dict[str, str]:
	"""The voice table a given firmware actually implements."""
	if firmware == "v18":
		return {k: v for k, v in VOICES.items() if k not in V20_ONLY_VOICES}
	return dict(VOICES)


@dataclass(frozen=True)
class DVParam:
	abbr: str
	label: str
	minimum: float
	maximum: float
	unit: str


# Table 5-3, DESIGN.md section 6, with the ranges CORRECTED against the
# ROM (see VOICE_PARAM_DEFAULTS below and DESIGN.md section 15). These
# bounds are enforced by clamp(), so a stale range here silently
# rewrites commands -- the manual's head-size range of 75..150 was
# clamping a requested 'hs 40' up to 'hs 75'. `sex` is handled separately (voice select
# already implies a sex; exposing it directly lets a user cross a voice with
# the opposite sex's formant characteristics, which the manual documents as
# a supported, if occasionally overload-prone, customization).
DV_PARAMS: dict[str, DVParam] = {
	"averagePitch": DVParam("ap", "Average pitch", 30, 300, "Hz"),
	"assertiveness": DVParam("as", "Assertiveness", 0, 100, "%"),
	"formant4Bandwidth": DVParam("b4", "4th formant bandwidth", 100, 2048, "Hz"),
	"formant5Bandwidth": DVParam("b5", "5th formant bandwidth", 100, 2048, "Hz"),
	"baselineFallBegin": DVParam("bf", "Beginning pitch baseline fall", 50, 200, "Hz"),
	"breathiness": DVParam("br", "Breathiness", 0, 72, "dB"),
	"baselineFallEnd": DVParam("ef", "End pitch baseline fall", 50, 200, "Hz"),
	"forte": DVParam("fo", "Forte voice", 0, 100, "%"),
	"spectralTilt": DVParam("ft", "F0-dependent spectral tilt", 0, 100, "%"),
	"gain1": DVParam("g1", "Synthesizer gain 1", 0, 80, "dB"),
	"gain2": DVParam("g2", "Synthesizer gain 2", 0, 80, "dB"),
	"gain3": DVParam("g3", "Synthesizer gain 3", 0, 80, "dB"),
	"gain4": DVParam("g4", "Synthesizer gain 4", 0, 80, "dB"),
	"gain5": DVParam("g5", "Synthesizer gain 5", 0, 80, "dB"),
	"fricationGain": DVParam("gf", "Gain of frication source", 0, 80, "dB"),
	"aspirationGain": DVParam("gh", "Gain of aspiration source", 0, 80, "dB"),
	"nasalGain": DVParam("gn", "Gain of nasal resonator", 0, 80, "dB"),
	"voicingGain": DVParam("gv", "Gain of voicing source", 0, 72, "dB"),
	"headSize": DVParam("hs", "Head size", 40, 200, "%"),
	"laryngealization": DVParam("la", "Laryngealization", 0, 100, "%"),
	"glottalOpenSamples": DVParam("nf", "Samples in glottal pulse open phase", 0, 60, ""),
	"pitchRange": DVParam("pr", "Pitch range", 0, 250, "%"),
	"richness": DVParam("ri", "Richness", 0, 100, "%"),
	"smoothness": DVParam("sm", "Smoothness", 0, 100, "%"),
	# The ROM's own master level control: "[:dv listall]" labels g5
	# "Loudness (gain of resonator 5)". Applied inside the synthesis
	# chain, so unlike gain on the finished audio it can prevent the
	# clipping that extreme head-size settings provoke.
	"loudness": DVParam("g5", "Loudness", 0, 80, "dB"),
}


# Per-voice Design Voice defaults and legal ranges, queried from the ROM
# itself via "[:dv listall]" (see tools/dump_voice_defaults.py). Each entry
# is (default, min, max).
#
# These supersede the ranges in the DV_PARAMS table above, which came from
# an OCR of the manual and are wrong for several parameters -- the ROM
# reports smoothness as 0..100 %, not 0..24 dB; breathiness 0..72, not
# 0..60; head size 40..200, not 75..150.
#
# They matter because a voice's default is often nowhere near the middle of
# its range: Huge Harry's average pitch is 78Hz within a 30..300 band, and
# Kit's is 306 -- above the reported maximum. A slider mapped linearly onto
# the absolute range therefore jumps wildly around its midpoint, so the
# driver maps 50% to *this voice's* default and scales outward from there.
VOICE_PARAM_DEFAULTS = {
	"paul": {"g5": (72, 0, 80), "ap": (120, 30, 300), "pr": (100, 0, 250), "hs": (100, 40, 200), "br": (0, 0, 72), "ri": (80, 0, 100), "sm": (54, 0, 100), "la": (0, 0, 100), "as": (100, 0, 100)},
	"betty": {"g5": (68, 0, 80), "ap": (180, 30, 300), "pr": (160, 0, 250), "hs": (100, 40, 200), "br": (46, 0, 72), "ri": (0, 0, 100), "sm": (44, 0, 100), "la": (0, 0, 100), "as": (65, 0, 100)},
	"harry": {"g5": (69, 0, 80), "ap": (78, 30, 300), "pr": (50, 0, 250), "hs": (120, 40, 200), "br": (0, 0, 72), "ri": (86, 0, 100), "sm": (34, 0, 100), "la": (0, 0, 100), "as": (100, 0, 100)},
	"frank": {"g5": (74, 0, 80), "ap": (153, 30, 300), "pr": (90, 0, 250), "hs": (90, 40, 200), "br": (50, 0, 72), "ri": (80, 0, 100), "sm": (36, 0, 100), "la": (12, 0, 100), "as": (65, 0, 100)},
	"dennis": {"g5": (74, 0, 80), "ap": (100, 30, 300), "pr": (135, 0, 250), "hs": (105, 40, 200), "br": (62, 0, 72), "ri": (0, 0, 100), "sm": (100, 0, 100), "la": (0, 0, 100), "as": (100, 0, 100)},
	"kit": {"g5": (62, 0, 80), "ap": (306, 30, 300), "pr": (180, 0, 250), "hs": (80, 40, 200), "br": (40, 0, 72), "ri": (40, 0, 100), "sm": (44, 0, 100), "la": (0, 0, 100), "as": (65, 0, 100)},
	"rita": {"g5": (72, 0, 80), "ap": (106, 30, 300), "pr": (80, 0, 250), "hs": (95, 40, 200), "br": (49, 0, 72), "ri": (0, 0, 100), "sm": (34, 0, 100), "la": (4, 0, 100), "as": (65, 0, 100)},
	"ursula": {"g5": (69, 0, 80), "ap": (264, 30, 300), "pr": (135, 0, 250), "hs": (95, 40, 200), "br": (0, 0, 72), "ri": (100, 0, 100), "sm": (64, 0, 100), "la": (0, 0, 100), "as": (100, 0, 100)},
	"wendy": {"g5": (80, 0, 80), "ap": (264, 30, 300), "pr": (135, 0, 250), "hs": (95, 40, 200), "br": (58, 0, 72), "ri": (0, 0, 100), "sm": (100, 0, 100), "la": (0, 0, 100), "as": (100, 0, 100)},
	"val": {"g5": (72, 0, 80), "ap": (120, 30, 300), "pr": (100, 0, 250), "hs": (100, 40, 200), "br": (0, 0, 72), "ri": (80, 0, 100), "sm": (54, 0, 100), "la": (0, 0, 100), "as": (100, 0, 100)},
}


def voice_param(voice: str, param: str) -> tuple[int, int, int]:
	"""(default, min, max) for a parameter on a voice. The ROM's own default
	can sit outside its reported range (Kit's average pitch), so the bounds
	are widened to include it rather than clamping the voice's own value."""
	entry = VOICE_PARAM_DEFAULTS.get(voice) or VOICE_PARAM_DEFAULTS["paul"]
	default, lo, hi = entry[param]
	return default, min(lo, default), max(hi, default)


def scale_from_default(voice: str, param: str, slider: int) -> int:
	"""Map NVDA's 0-100 slider onto a parameter, with 50 = this voice's
	default. Piecewise linear so the midpoint is continuous -- the previous
	absolute mapping made 49 and 51 land far apart on voices whose default
	is off-centre."""
	default, lo, hi = voice_param(voice, param)
	slider = max(0, min(100, int(slider)))
	if slider == 50:
		return default
	if slider < 50:
		return int(round(lo + (default - lo) * (slider / 50.0)))
	return int(round(default + (hi - default) * ((slider - 50) / 50.0)))


def clamp(name: str, value: float) -> int:
	p = DV_PARAMS[name]
	return int(round(min(max(value, p.minimum), p.maximum)))


def rate_command(words_per_minute: int) -> str:
	wpm = min(max(int(round(words_per_minute)), RATE_MIN_WPM), RATE_MAX_WPM)
	return f"[:ra {wpm}]"


def pause_command(comma_ms: int | None = None, period_ms: int | None = None) -> str:
	parts = []
	if comma_ms is not None:
		parts.append(f":cp {int(comma_ms)}")
	if period_ms is not None:
		parts.append(f":pp {int(period_ms)}")
	return f"[{' '.join(parts)}]" if parts else ""


def voice_command(name: str) -> str:
	mnemonic = VOICES.get(name.lower())
	if mnemonic is None:
		raise ValueError(f"Unknown DTC-01 voice {name!r}; valid: {sorted(VOICES)}")
	return f"[:{mnemonic}]"


def sex_command(male: bool) -> str:
	return f"[:dv sex {'m' if male else 'f'}]"


def design_voice_command(**params: float) -> str:
	"""Build a [:dv ...] command from NVDA-facing parameter names (the keys
	of DV_PARAMS) mapped to their DTC-01 abbreviation and clamped range.
	Unknown keys raise -- callers should only pass names from DV_PARAMS."""
	tokens = []
	for name, value in params.items():
		if name not in DV_PARAMS:
			raise ValueError(f"Unknown DTC-01 voice parameter {name!r}")
		p = DV_PARAMS[name]
		tokens.append(f"{p.abbr} {clamp(name, value)}")
	if not tokens:
		return ""
	return "[:dv " + " ".join(tokens) + "]"


_ISOLATED_OPEN_PAREN = re.compile(r"\((?=\s|$)")
# Written as a group rather than a look-behind: "(?<=\s|^)" is variable
# width, which re rejects.
_ISOLATED_CLOSE_PAREN = re.compile(r"(^|\s)\)")


def sanitize_text(text: str) -> str:
	"""Make text safe to embed in a DTC-01 command stream, and stop the
	firmware announcing punctuation NVDA has already decided to keep silent.

	Two firmware behaviours drive this, both measured against the real ROM:

	* Square brackets are **ignored entirely** by the synthesiser. They must
	  still be removed, because "[:" opens a command sequence and text
	  containing brackets (code, markdown) could otherwise be misparsed as
	  synthesiser commands. Replacing them with a space reproduces exactly
	  what the hardware does with them.
	* An **isolated** parenthesis is *spoken aloud* as "left parenthesis" /
	  "right parenthesis" (+1.4s on a test phrase), while a parenthesis
	  wrapped around a word is just a phrasing pause. NVDA has already
	  applied the user's punctuation-level preference before the text
	  reaches us, so the synthesiser announcing punctuation on its own gives
	  readings no other synth produces. Isolated ones are dropped; "(word)"
	  is left alone so grouping and number handling still work.

	An earlier version substituted "(" for "[", which was the worst possible
	choice here: it turned a character the firmware ignores into one it reads
	out loud.
	"""
	text = text.replace("[", " ").replace("]", " ")
	text = _ISOLATED_OPEN_PAREN.sub(" ", text)
	text = _ISOLATED_CLOSE_PAREN.sub(r"\1 ", text)
	return text
