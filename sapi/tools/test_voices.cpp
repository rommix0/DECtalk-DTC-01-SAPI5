#include "voices.hpp"
#include <cassert>
#include <cstring>
int wmain() {
    assert(dtc01::voice_count() == 18);
    int v20 = 0, v18 = 0;
    for (int i = 0; i < 18; ++i)
        (std::strcmp(dtc01::VOICES[i].firmware, "v20") == 0 ? v20 : v18)++;
    assert(v20 == 10 && v18 == 8);
    assert(dtc01::find_voice("dennis", "v18") == nullptr);   // dennis is v20-only
    assert(std::strcmp(dtc01::find_voice("paul","v20")->mnemonic, "np") == 0);
    return 0;
}
