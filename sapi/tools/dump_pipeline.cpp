// dump_pipeline.cpp - Task E1 command-parity harness: runs the real C++
// text_pipeline primitives (the ones the SAPI engine calls) and prints their
// output in a form sapi/tools/verify_pipeline.py can diff, line for line,
// against addon/synthDrivers/dectalkDtc01/protocol/commands.py.
//
// Usage:
//   dump_pipeline <voice_key> <wpm> <inflection> <head_size> <breathiness> \
//                 <richness> <smoothness> <loudness> <laryngealization> \
//                 <assertiveness> <pitch> <text>
//
// The 9 slider args are in dtc01::DvParams' own field order (NOT the C++
// dv_command emit order -- see text_pipeline.hpp). <text> is everything left
// in argv after the 11 fixed positional args, rejoined with single spaces
// (robust to a caller that didn't quote a multi-word text argument; a
// properly quoted/list-based invocation -- which is how verify_pipeline.py's
// subprocess call works -- already arrives as one argv element, so the join
// is a no-op there).
#include "text_pipeline.hpp"
#include "voices.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

// Wide (UTF-16, as argv arrives under wmain) -> UTF-8 std::string.
std::string to_utf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len - 1), '\0');  // len includes the NUL
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// Renders \r and \n as the two-character sequences \r and \n so a
// line-based reader on the Python side sees one clean line per field, even
// when the underlying text/command contains literal control characters.
std::string escape_control(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\n') {
            out += "\\n";
        } else {
            out += ch;
        }
    }
    return out;
}

// _terminate()'s own rstrip() strips Python's str.strip() whitespace set;
// restricted here to the ASCII whitespace chars that can actually appear in
// sanitized DTC-01 text (sanitize_text never introduces anything outside
// ASCII whitespace).
std::string rstrip_ascii_whitespace(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n\f\v");
    if (end == std::string::npos) return std::string();
    return s.substr(0, end + 1);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // program + voice_key + wpm + 9 sliders + at least 1 text token.
    if (argc < 13) {
        std::fwprintf(stderr,
            L"usage: %s <voice_key> <wpm> <inflection> <head_size> <breathiness> "
            L"<richness> <smoothness> <loudness> <laryngealization> "
            L"<assertiveness> <pitch> <text>\n",
            argc > 0 ? argv[0] : L"dump_pipeline");
        return 2;
    }

    const std::string voice_key = to_utf8(argv[1]);
    const int wpm = _wtoi(argv[2]);

    dtc01::DvParams params;
    params.inflection       = _wtoi(argv[3]);
    params.head_size        = _wtoi(argv[4]);
    params.breathiness      = _wtoi(argv[5]);
    params.richness         = _wtoi(argv[6]);
    params.smoothness       = _wtoi(argv[7]);
    params.loudness         = _wtoi(argv[8]);
    params.laryngealization = _wtoi(argv[9]);
    params.assertiveness    = _wtoi(argv[10]);
    params.pitch            = _wtoi(argv[11]);

    std::string text;
    for (int i = 12; i < argc; ++i) {
        if (i > 12) text += " ";
        text += to_utf8(argv[i]);
    }

    // Mnemonic is firmware-independent (both firmware tables map the same
    // key to the same mnemonic for every voice they share); "v20" carries
    // all 10 base voices the corpus exercises, including dennis/wendy.
    const dtc01::VoiceDef* voice = dtc01::find_voice(voice_key, "v20");
    if (!voice) {
        std::fprintf(stderr, "ERROR: unknown voice\n");
        return 1;
    }

    const std::string sanitized = dtc01::sanitize_text(text);
    const std::string trimmed = rstrip_ascii_whitespace(sanitized);

    std::printf("RATE=%s\n", dtc01::rate_command(wpm).c_str());
    std::printf("VOICE=%s\n", dtc01::voice_command(voice->mnemonic).c_str());
    std::printf("DV=%s\n", dtc01::dv_command(voice_key, params).c_str());
    std::printf("SANITIZE=%s\n", escape_control(sanitized).c_str());
    std::printf("FLUSH=%s\n", escape_control(dtc01::flush_suffix(trimmed)).c_str());
    return 0;
}
