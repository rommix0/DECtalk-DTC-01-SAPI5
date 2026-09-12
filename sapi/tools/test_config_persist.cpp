// Exercises dectalk_config_logic.hpp's control-value -> HKCU write functions
// with no window, dialog, or message loop at all -- the GUI-free contract
// the DECtalk DTC-01 configuration utility (dectalk_config.cpp) is built on.
// Like test_settings.cpp, this runs against the real
// HKCU\Software\DECtalkDTC01 key (there is no registry sandbox available to
// a command-line tool); it cleans up after itself with reset_all().
#include "dectalk_config_logic.hpp"
#include "dectalk_config_res.h"
#include "../src/user_settings.hpp"
#include <cassert>

int wmain() {
    using namespace dectalk::settings;

    // Start from a clean slate so this test is not at the mercy of whatever
    // a previous run (or a real user) left behind.
    reset_all();

    // A per-voice slider: apply_setting() writes it, and load_voice() (the
    // function the SAPI engine itself reads settings through) sees it.
    assert(dectalk_config::apply_setting("paul", "v20", IDC_HEADSIZE, 77));
    assert(load_voice("paul", "v20").head_size == 77);

    // A different voice is untouched -- writing one voice's slider must not
    // disturb another's.
    assert(load_voice("betty", "v20").head_size == 50);

    // Every per-voice slider control id round-trips through its own
    // registry value.
    assert(dectalk_config::apply_setting("harry", "v18", IDC_PITCH, 12));
    assert(load_voice("harry", "v18").pitch == 12);
    assert(dectalk_config::apply_setting("harry", "v18", IDC_LARYNGEALIZATION, 88));
    assert(load_voice("harry", "v18").laryngealization == 88);

    // A control id that isn't a per-voice slider is rejected rather than
    // silently writing to the wrong place.
    assert(!dectalk_config::apply_setting("paul", "v20", IDC_RATE, 150));

    // A global int-valued control: apply_global_setting() writes it, and
    // load_global() sees it.
    assert(dectalk_config::apply_global_setting(IDC_RATE, 175));
    assert(load_global().rate_percent == 175);
    assert(dectalk_config::apply_global_setting(IDC_VOLUME, -12));
    assert(load_global().volume_db == -12);
    assert(dectalk_config::apply_global_setting(IDC_RATEBOOST, 40));
    assert(load_global().rate_boost == 40);

    // A control id that isn't a global int-valued control is rejected.
    assert(!dectalk_config::apply_global_setting(IDC_PITCH, 50));

    // The default-firmware combo, the one global control that isn't an int.
    assert(dectalk_config::apply_global_firmware(L"v18"));
    assert(load_global().default_firmware == "v18");
    assert(dectalk_config::apply_global_firmware(L"v20"));
    assert(load_global().default_firmware == "v20");

    // Clean up every HKCU value this test (and dectalk_config.cpp) manage,
    // so a developer's own settings are left exactly as they were.
    reset_all();
    assert(load_voice("paul", "v20").head_size == 50);
    assert(load_global().rate_percent == 100);

    return 0;
}
