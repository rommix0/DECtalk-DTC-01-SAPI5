// dectalk_config_logic.hpp - the control-value -> HKCU write logic for the
// DECtalk DTC-01 configuration utility, factored out of dectalk_config.cpp
// so it can be exercised with no window, dialog, or message loop at all
// (see tools/test_config_persist.cpp). Every function here does exactly one
// registry write via dectalk::settings and nothing else -- dialog_proc in
// dectalk_config.cpp only decides *when* to call them (on WM_HSCROLL,
// CBN_SELCHANGE, ...), and pure GUI concerns (updating a readout, disabling
// a control) never happen in here.
#pragma once

#include "../src/user_settings.hpp"
#include "dectalk_config_res.h"

namespace dectalk_config {

// One entry per per-voice slider control: its dialog control id and the
// registry value name dectalk::settings::SLIDER_FIELDS (user_settings.hpp)
// expects for it. Keeping the mapping in a table, rather than a switch,
// means a slider's control id and its registry name only need to agree in
// one place.
struct voice_field {
    int id;
    const wchar_t* reg_name;
};

inline constexpr voice_field VOICE_FIELDS[] = {
    { IDC_PITCH,            L"Pitch" },
    { IDC_INFLECTION,       L"Inflection" },
    { IDC_HEADSIZE,         L"HeadSize" },
    { IDC_BREATHINESS,      L"Breathiness" },
    { IDC_RICHNESS,         L"Richness" },
    { IDC_SMOOTHNESS,       L"Smoothness" },
    { IDC_LOUDNESS,         L"Loudness" },
    { IDC_LARYNGEALIZATION, L"Laryngealization" },
    { IDC_ASSERTIVENESS,    L"Assertiveness" },
};

// Writes one per-voice slider's new value to HKCU for (voice_key, firmware).
// Returns false without writing anything if control_id isn't one of
// VOICE_FIELDS (e.g. a global control's id passed here by mistake).
[[nodiscard]] inline bool apply_setting(const char* voice_key, const char* firmware,
                                        int control_id, int value) {
    for (const voice_field& f : VOICE_FIELDS) {
        if (f.id == control_id) {
            return dectalk::settings::write_voice_int(voice_key, firmware, f.reg_name, value);
        }
    }
    return false;
}

// Writes one global int-valued control's new value to HKCU.
[[nodiscard]] inline bool apply_global_setting(int control_id, int value) {
    switch (control_id) {
    case IDC_RATE:      return dectalk::settings::write_global_int(L"RatePercent", value);
    case IDC_VOLUME:    return dectalk::settings::write_global_int(L"VolumeDB", value);
    case IDC_RATEBOOST: return dectalk::settings::write_global_int(L"RateBoost", value);
    default:            return false;
    }
}

// The one global control that isn't an int: the default-firmware combo.
// value is "v20" or "v18", matching dtc01::VoiceDef::firmware.
[[nodiscard]] inline bool apply_global_firmware(const wchar_t* firmware) {
    return dectalk::settings::write_global_string(L"DefaultFirmware", firmware);
}

}  // namespace dectalk_config
