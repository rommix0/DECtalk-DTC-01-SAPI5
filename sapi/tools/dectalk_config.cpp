// DECtalk DTC-01 configuration utility.
//
// A plain Win32 dialog, chosen deliberately: standard controls with labels
// in tab order are what screen readers handle best, with no framework in
// the way (see dectalk_config.rc for the accessibility rationale in detail).
// Every change is written to HKCU\Software\DECtalkDTC01 the moment it is
// made (dectalk_config_logic.hpp), and the SAPI engine (Task C2/C3) re-reads
// those values on every utterance -- so a change here needs no "Apply"
// button, and closing the dialog by any means (Close, Esc, Alt-F4, the
// title-bar X) loses nothing, because nothing was ever held back.
//
// The dialog edits one voice at a time (the Voice box picks which), can
// preview it through SAPI itself with "Play sample", and separately holds
// the global settings -- rate/volume/rate-boost/default-firmware -- that
// apply no matter which voice is speaking.
#include <windows.h>
#include <commctrl.h>
#include <sapi.h>
// initguid ahead of oleacc instantiates the accessibility GUIDs (the
// annotation service and the Name property) right here, with no extra lib.
#include <initguid.h>
#include <oleacc.h>
#include <algorithm>
#include <cstdio>
#include <string>

#include "../src/voices.hpp"
#include "../src/voice_registry.hpp"
#include "dectalk_config_logic.hpp"
#include "dectalk_config_res.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using dectalk::settings::GlobalSettings;
using dectalk::settings::VoiceSettings;

namespace {

// ---------------------------------------------------------------------------
// State: which of the 18 static voices (dtc01::VOICES) is being edited.
// ---------------------------------------------------------------------------

int g_voice_index = 0;

ISpVoice* g_preview = nullptr;

const dtc01::VoiceDef& current_voice() {
    return dtc01::VOICES[g_voice_index];
}

// ---------------------------------------------------------------------------
// Trackbars: one table drives range setup, live readout updates, and (for
// WM_HSCROLL) which registry write applies. IDC_RATE/VOLUME/RATEBOOST are
// global; every other id here is a per-voice slider (see VOICE_SLIDER_CTLS
// below for the VoiceSettings member each one maps to).
// ---------------------------------------------------------------------------

struct slider_range { int id; int val_id; int min; int max; };

const slider_range SLIDERS[] = {
    { IDC_PITCH,            IDC_PITCH_VAL,            dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_INFLECTION,       IDC_INFLECTION_VAL,       dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_HEADSIZE,         IDC_HEADSIZE_VAL,         dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_BREATHINESS,      IDC_BREATHINESS_VAL,      dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_RICHNESS,         IDC_RICHNESS_VAL,         dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_SMOOTHNESS,       IDC_SMOOTHNESS_VAL,       dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_LOUDNESS,         IDC_LOUDNESS_VAL,         dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_LARYNGEALIZATION, IDC_LARYNGEALIZATION_VAL, dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_ASSERTIVENESS,    IDC_ASSERTIVENESS_VAL,    dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX },
    { IDC_RATE,             IDC_RATE_VAL,             dectalk::settings::RATE_PERCENT_MIN, dectalk::settings::RATE_PERCENT_MAX },
    { IDC_VOLUME,           IDC_VOLUME_VAL,           dectalk::settings::VOLUME_DB_MIN, dectalk::settings::VOLUME_DB_MAX },
    { IDC_RATEBOOST,        IDC_RATEBOOST_VAL,        dectalk::settings::RATE_BOOST_MIN, dectalk::settings::RATE_BOOST_MAX },
};

void init_slider(HWND dlg, const slider_range& s) {
    HWND tb = GetDlgItem(dlg, s.id);
    SendMessageW(tb, TBM_SETRANGEMIN, FALSE, s.min);
    SendMessageW(tb, TBM_SETRANGEMAX, FALSE, s.max);
    SendMessageW(tb, TBM_SETLINESIZE, 0, 1);
    SendMessageW(tb, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(tb, TBM_SETTICFREQ, (s.max - s.min) / 10, 0);
}

// Moves the trackbar and refreshes its readout -- the one place a slider's
// current value is announced to a screen reader (via the accessible name
// pinned on the readout in set_accessible_names, and via its own text).
void set_slider(HWND dlg, int id, int value) {
    for (const slider_range& s : SLIDERS) {
        if (s.id == id) {
            SendMessageW(GetDlgItem(dlg, id), TBM_SETPOS, TRUE, value);
            wchar_t buf[16];
            _snwprintf_s(buf, _TRUNCATE, L"%d", value);
            SetDlgItemTextW(dlg, s.val_id, buf);
            return;
        }
    }
}

int slider_pos(HWND dlg, int id) {
    return static_cast<int>(SendMessageW(GetDlgItem(dlg, id), TBM_GETPOS, 0, 0));
}

// ---------------------------------------------------------------------------
// Mapping from a per-voice slider's control id to the VoiceSettings member
// it edits, used only to *load* a voice's sliders (dectalk_config_logic.hpp
// has the parallel id -> registry-name table used to *write* one).
// ---------------------------------------------------------------------------

struct voice_slider_ctl { int id; int VoiceSettings::*member; };

const voice_slider_ctl VOICE_SLIDER_CTLS[] = {
    { IDC_PITCH,            &VoiceSettings::pitch },
    { IDC_INFLECTION,       &VoiceSettings::inflection },
    { IDC_HEADSIZE,         &VoiceSettings::head_size },
    { IDC_BREATHINESS,      &VoiceSettings::breathiness },
    { IDC_RICHNESS,         &VoiceSettings::richness },
    { IDC_SMOOTHNESS,       &VoiceSettings::smoothness },
    { IDC_LOUDNESS,         &VoiceSettings::loudness },
    { IDC_LARYNGEALIZATION, &VoiceSettings::laryngealization },
    { IDC_ASSERTIVENESS,    &VoiceSettings::assertiveness },
};

bool is_voice_slider(int id) {
    for (const voice_slider_ctl& c : VOICE_SLIDER_CTLS) {
        if (c.id == id) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Accessibility: pin an explicit accessible name onto every trackbar, combo
// box and value readout. A screen reader otherwise guesses a control's name
// from the nearest preceding static -- the .rc's resource order already
// makes that guess correct, but pinning the name removes the dependency on
// that layout heuristic entirely, and lets the announcement include the
// control's range (which the visible label also states).
// ---------------------------------------------------------------------------

void set_accessible_names(HWND dlg) {
    IAccPropServices* props = nullptr;
    if (FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IAccPropServices,
                                reinterpret_cast<void**>(&props))) || !props) {
        return;
    }
    const struct { int id; const wchar_t* name; } names[] = {
        { IDC_VOICE,                  L"Voice" },
        { IDC_PITCH,                  L"Pitch, 0 to 100" },
        { IDC_PITCH_VAL,              L"Pitch value" },
        { IDC_INFLECTION,             L"Inflection, 0 to 100" },
        { IDC_INFLECTION_VAL,         L"Inflection value" },
        { IDC_HEADSIZE,               L"Head size, 0 to 100" },
        { IDC_HEADSIZE_VAL,           L"Head size value" },
        { IDC_BREATHINESS,            L"Breathiness, 0 to 100" },
        { IDC_BREATHINESS_VAL,        L"Breathiness value" },
        { IDC_RICHNESS,               L"Richness, 0 to 100" },
        { IDC_RICHNESS_VAL,           L"Richness value" },
        { IDC_SMOOTHNESS,             L"Smoothness, 0 to 100" },
        { IDC_SMOOTHNESS_VAL,         L"Smoothness value" },
        { IDC_LOUDNESS,               L"Loudness, 0 to 100" },
        { IDC_LOUDNESS_VAL,           L"Loudness value" },
        { IDC_LARYNGEALIZATION,       L"Laryngealization, 0 to 100" },
        { IDC_LARYNGEALIZATION_VAL,   L"Laryngealization value" },
        { IDC_ASSERTIVENESS,          L"Assertiveness, 0 to 100" },
        { IDC_ASSERTIVENESS_VAL,      L"Assertiveness value" },
        { IDC_RATE,                   L"Rate, 25 to 400 percent" },
        { IDC_RATE_VAL,               L"Rate value" },
        { IDC_VOLUME,                 L"Volume adjustment, -40 to 12 dB" },
        { IDC_VOLUME_VAL,             L"Volume adjustment value" },
        { IDC_RATEBOOST,              L"Rate boost, 0 to 200 percent" },
        { IDC_RATEBOOST_VAL,          L"Rate boost value" },
        { IDC_FIRMWARE,               L"Default firmware" },
    };
    for (const auto& n : names) {
        if (HWND ctl = GetDlgItem(dlg, n.id)) {
            props->SetHwndPropStr(ctl, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, n.name);
        }
    }
    props->Release();
}

// ---------------------------------------------------------------------------
// Loading control state from the registry.
// ---------------------------------------------------------------------------

void load_voice_controls(HWND dlg) {
    const dtc01::VoiceDef& v = current_voice();
    const VoiceSettings s = dectalk::settings::load_voice(v.key, v.firmware);
    for (const voice_slider_ctl& c : VOICE_SLIDER_CTLS) {
        set_slider(dlg, c.id, s.*(c.member));
    }
}

void load_global_controls(HWND dlg) {
    const GlobalSettings g = dectalk::settings::load_global();
    set_slider(dlg, IDC_RATE, g.rate_percent);
    set_slider(dlg, IDC_VOLUME, g.volume_db);
    set_slider(dlg, IDC_RATEBOOST, g.rate_boost);
    SendDlgItemMessageW(dlg, IDC_FIRMWARE, CB_SETCURSEL,
                        g.default_firmware == "v18" ? 1 : 0, 0);
}

// ---------------------------------------------------------------------------
// Preview through SAPI: resolves the currently-selected voice's own token by
// the id the installer registers it under (voice_registry.hpp), the same
// path Narrator or any other SAPI5 client would take.
// ---------------------------------------------------------------------------

void play_sample(HWND dlg) {
    const dtc01::VoiceDef& v = current_voice();

    HRESULT hr = S_OK;
    if (!g_preview) {
        hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                              __uuidof(ISpVoice), reinterpret_cast<void**>(&g_preview));
    }
    if (FAILED(hr) || !g_preview) {
        MessageBoxW(dlg, L"Play sample requires the voices to be installed.",
                    L"DECtalk DTC-01 Configuration", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring id = std::wstring(L"HKEY_LOCAL_MACHINE\\") +
                            dectalk::sapi::voices_path + L"\\" +
                            dectalk::sapi::token_id_for(v);

    ISpObjectToken* token = nullptr;
    hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                          __uuidof(ISpObjectToken), reinterpret_cast<void**>(&token));
    if (SUCCEEDED(hr)) {
        hr = token->SetId(nullptr, id.c_str(), FALSE);
    }
    if (SUCCEEDED(hr)) {
        hr = g_preview->SetVoice(token);
    }
    if (token) {
        token->Release();
    }
    if (FAILED(hr)) {
        // Not yet registered (voices are registered by the installer) or SAPI
        // otherwise refused the token -- either way, tell the user rather
        // than crash the utility.
        MessageBoxW(dlg, L"Play sample requires the voices to be installed.",
                    L"DECtalk DTC-01 Configuration", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring text = L"DECtalk D T C zero one sample. This is ";
    text += v.display;
    text += L".";
    g_preview->Speak(text.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
}

// ---------------------------------------------------------------------------
// Dialog proc
// ---------------------------------------------------------------------------

// DS_CENTER sizes and centers the window from the DIALOGEX template's own
// declared extent (480x400 DLU) -- but that sizing was observed, on a
// scaled-DPI display, to use a slightly different DLU-to-pixel factor than
// the one used to place the CHILDREN (see IDD_MAIN's controls in
// dectalk_config.rc), leaving the window a little short of its own content:
// the Advanced group and every button below it landed just past the
// window's bottom-right edge -- still present and reachable by Tab, but
// invisible and unclickable. MapDialogRect uses the dialog's actual font to
// convert the SAME template extent, on the SAME window, so re-deriving the
// window size from it (rather than trusting the size DialogBoxParam already
// picked) guarantees the window is exactly as big as its own children.
void fit_window_to_own_template(HWND dlg) {
    RECT r = { 0, 0, 480, 400 };
    MapDialogRect(dlg, &r);
    AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(dlg, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(dlg, GWL_EXSTYLE)));
    const int width = r.right - r.left;
    const int height = r.bottom - r.top;

    // Re-center on the primary monitor now that the true size is known,
    // rather than leave the window wherever its too-small initial placement
    // put it. Clamped on both ends (not just the top-left) so a dialog
    // taller or wider than the screen still opens with its top-left corner
    // on-screen, rather than centered enough to push its bottom-right --
    // including the buttons -- past the visible edge.
    const int screen_w = GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_w - width) / 2;
    int y = (screen_h - height) / 2;
    x = std::max(0, std::min(x, screen_w - width));
    y = std::max(0, std::min(y, screen_h - height));

    SetWindowPos(dlg, nullptr, x, y, width, height, SWP_NOZORDER);
}

void on_init(HWND dlg) {
    HWND voice_combo = GetDlgItem(dlg, IDC_VOICE);
    for (int i = 0; i < dtc01::voice_count(); ++i) {
        const dtc01::VoiceDef& v = dtc01::VOICES[i];
        std::wstring label = v.display;
        label += L" (";
        label += dectalk::sapi::detail::firmware_label(v.firmware);
        label += L")";
        const int item = static_cast<int>(
            SendMessageW(voice_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
        SendMessageW(voice_combo, CB_SETITEMDATA, item, i);
    }
    g_voice_index = 0;
    SendMessageW(voice_combo, CB_SETCURSEL, 0, 0);

    HWND fw_combo = GetDlgItem(dlg, IDC_FIRMWARE);
    SendMessageW(fw_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"v2.0"));
    SendMessageW(fw_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"v1.8"));

    for (const slider_range& s : SLIDERS) {
        init_slider(dlg, s);
    }

    load_voice_controls(dlg);
    load_global_controls(dlg);
    set_accessible_names(dlg);
    fit_window_to_own_template(dlg);
}

// Applies one slider's new value: the registry write (dectalk_config_logic)
// plus its own live readout, whether it is a per-voice or a global slider.
void on_slider_changed(HWND dlg, HWND slider) {
    const int id = GetDlgCtrlID(slider);
    const int value = slider_pos(dlg, id);
    set_slider(dlg, id, value);

    if (is_voice_slider(id)) {
        const dtc01::VoiceDef& v = current_voice();
        (void)dectalk_config::apply_setting(v.key, v.firmware, id, value);
    } else {
        (void)dectalk_config::apply_global_setting(id, value);
    }
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG:
        on_init(dlg);
        return TRUE;

    case WM_HSCROLL:
        if (lparam) {
            on_slider_changed(dlg, reinterpret_cast<HWND>(lparam));
        }
        return TRUE;

    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);

        if (id == IDC_VOICE && code == CBN_SELCHANGE) {
            const int cur = static_cast<int>(SendDlgItemMessageW(dlg, IDC_VOICE, CB_GETCURSEL, 0, 0));
            const int data = static_cast<int>(SendDlgItemMessageW(dlg, IDC_VOICE, CB_GETITEMDATA, cur, 0));
            if (cur >= 0 && data != CB_ERR) {
                g_voice_index = data;
                load_voice_controls(dlg);
            }
            return TRUE;
        }
        if (id == IDC_FIRMWARE && code == CBN_SELCHANGE) {
            const int cur = static_cast<int>(SendDlgItemMessageW(dlg, IDC_FIRMWARE, CB_GETCURSEL, 0, 0));
            (void)dectalk_config::apply_global_firmware(cur == 1 ? L"v18" : L"v20");
            return TRUE;
        }
        if (id == IDC_RESET_VOICE && code == BN_CLICKED) {
            const dtc01::VoiceDef& v = current_voice();
            dectalk::settings::reset_voice(v.key, v.firmware);
            load_voice_controls(dlg);
            return TRUE;
        }
        if (id == IDC_RESET_ALL && code == BN_CLICKED) {
            if (MessageBoxW(dlg,
                            L"Reset every voice and every speech setting to the "
                            L"DECtalk DTC-01 defaults?",
                            L"DECtalk DTC-01 Configuration",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                dectalk::settings::reset_all();
                load_voice_controls(dlg);
                load_global_controls(dlg);
            }
            return TRUE;
        }
        if (id == IDC_PREVIEW && code == BN_CLICKED) {
            play_sample(dlg);
            return TRUE;
        }
        if ((id == IDOK || id == IDCANCEL) &&
            (code == BN_CLICKED || code == 0)) {
            // Everything else was already written as it changed; there is
            // nothing left to save.
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }

    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Without this, an unaware process on a scaled-DPI display gets its
    // dialog bitmap stretched by the OS -- and critically, the window frame
    // and its child controls were observed to be stretched by DIFFERENT
    // factors, so the bottom of the dialog (the Advanced group and every
    // button) rendered well below the window's own visible client area:
    // still present and still reachable by Tab, but invisible and
    // unclickable. Declaring DPI awareness makes Windows lay out the window
    // and its children from the same, real DPI, eliminating the mismatch.
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialog_proc, 0);

    if (g_preview) {
        g_preview->Release();
        g_preview = nullptr;
    }
    CoUninitialize();
    return 0;
}
