// Exercises the HKCU settings model directly against the real
// HKCU\Software\DECtalkDTC01 key (there is no registry sandbox available to
// a command-line tool). That's acceptable because it's the app's own key and
// the test cleans up after itself with reset_all() -- but it does mean a
// developer who has hand-tuned settings under this key will have them wiped
// by running this test.
#include "user_settings.hpp"
#include <cassert>

int wmain() {
    using namespace dectalk::settings;

    // Start from a clean slate so this test is not at the mercy of whatever
    // a previous run (or a real user) left behind.
    reset_all();

    // Global int round-trips, clamped on read.
    assert(write_global_int(L"RatePercent", 150));
    assert(load_global().rate_percent == 150);

    // A voice's slider round-trips, and a different, untouched voice keeps
    // its default -- writing one voice must not disturb another.
    assert(write_voice_int("paul", "v20", L"HeadSize", 70));
    assert(load_voice("paul", "v20").head_size == 70);
    assert(load_voice("betty", "v20").head_size == 50);

    // Out-of-range writes are clamped on read, not on write.
    assert(write_global_int(L"RatePercent", 9999));
    assert(load_global().rate_percent == 400);
    assert(write_global_int(L"RatePercent", -5));
    assert(load_global().rate_percent == 25);

    // DefaultFirmware is REG_SZ, not REG_DWORD.
    assert(write_global_string(L"DefaultFirmware", L"v18"));
    assert(load_global().default_firmware == "v18");

    // A garbage firmware string falls back to the default rather than
    // propagating an invalid value.
    assert(write_global_string(L"DefaultFirmware", L"bogus"));
    assert(load_global().default_firmware == "v20");

    // reset_voice() clears just the one voice.
    assert(write_voice_int("paul", "v20", L"HeadSize", 80));
    reset_voice("paul", "v20");
    assert(load_voice("paul", "v20").head_size == 50);

    // reset_all() clears everything this module manages.
    assert(write_global_int(L"RatePercent", 200));
    assert(write_voice_int("paul", "v20", L"HeadSize", 90));
    reset_all();
    assert(load_global().rate_percent == 100);
    assert(load_global().volume_db == 0);
    assert(load_global().rate_boost == 0);
    assert(load_global().default_firmware == "v20");
    assert(load_voice("paul", "v20").head_size == 50);

    return 0;
}
