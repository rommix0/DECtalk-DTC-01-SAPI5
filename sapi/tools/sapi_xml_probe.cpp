// sapi_xml_probe.cpp - speaks SAPI XML through the real SAPI stack -- SpVoice,
// its XML parser and its audio queue -- into a WAV file, with the engine
// taken from this build rather than from whatever is installed.
//
// sapi_probe.cpp and the other probes hand the engine text fragments
// directly. A real host goes through SAPI instead, which turns markup such as
// <pitch absmiddle="15"> into the fragment state the engine sees; that is how
// NVDA sends its pitch setting and raises the pitch of capital letters, so
// this is the probe for anything markup-driven.
//
// DectalkDtc01SAPI.dll is loaded from beside this exe through a
// registration-free COM manifest (sapi_xml_probe.manifest), and the voice
// token is a temporary one under HKCU, deleted before exit. Nothing has to be
// installed, no admin rights are needed, and an installed copy is not used.
//
//   sapi_xml_probe <voice_key> <firmware> <out.wav> <xml>
//
// The engine resolves its ROM dir / core DLL from DTC01_ROM_DIR /
// DTC01_CORE_DLL, as for the other probes.

#include <windows.h>
#include <sapi.h>
#include <sperror.h>

#include <cstdio>
#include <string>

namespace {

constexpr const wchar_t* kProbeKey = L"Software\\DECtalkDTC01Probe";
constexpr const wchar_t* kTokenKey = L"Software\\DECtalkDTC01Probe\\Token";
constexpr const wchar_t* kTokenId = L"HKEY_CURRENT_USER\\Software\\DECtalkDTC01Probe\\Token";
// CLSID of dtc01::sapi::DectalkTtsEngine (sapi/src/DectalkTtsEngine.hpp); it
// must match the manifest's comClass.
constexpr const wchar_t* kEngineClsid = L"{E877CE12-F153-40DF-AEAC-2AFA4D4A3DE8}";

bool set_string(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// The same token shape voice_registry.hpp registers, under HKCU.
bool create_token(const std::wstring& voice, const std::wstring& firmware)
{
    HKEY token = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kTokenKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                        &token, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = set_string(token, nullptr, L"DECtalk DTC-01 probe") &&
              set_string(token, L"CLSID", kEngineClsid);
    HKEY attrs = nullptr;
    if (ok && RegCreateKeyExW(token, L"Attributes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                              &attrs, nullptr) == ERROR_SUCCESS) {
        ok = set_string(attrs, L"Name", L"DECtalk DTC-01 probe") &&
             set_string(attrs, L"Language", L"409") &&
             set_string(attrs, L"Gender", L"Male") &&
             set_string(attrs, L"Age", L"Adult") &&
             set_string(attrs, L"Vendor", L"DECtalk") &&
             set_string(attrs, L"DtcVoice", voice) &&
             set_string(attrs, L"DtcFirmware", firmware);
        RegCloseKey(attrs);
    } else {
        ok = false;
    }
    RegCloseKey(token);
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5) {
        wprintf(L"usage: sapi_xml_probe <voice_key> <firmware> <out.wav> <xml>\n");
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        wprintf(L"FAIL: CoInitializeEx -> 0x%08X\n", hr);
        return 1;
    }
    if (!create_token(argv[1], argv[2])) {
        wprintf(L"FAIL: could not create the temporary voice token under HKCU\n");
        RegDeleteTreeW(HKEY_CURRENT_USER, kProbeKey);
        CoUninitialize();
        return 1;
    }

    ISpObjectToken* token = nullptr;
    ISpVoice* voice = nullptr;
    ISpStream* stream = nullptr;
    const wchar_t* step = L"CoCreateInstance(SpObjectToken)";
    hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, __uuidof(ISpObjectToken),
                          reinterpret_cast<void**>(&token));
    if (SUCCEEDED(hr)) {
        step = L"ISpObjectToken::SetId";
        hr = token->SetId(nullptr, kTokenId, FALSE);
    }
    if (SUCCEEDED(hr)) {
        step = L"CoCreateInstance(SpVoice)";
        hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, __uuidof(ISpVoice),
                              reinterpret_cast<void**>(&voice));
    }
    if (SUCCEEDED(hr)) {
        step = L"ISpVoice::SetVoice";
        hr = voice->SetVoice(token);
    }
    if (SUCCEEDED(hr)) {
        step = L"CoCreateInstance(SpStream)";
        hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, __uuidof(ISpStream),
                              reinterpret_cast<void**>(&stream));
    }
    if (SUCCEEDED(hr)) {
        // The engine's own format, so SAPI has nothing to convert.
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 10000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = 20000;
        step = L"ISpStream::BindToFile";
        hr = stream->BindToFile(argv[3], SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0);
    }
    if (SUCCEEDED(hr)) {
        step = L"ISpVoice::SetOutput";
        hr = voice->SetOutput(stream, TRUE);
    }
    if (SUCCEEDED(hr)) {
        step = L"ISpVoice::Speak";
        hr = voice->Speak(argv[4], SPF_IS_XML, nullptr);
    }
    if (stream) {
        stream->Close();
        stream->Release();
    }
    if (voice) voice->Release();
    if (token) token->Release();
    RegDeleteTreeW(HKEY_CURRENT_USER, kProbeKey);

    if (FAILED(hr)) {
        wprintf(L"FAIL: %s -> 0x%08X\n", step, hr);
    } else {
        wprintf(L"%s/%s spoke through SAPI into %s\n", argv[1], argv[2], argv[3]);
    }
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
