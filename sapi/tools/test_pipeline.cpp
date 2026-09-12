#include "text_pipeline.hpp"
#include <cassert>
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
    return 0;
}
