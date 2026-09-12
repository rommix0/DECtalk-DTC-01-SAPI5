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
    return 0;
}
