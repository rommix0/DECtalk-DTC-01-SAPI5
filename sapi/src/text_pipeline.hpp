// text_pipeline.hpp - the DECtalk DTC-01 in-line command language, ported
// verbatim from addon/synthDrivers/dectalkDtc01/protocol/commands.py (source
// of truth for every value and formula below) plus the firmware-flush rule
// from addon/synthDrivers/dectalkDtc01/__init__.py (_terminate / _speakLine).
//
// This header intentionally has no Python runtime to fall back on, so the
// per-voice Design Voice defaults (VOICE_PARAM_DEFAULTS) and parameter
// ranges (DV_PARAMS) are copied here as C++ tables. Keep them byte-for-byte
// in sync with commands.py -- a future task (E1) diffs the two outputs.
#pragma once

#include <string>

namespace dtc01 {

// Slider values 0..100; 50 means "this voice's own default", mirroring the
// NVDA settings ring (__init__.py::_designVoiceCommand). -1 is reserved for
// callers that want to mark a slider "unset" but is not treated specially
// here -- dv_command scales every field of DvParams, always emitting all
// nine DTC-01 parameters (see dv_command's comment below).
struct DvParams {
    int inflection = 50;       // -> pr (pitch range)
    int head_size = 50;        // -> hs
    int breathiness = 50;      // -> br
    int richness = 50;         // -> ri
    int smoothness = 50;       // -> sm
    int loudness = 50;         // -> g5
    int laryngealization = 50; // -> la
    int assertiveness = 50;    // -> as
    int pitch = 50;            // -> ap (average pitch)
};

// "[:ra <wpm>]", wpm clamped to [RATE_MIN_WPM, RATE_MAX_WPM] (commands.py
// rate_command / RATE_MIN_WPM=120 / RATE_MAX_WPM=350).
std::string rate_command(int wpm);

// "[:<mnemonic>]" -- mnemonic is already resolved (e.g. "np", "nh"); unlike
// commands.py's voice_command(name), which looks a friendly name up in
// VOICES and raises on an unknown one, this takes the mnemonic directly (the
// lookup lives in voices.hpp on the C++ side).
std::string voice_command(const char* mnemonic);

// "[:dv ap <v> pr <v> hs <v> br <v> ri <v> sm <v> g5 <v> la <v> as <v>]",
// mirroring commands.py's design_voice_command() fed by scale_from_default()
// for every one of DvParams' nine sliders (voice_key looked up in
// VOICE_PARAM_DEFAULTS, falling back to "paul" if unknown, exactly like
// commands.py's voice_param()). Always emits all nine tokens -- never "".
std::string dv_command(const std::string& voice_key, const DvParams& p);

// Square brackets are dropped (space-replaced, since the firmware ignores
// them but "[:" would otherwise be misparsed as a command) and isolated
// parentheses -- "(" or ")" not hugging a word -- are dropped too (spoken
// aloud by the firmware otherwise); "(word)" is left alone. Ports
// commands.py's sanitize_text() verbatim, regex for regex.
std::string sanitize_text(const std::string& utf8);

// The suffix to append to a sanitized, not-yet-terminated chunk so the
// firmware actually flushes it: a comma if the text doesn't already end in
// one of ".!?," (mirrors __init__.py's _terminate(), which appends "," --
// never "." -- to anything else), plus the "\r" every payload gets
// unconditionally (__init__.py::_speakLine). Trailing whitespace on
// `sanitized` is ignored when checking the last real character, matching
// _terminate()'s own rstrip(); the caller is responsible for stripping that
// trailing whitespace from `sanitized` itself before appending this suffix,
// since this function only returns the suffix, not the trimmed text.
std::string flush_suffix(const std::string& sanitized);

}  // namespace dtc01
