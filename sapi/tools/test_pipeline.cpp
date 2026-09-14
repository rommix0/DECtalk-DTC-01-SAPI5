#include "text_pipeline.hpp"
#include <cassert>
#include <cwctype>
int wmain() {
    assert(dtc01::rate_command(999) == "[:ra 350]");         // clamp high
    assert(dtc01::rate_command(50)  == "[:ra 120]");         // clamp low
    assert(dtc01::voice_command("nh") == "[:nh]");
    assert(dtc01::sanitize_text("a[b]c") == "a b c");        // brackets -> space
    dtc01::DvParams def;                                     // all 50 -> voice defaults
    // paul head-size default 100 (%). At slider 50 dv must emit hs 100 (or omit if all default).
    auto s = dtc01::dv_command("paul", def);
    assert(s.empty() || s.find("hs 100") != std::string::npos);
    // All sliders at 50 (voice default) must send NO [:dv ...] at all --
    // sending the full 9-token form broke real firmware synthesis
    // (v2.0 truncated, v1.8 ran to the line cap and never ended).
    assert(dtc01::dv_command("paul", def) == "");
    // Exactly one non-default slider must yield a [:dv ...] with only that
    // param's token -- the other eight abbreviations must not appear.
    dtc01::DvParams oneChanged;
    oneChanged.head_size = 70;
    auto hs = dtc01::dv_command("paul", oneChanged);
    assert(hs.find("hs ") != std::string::npos);
    assert(hs.find("ap ") == std::string::npos);
    assert(hs.find("pr ") == std::string::npos);
    assert(hs.find("br ") == std::string::npos);
    assert(hs.find("ri ") == std::string::npos);
    assert(hs.find("sm ") == std::string::npos);
    assert(hs.find("g5 ") == std::string::npos);
    assert(hs.find("la ") == std::string::npos);
    assert(hs.find("as ") == std::string::npos);

    // split_for_firmware: the firmware drops input lines past ~134 bytes
    // (DESIGN.md s19), so long fragments are fed one fitting piece at a time.
    {
        using dtc01::split_for_firmware;
        const auto text_of = [](const std::wstring& s, const dtc01::TextPiece& p) {
            return s.substr(p.begin, p.end - p.begin);
        };

        // Short text: one piece, without the surrounding whitespace.
        const std::wstring short_text = L"  Desktop  list  ";
        const auto one = split_for_firmware(short_text, 100, 118);
        assert(one.size() == 1 && text_of(short_text, one[0]) == L"Desktop  list");
        assert(split_for_firmware(L"   ", 100, 118).empty());
        assert(split_for_firmware(L"", 100, 118).empty());

        // A sentence ender beats a later clause mark or space...
        const std::wstring s1 = L"First sentence here. Second part, with a clause and more after it";
        assert(text_of(s1, split_for_firmware(s1, 40, 40)[0]) == L"First sentence here.");
        // ...and a clause mark beats a later plain space.
        const std::wstring s2 = L"alpha beta gamma, delta epsilon zeta eta theta";
        assert(text_of(s2, split_for_firmware(s2, 30, 30)[0]) == L"alpha beta gamma,");

        // A mark followed by anything but whitespace is not a break, so a
        // version number is never cut (DESIGN.md s22).
        const std::wstring s3 = L"install version 0.5.59 of the add-on now";
        for (const auto& p : split_for_firmware(s3, 24, 24)) {
            const std::wstring t = text_of(s3, p);
            assert(t.find(L"0.5") == std::wstring::npos || t.find(L"0.5.59") != std::wstring::npos);
        }

        // Pieces fit their budgets, run in order, and only whitespace falls
        // between them.
        std::wstring long_text;
        for (int i = 0; i < 40; ++i) {
            long_text += L"The quick brown fox jumps over the lazy dog, again and again. ";
        }
        const auto pieces = split_for_firmware(long_text, 60, 118);
        assert(pieces.size() > 1);
        size_t cursor = 0;
        for (size_t i = 0; i < pieces.size(); ++i) {
            assert(pieces[i].begin >= cursor && pieces[i].end > pieces[i].begin);
            for (size_t k = cursor; k < pieces[i].begin; ++k) assert(iswspace(long_text[k]));
            assert(pieces[i].end - pieces[i].begin <= (i == 0 ? 60u : 118u));  // ASCII: units == bytes
            cursor = pieces[i].end;
        }
        for (size_t k = cursor; k < long_text.size(); ++k) assert(iswspace(long_text[k]));

        // A word longer than the budget is cut, not dropped or overrun.
        const auto word = split_for_firmware(std::wstring(300, L'x'), 100, 118);
        assert(word.size() == 3 && word[0].end == 100 && word[1].end == 218 && word[2].end == 300);

        // Budgets are UTF-8 bytes: U+00E9 takes two, so fifty fill 100...
        assert(split_for_firmware(std::wstring(120, L'é'), 100, 100)[0].end == 50);
        // ...and a surrogate pair (four bytes) is never split.
        std::wstring faces;
        for (int i = 0; i < 30; ++i) faces += L"\U0001F600";
        for (const auto& p : split_for_firmware(faces, 10, 10)) assert((p.end - p.begin) % 2 == 0);
    }

    // Design Voice state: what the firmware holds, and the way back from a
    // change (DESIGN.md s24).
    {
        const dtc01::DvValues paul = dtc01::dv_defaults("paul");
        assert(paul.value[0] == 120);  // ap
        const dtc01::DvValues same = dtc01::dv_values("paul", dtc01::DvParams{});
        for (int i = 0; i < 9; ++i) assert(same.value[i] == paul.value[i]);
        bool needs_voice = true;
        assert(dtc01::dv_change_command(paul, same, &needs_voice).empty() && !needs_voice);

        // Pitch up -- a capital letter -- then back to the voice's own pitch.
        dtc01::DvParams up;
        up.pitch = 80;
        const dtc01::DvValues high = dtc01::dv_values("paul", up);
        assert(dtc01::dv_change_command(paul, high, nullptr) == "[:dv ap 228]");
        assert(dtc01::dv_change_command(high, paul, &needs_voice) == "[:dv ap 120]" && !needs_voice);

        // Only what differs is sent, and from the defaults it agrees with
        // dv_command.
        dtc01::DvParams both = up;
        both.head_size = 70;
        const dtc01::DvValues tuned = dtc01::dv_values("paul", both);
        assert(dtc01::dv_change_command(high, tuned, nullptr) == "[:dv hs 140]");
        assert(dtc01::dv_change_command(paul, tuned, nullptr) == dtc01::dv_command("paul", both));

        // Kit's default pitch (306) is above what [:dv ap] accepts (300):
        // raising it sends nothing rather than lowering it to 300, lowering it
        // works, and only selecting the voice again gets 306 back.
        const dtc01::DvValues kit = dtc01::dv_defaults("kit");
        assert(kit.value[0] == 306);
        assert(dtc01::dv_change_command(kit, dtc01::dv_values("kit", up), nullptr).empty());
        dtc01::DvParams down;
        down.pitch = 20;
        const dtc01::DvValues kit_low = dtc01::dv_values("kit", down);
        assert(dtc01::dv_change_command(kit, kit_low, nullptr) == "[:dv ap 140]");
        assert(dtc01::dv_change_command(kit_low, kit, &needs_voice).empty() && needs_voice);
    }
    return 0;
}
