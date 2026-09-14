// text_pipeline.cpp - see text_pipeline.hpp. Every table and formula here is
// ported verbatim from addon/synthDrivers/dectalkDtc01/protocol/commands.py;
// keep the two in sync (a later task diffs their outputs byte-for-byte).
#include "text_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <regex>

namespace dtc01 {

namespace {

constexpr int RATE_MIN_WPM = 120;
constexpr int RATE_MAX_WPM = 350;

// Python's round() is round-half-to-even ("banker's rounding"), unlike
// C++'s std::round (round-half-away-from-zero). commands.py relies on
// round() throughout (scale_from_default, clamp), so a value landing exactly
// on .5 would come out differently under std::round. Every input here is
// non-negative, so this only needs to handle that case.
long long py_round(double x) {
    double floor_val = std::floor(x);
    double diff = x - floor_val;
    long long fl = static_cast<long long>(floor_val);
    if (diff < 0.5) return fl;
    if (diff > 0.5) return fl + 1;
    return (fl % 2 == 0) ? fl : fl + 1;  // exactly .5: round to even
}

// DV_PARAMS (commands.py), restricted to the nine abbreviations DvParams
// exposes. Keyed by abbreviation here (rather than commands.py's full
// descriptive name) since that's the only thing clamp() actually needs the
// name for: looking up this same (min, max) pair.
struct ClampRange { double lo, hi; };

const ClampRange& clamp_range(const std::string& abbr) {
    static const std::array<std::pair<const char*, ClampRange>, 9> kRanges{{
        {"ap", {30, 300}},
        {"as", {0, 100}},
        {"br", {0, 72}},
        {"hs", {40, 200}},
        {"la", {0, 100}},
        {"pr", {0, 250}},
        {"ri", {0, 100}},
        {"sm", {0, 100}},
        {"g5", {0, 80}},
    }};
    for (const auto& entry : kRanges) {
        if (abbr == entry.first) return entry.second;
    }
    return kRanges[0].second;  // unreachable for the callers below
}

int clamp(const std::string& abbr, double value) {
    const ClampRange& r = clamp_range(abbr);
    double bounded = std::min(std::max(value, r.lo), r.hi);
    return static_cast<int>(py_round(bounded));
}

// VOICE_PARAM_DEFAULTS (commands.py), copied verbatim: (default, min, max)
// per voice per DV abbreviation.
struct VoiceParamEntry { const char* abbr; int def, lo, hi; };
struct VoiceDefaults { const char* voice; std::array<VoiceParamEntry, 9> params; };

const std::array<VoiceDefaults, 10> kVoiceParamDefaults{{
    {"paul",   {{{"g5", 72, 0, 80}, {"ap", 120, 30, 300}, {"pr", 100, 0, 250}, {"hs", 100, 40, 200}, {"br", 0, 0, 72}, {"ri", 80, 0, 100}, {"sm", 54, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"betty",  {{{"g5", 68, 0, 80}, {"ap", 180, 30, 300}, {"pr", 160, 0, 250}, {"hs", 100, 40, 200}, {"br", 46, 0, 72}, {"ri", 0, 0, 100}, {"sm", 44, 0, 100}, {"la", 0, 0, 100}, {"as", 65, 0, 100}}}},
    {"harry",  {{{"g5", 69, 0, 80}, {"ap", 78, 30, 300}, {"pr", 50, 0, 250}, {"hs", 120, 40, 200}, {"br", 0, 0, 72}, {"ri", 86, 0, 100}, {"sm", 34, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"frank",  {{{"g5", 74, 0, 80}, {"ap", 153, 30, 300}, {"pr", 90, 0, 250}, {"hs", 90, 40, 200}, {"br", 50, 0, 72}, {"ri", 80, 0, 100}, {"sm", 36, 0, 100}, {"la", 12, 0, 100}, {"as", 65, 0, 100}}}},
    {"dennis", {{{"g5", 74, 0, 80}, {"ap", 100, 30, 300}, {"pr", 135, 0, 250}, {"hs", 105, 40, 200}, {"br", 62, 0, 72}, {"ri", 0, 0, 100}, {"sm", 100, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"kit",    {{{"g5", 62, 0, 80}, {"ap", 306, 30, 300}, {"pr", 180, 0, 250}, {"hs", 80, 40, 200}, {"br", 40, 0, 72}, {"ri", 40, 0, 100}, {"sm", 44, 0, 100}, {"la", 0, 0, 100}, {"as", 65, 0, 100}}}},
    {"rita",   {{{"g5", 72, 0, 80}, {"ap", 106, 30, 300}, {"pr", 80, 0, 250}, {"hs", 95, 40, 200}, {"br", 49, 0, 72}, {"ri", 0, 0, 100}, {"sm", 34, 0, 100}, {"la", 4, 0, 100}, {"as", 65, 0, 100}}}},
    {"ursula", {{{"g5", 69, 0, 80}, {"ap", 264, 30, 300}, {"pr", 135, 0, 250}, {"hs", 95, 40, 200}, {"br", 0, 0, 72}, {"ri", 100, 0, 100}, {"sm", 64, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"wendy",  {{{"g5", 80, 0, 80}, {"ap", 264, 30, 300}, {"pr", 135, 0, 250}, {"hs", 95, 40, 200}, {"br", 58, 0, 72}, {"ri", 0, 0, 100}, {"sm", 100, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"val",    {{{"g5", 72, 0, 80}, {"ap", 120, 30, 300}, {"pr", 100, 0, 250}, {"hs", 100, 40, 200}, {"br", 0, 0, 72}, {"ri", 80, 0, 100}, {"sm", 54, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
}};

const VoiceDefaults& voice_defaults_for(const std::string& voice_key) {
    for (const auto& v : kVoiceParamDefaults) {
        if (voice_key == v.voice) return v;
    }
    for (const auto& v : kVoiceParamDefaults) {  // fall back to "paul"
        if (std::string("paul") == v.voice) return v;
    }
    return kVoiceParamDefaults[0];  // unreachable
}

// voice_param(voice, param) (commands.py): (default, min, max), with the
// range widened to include the default itself (Kit's average pitch default
// of 306 sits above the reported 30..300 maximum).
void voice_param(const std::string& voice_key, const std::string& abbr,
                  int* out_default, int* out_lo, int* out_hi) {
    const VoiceDefaults& v = voice_defaults_for(voice_key);
    for (const auto& p : v.params) {
        if (abbr == p.abbr) {
            *out_default = p.def;
            *out_lo = std::min(p.lo, p.def);
            *out_hi = std::max(p.hi, p.def);
            return;
        }
    }
    *out_default = 0;
    *out_lo = 0;
    *out_hi = 0;
}

// scale_from_default(voice, param, slider) (commands.py): piecewise-linear
// map of NVDA's 0..100 slider onto the parameter, 50 = this voice's default.
int scale_from_default(const std::string& voice_key, const std::string& abbr, int slider) {
    int default_v, lo, hi;
    voice_param(voice_key, abbr, &default_v, &lo, &hi);
    slider = std::max(0, std::min(100, slider));
    if (slider == 50) return default_v;
    if (slider < 50) {
        return static_cast<int>(py_round(lo + (default_v - lo) * (slider / 50.0)));
    }
    return static_cast<int>(py_round(default_v + (hi - default_v) * ((slider - 50) / 50.0)));
}

}  // namespace

std::string rate_command(int wpm) {
    int clamped = std::min(std::max(wpm, RATE_MIN_WPM), RATE_MAX_WPM);
    return "[:ra " + std::to_string(clamped) + "]";
}

std::string voice_command(const char* mnemonic) {
    return std::string("[:") + mnemonic + "]";
}

std::string dv_command(const std::string& voice_key, const DvParams& p) {
    // Mirrors __init__.py::_designVoiceCommand exactly: a slider left at 50
    // (this voice's own default) is skipped entirely -- `if sliderValue ==
    // 50: continue` -- and if nothing differs, no [:dv ...] is sent at all
    // -- `if not params: return ""`. Measured against the real firmware:
    // sending the full 9-token form (even with every value at the voice's
    // own default) breaks synthesis outright, so this is not merely a
    // shorter-prefix optimization.
    //
    // Order matches commands.py's design_voice_command() as driven by
    // __init__.py::_designVoiceCommand's own iteration order: ap, pr, hs,
    // br, ri, sm, g5, then laryngealization/assertiveness (la, as), which
    // that driver method doesn't currently expose but DV_PARAMS supports.
    struct Token { const char* abbr; int slider; };
    const Token tokens[] = {
        {"ap", p.pitch},
        {"pr", p.inflection},
        {"hs", p.head_size},
        {"br", p.breathiness},
        {"ri", p.richness},
        {"sm", p.smoothness},
        {"g5", p.loudness},
        {"la", p.laryngealization},
        {"as", p.assertiveness},
    };
    std::string out;
    for (const auto& t : tokens) {
        if (t.slider == 50) continue;  // this voice's own default: omit
        int scaled = scale_from_default(voice_key, t.abbr, t.slider);
        int value = clamp(t.abbr, scaled);
        out += " ";
        out += t.abbr;
        out += " ";
        out += std::to_string(value);
    }
    if (out.empty()) return "";
    return "[:dv" + out + "]";
}

std::string sanitize_text(const std::string& utf8) {
    std::string text = utf8;
    for (char& ch : text) {
        if (ch == '[' || ch == ']') ch = ' ';
    }
    // commands.py: _ISOLATED_OPEN_PAREN = re.compile(r"\((?=\s|$)")
    static const std::regex isolated_open(R"(\((?=\s|$))");
    text = std::regex_replace(text, isolated_open, " ");
    // commands.py: _ISOLATED_CLOSE_PAREN = re.compile(r"(^|\s)\)")
    static const std::regex isolated_close(R"((^|\s)\))");
    text = std::regex_replace(text, isolated_close, "$1 ");
    return text;
}

std::string flush_suffix(const std::string& sanitized) {
    // Mirrors __init__.py's _terminate() (append "," unless already ending
    // in one of ".!?,", after an rstrip) plus the unconditional trailing
    // "\r" every payload gets in _speakLine.
    static const std::string terminators = ".!?,";
    size_t end = sanitized.find_last_not_of(" \t\r\n\f\v");
    if (end == std::string::npos) {
        // Empty or all-whitespace: _terminate() returns "" unchanged (its
        // `if not text: return text` guard), so the payload is just "\r".
        return "\r";
    }
    char last = sanitized[end];
    if (terminators.find(last) != std::string::npos) {
        return "\r";
    }
    return ",\r";
}

namespace {

// UTF-8 bytes for the code point starting at text[i], and how many UTF-16
// units it spans -- matching WideCharToMultiByte(CP_UTF8), which turns an
// unpaired surrogate into U+FFFD (three bytes).
size_t utf8_bytes_at(const std::wstring& text, size_t i, size_t* units) {
    const wchar_t c = text[i];
    *units = 1;
    if (c < 0x80) return 1;
    if (c < 0x800) return 2;
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() &&
        text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
        *units = 2;
        return 4;
    }
    return 3;
}

bool is_space(wchar_t c) {
    return iswspace(c) != 0;
}

}  // namespace

std::vector<TextPiece> split_for_firmware(const std::wstring& text, size_t first_budget,
                                          size_t budget) {
    std::vector<TextPiece> pieces;
    const size_t n = text.size();
    size_t pos = 0;
    for (;;) {
        while (pos < n && is_space(text[pos])) ++pos;
        if (pos >= n) break;

        // The furthest end whose text fits this piece's budget. At least one
        // code point is always taken, so a budget can never stall the loop.
        const size_t limit = pieces.empty() ? first_budget : budget;
        size_t fit = pos;
        size_t bytes = 0;
        while (fit < n) {
            size_t units = 0;
            const size_t b = utf8_bytes_at(text, fit, &units);
            if (bytes + b > limit && fit > pos) break;
            bytes += b;
            fit += units;
            if (bytes >= limit) break;
        }

        size_t rest = fit;
        while (rest < n && is_space(text[rest])) ++rest;
        if (rest >= n) {
            size_t end = fit;
            while (end > pos && is_space(text[end - 1])) --end;
            pieces.push_back({pos, end});
            break;
        }

        // Break at the first space of a run, ranked by what precedes it: a
        // sentence ender, a clause mark, or anything else. Rightmost wins
        // within the best rank present.
        size_t best[3] = {0, 0, 0};
        for (size_t k = pos + 1; k <= fit && k < n; ++k) {
            if (!is_space(text[k]) || is_space(text[k - 1])) continue;
            const wchar_t before = text[k - 1];
            int rank = 2;
            if (before == L'.' || before == L'!' || before == L'?') {
                rank = 0;
            } else if (before == L',' || before == L';' || before == L':') {
                rank = 1;
            }
            best[rank] = k;
        }
        size_t end = best[0] ? best[0] : best[1] ? best[1] : best[2];
        if (!end) end = fit;  // no whitespace within reach: cut the word
        pieces.push_back({pos, end});
        pos = end;
    }
    return pieces;
}

}  // namespace dtc01
