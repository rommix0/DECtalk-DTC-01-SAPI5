// Exercises dectalk_config_logic.hpp's control-value -> HKCU write functions
// with no window, dialog, or message loop at all -- the GUI-free contract
// the DECtalk DTC-01 configuration utility (dectalk_config.cpp) is built on.
// Like test_settings.cpp, this runs against the real
// HKCU\Software\DECtalkDTC01 key (there is no registry sandbox available to
// a command-line tool), with the settings already there copied aside first and
// put back when it ends (settings_backup.hpp).
#include "dectalk_config_logic.hpp"
#include "dectalk_config_res.h"
#include "settings_backup.hpp"
#include "../src/user_settings.hpp"
#include <cassert>
#include <cstdio>

int wmain() {
    using namespace dectalk::settings;

    const dectalk::test::SettingsBackup backup;
    if (!backup.ok()) {
        std::fputs("test_config_persist: could not back up HKCU\\Software\\DECtalkDTC01\n", stderr);
        return 1;
    }

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

    // "Apply settings to all voices", on the case that asked for it: Perfect
    // Paul tuned -- assertiveness 60, richness 0, breathiness 0, loudness 100
    // -- and then every other voice given the same adjustments.
    reset_all();
    assert(dectalk_config::apply_setting("paul", "v20", IDC_ASSERTIVENESS, 60));
    assert(dectalk_config::apply_setting("paul", "v20", IDC_RICHNESS, 0));
    assert(dectalk_config::apply_setting("paul", "v20", IDC_BREATHINESS, 0));
    assert(dectalk_config::apply_setting("paul", "v20", IDC_LOUDNESS, 100));
    assert(dectalk_config::copy_voice_to_all_voices("paul", "v20"));
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        const VoiceSettings s = load_voice(v.key, v.firmware);
        assert(s.assertiveness == 60 && s.richness == 0 && s.breathiness == 0 && s.loudness == 100);
        assert(s.pitch == 50 && s.inflection == 50 && s.head_size == 50 &&
               s.smoothness == 50 && s.laryngealization == 50);
    }

    // While the box is ticked, one slider change reaches every voice...
    assert(dectalk_config::apply_setting_to_all_voices(IDC_PITCH, 35));
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        assert(load_voice(v.key, v.firmware).pitch == 35);
    }
    // ...and a control that isn't a voice slider writes nothing anywhere.
    assert(!dectalk_config::apply_setting_to_all_voices(IDC_RATE, 150));
    assert(load_global().rate_percent == 100);

    // "Reset all voices" puts every voice back to its own defaults.
    dectalk_config::reset_all_voices();
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        const VoiceSettings s = load_voice(v.key, v.firmware);
        assert(s.pitch == 50 && s.assertiveness == 50 && s.richness == 50 && s.loudness == 50);
    }

    // The tick box persists, and reset_all() clears it with everything else.
    assert(!load_apply_to_all_voices());
    assert(dectalk_config::set_apply_to_all_voices(true));
    assert(load_apply_to_all_voices());
    assert(dectalk_config::set_apply_to_all_voices(false));
    assert(!load_apply_to_all_voices());
    assert(dectalk_config::set_apply_to_all_voices(true));

    // "Allow SAPI5 apps to control rate, pitch and volume" is on until it is
    // unticked, and reset_all() turns it back on.
    assert(load_global().app_control);
    assert(dectalk_config::set_app_control(false));
    assert(!load_global().app_control);
    assert(dectalk_config::set_app_control(true));
    assert(load_global().app_control);
    assert(dectalk_config::set_app_control(false));

    // "Diagnostic log for bug reports" is Off until chosen, round-trips
    // through load_logging(), and reset_all() turns it off again.
    assert(!load_logging());
    assert(dectalk_config::set_logging(true));
    assert(load_logging());
    assert(dectalk_config::set_logging(false));
    assert(!load_logging());
    assert(dectalk_config::set_logging(true));

    // reset_all() clears every HKCU value this test (and dectalk_config.cpp)
    // manage; the backup then puts back whatever was saved before the test.
    reset_all();
    assert(load_voice("paul", "v20").head_size == 50);
    assert(load_global().rate_percent == 100);
    assert(!load_apply_to_all_voices());
    assert(load_global().app_control);
    assert(!load_logging());

    return 0;
}
