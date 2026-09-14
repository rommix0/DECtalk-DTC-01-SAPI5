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

#include <cstddef>
#include <string>
#include <vector>

namespace dtc01 {

// Slider values 0..100; 50 means "this voice's own default", mirroring the
// NVDA settings ring (__init__.py::_designVoiceCommand). A slider left at 50
// is omitted from dv_command's output entirely -- see that function's
// comment below.
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

// "[:dv ...]" for whichever of DvParams' nine sliders differ from 50 (this
// voice's own default), scaled via scale_from_default() (voice_key looked
// up in VOICE_PARAM_DEFAULTS, falling back to "paul" if unknown, exactly
// like commands.py's voice_param()); returns "" if every slider is 50.
//
// This mirrors __init__.py::_designVoiceCommand exactly (`if sliderValue ==
// 50: continue`, `if not params: return ""`), NOT commands.py's
// design_voice_command() in isolation, which will happily emit a token for
// whatever it's handed. Sending the full 9-token [:dv ...] prefix -- even
// with every value equal to the voice's own default -- was measured to
// break firmware synthesis outright (v2.0 truncates early, v1.8 runs to the
// line-length cap and never ends), so omitting default-valued params is not
// an optimization here, it's required for correct playback.
std::string dv_command(const std::string& voice_key, const DvParams& p);

// The nine Design Voice values the firmware holds for the current voice, in
// firmware units and dv_command's token order: ap, pr, hs, br, ri, sm, g5, la,
// as. The firmware keeps a [:dv] value until the voice is selected again, so
// an utterance whose pitch goes up and comes back down -- NVDA's capital
// letters -- has to send the way back too (DESIGN.md s24).
struct DvValues {
    int value[9];
};

// What [:n_] loads for this voice: its factory defaults, read from the ROM
// (VOICE_PARAM_DEFAULTS).
DvValues dv_defaults(const std::string& voice_key);

// The values DvParams' sliders ask for. A slider at 50, or one whose scaled
// value lands on the voice's own default, gives exactly that default -- even
// where [:dv] would not accept it: Kit's average pitch, 306, is above the 300
// the firmware allows, and sending 306 gets 300.
DvValues dv_values(const std::string& voice_key, const DvParams& p);

// "[:dv ...]" taking the firmware from `held` to `wanted`: a token for each
// value that differs, "" if none do. A value [:dv] would not accept can only
// be reached by selecting the voice again; if `wanted` needs one, it is left
// out and *needs_voice (when not null) is set, and the caller should send the
// voice command and ask again from dv_defaults(). From dv_defaults() this
// matches dv_command(), less any slider whose value is the default anyway.
std::string dv_change_command(const DvValues& held, const DvValues& wanted, bool* needs_voice);

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

// The firmware silently discards an input line longer than ~134 bytes, the
// command prefix and terminating ",\r" included, and holds only about two
// lines at once (DESIGN.md s19) -- so long text has to be split, and each
// piece spoken before the next is fed.
//
// One piece is [begin, end) in UTF-16 units of the text passed to
// split_for_firmware, and never begins or ends with whitespace.
struct TextPiece {
    size_t begin;
    size_t end;
};

// Splits `text` into pieces whose UTF-8 length is at most `first_budget`
// bytes for the first piece (which also carries the command prefix) and
// `budget` bytes for the rest. A piece ends after a sentence ender (.!?) by
// preference, then a clause mark (,;:), then any whitespace; a mark only
// counts when whitespace follows it, so "0.5.59", "1,234" and "U.S.A." are
// never split inside (DESIGN.md s22). A stretch with no whitespace that is
// longer than the budget is cut where the budget runs out. Whitespace-only
// text yields no pieces.
std::vector<TextPiece> split_for_firmware(const std::wstring& text, size_t first_budget,
                                          size_t budget);

}  // namespace dtc01
