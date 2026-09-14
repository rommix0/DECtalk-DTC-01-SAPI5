// test_log_engine.cpp - the diagnostic log follows its setting from one
// utterance to the next, through the real DectalkDtc01SAPI.dll
// (DllGetClassObject, no registration) like test_pitch.cpp.
//
// The configuration utility's "Diagnostic log for bug reports" box writes
// HKCU\Software\DECtalkDTC01 Logging while a screen reader has the engine
// loaded, so the engine has to pick the change up on its next Speak() rather
// than when the host restarts. One engine speaks four words -- with no Logging
// value, then Logging = 1, 0 and 1 -- and the log must hold the two spoken
// while it was on, and only those.
//
//   test_log_engine <DectalkDtc01SAPI.dll>
//
// The engine resolves its ROM dir / core DLL from DTC01_ROM_DIR /
// DTC01_CORE_DLL. LOCALAPPDATA points at a scratch folder for this process, so
// a real %LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log is never touched, and
// the settings already saved are put aside and restored afterwards
// (settings_backup.hpp). Logging is a per-user setting, though: a screen
// reader speaking with DECtalk while this runs may log a line or two to its
// own log.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <share.h>

#include <cstdio>
#include <string>

#include "user_settings.hpp"
#include "settings_backup.hpp"

namespace {

// --- the token stand-in the other probes use --------------------------------
class ProbeDataKey : public ISpDataKey
{
public:
    ProbeDataKey(const std::wstring& voice, const std::wstring& firmware)
        : voice_(voice), firmware_(firmware) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISpDataKey)) {
            *ppv = static_cast<ISpDataKey*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override
    {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHOD(GetStringValue)(LPCWSTR name, LPWSTR* value) override
    {
        const wchar_t* found = nullptr;
        if (name && _wcsicmp(name, L"DtcVoice") == 0) found = voice_.c_str();
        else if (name && _wcsicmp(name, L"DtcFirmware") == 0) found = firmware_.c_str();
        if (!found) return SPERR_NOT_FOUND;

        const size_t bytes = (wcslen(found) + 1) * sizeof(wchar_t);
        *value = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!*value) return E_OUTOFMEMORY;
        memcpy(*value, found, bytes);
        return S_OK;
    }

    STDMETHOD(SetData)(LPCWSTR, ULONG, const BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(GetData)(LPCWSTR, ULONG*, BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(SetStringValue)(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(SetDWORD)(LPCWSTR, DWORD) override { return E_NOTIMPL; }
    STDMETHOD(GetDWORD)(LPCWSTR, DWORD*) override { return E_NOTIMPL; }
    STDMETHOD(OpenKey)(LPCWSTR, ISpDataKey**) override { return SPERR_NOT_FOUND; }
    STDMETHOD(CreateKey)(LPCWSTR, ISpDataKey**) override { return E_NOTIMPL; }
    STDMETHOD(DeleteKey)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(DeleteValue)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(EnumKeys)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }
    STDMETHOD(EnumValues)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }

private:
    LONG ref_ = 1;
    std::wstring voice_, firmware_;
};

class ProbeToken : public ISpObjectToken
{
public:
    ProbeToken(const std::wstring& voice, const std::wstring& firmware)
        : voice_(voice), firmware_(firmware) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISpDataKey) ||
            riid == __uuidof(ISpObjectToken)) {
            *ppv = static_cast<ISpObjectToken*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override
    {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHOD(OpenKey)(LPCWSTR name, ISpDataKey** key) override
    {
        if (!name || _wcsicmp(name, L"Attributes") != 0) return SPERR_NOT_FOUND;
        *key = new ProbeDataKey(voice_, firmware_);
        return S_OK;
    }

    STDMETHOD(SetData)(LPCWSTR, ULONG, const BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(GetData)(LPCWSTR, ULONG*, BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(SetStringValue)(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(GetStringValue)(LPCWSTR, LPWSTR*) override { return SPERR_NOT_FOUND; }
    STDMETHOD(SetDWORD)(LPCWSTR, DWORD) override { return E_NOTIMPL; }
    STDMETHOD(GetDWORD)(LPCWSTR, DWORD*) override { return E_NOTIMPL; }
    STDMETHOD(CreateKey)(LPCWSTR, ISpDataKey**) override { return E_NOTIMPL; }
    STDMETHOD(DeleteKey)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(DeleteValue)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(EnumKeys)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }
    STDMETHOD(EnumValues)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }

    STDMETHOD(SetId)(LPCWSTR, LPCWSTR, BOOL) override { return E_NOTIMPL; }
    STDMETHOD(GetId)(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(GetCategory)(ISpObjectTokenCategory**) override { return E_NOTIMPL; }
    STDMETHOD(CreateInstance)(IUnknown*, DWORD, REFIID, void**) override { return E_NOTIMPL; }
    STDMETHOD(GetStorageFileName)(REFCLSID, LPCWSTR, LPCWSTR, ULONG, LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(RemoveStorageFileName)(REFCLSID, LPCWSTR, BOOL) override { return E_NOTIMPL; }
    STDMETHOD(Remove)(const CLSID*) override { return E_NOTIMPL; }
    STDMETHOD(IsUISupported)(LPCWSTR, void*, ULONG, IUnknown*, BOOL*) override { return E_NOTIMPL; }
    STDMETHOD(DisplayUI)(HWND, LPCWSTR, LPCWSTR, void*, ULONG, IUnknown*) override { return E_NOTIMPL; }
    STDMETHOD(MatchesAttributes)(LPCWSTR, BOOL*) override { return E_NOTIMPL; }

private:
    LONG ref_ = 1;
    std::wstring voice_, firmware_;
};

// --- a site that only counts the audio ---------------------------------------
class CountingSite : public ISpTTSEngineSite
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISpEventSink) ||
            riid == __uuidof(ISpTTSEngineSite)) {
            *ppv = static_cast<ISpTTSEngineSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override
    {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHOD(AddEvents)(const SPEVENT*, ULONG) override { return S_OK; }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override
    {
        *interest = 0;
        return S_OK;
    }
    STDMETHOD_(DWORD, GetActions)() override { return SPVES_CONTINUE; }
    STDMETHOD(Write)(const void*, ULONG count, ULONG* written) override
    {
        bytes_ += count;
        if (written) *written = count;
        return S_OK;
    }
    STDMETHOD(GetRate)(long* rate) override { *rate = 0; return S_OK; }
    STDMETHOD(GetVolume)(USHORT* volume) override { *volume = 100; return S_OK; }
    STDMETHOD(GetSkipInfo)(SPVSKIPTYPE* type, long* items) override
    {
        *type = SPVST_SENTENCE;
        *items = 0;
        return S_OK;
    }
    STDMETHOD(CompleteSkip)(long) override { return S_OK; }

    ULONGLONG bytes() const { return bytes_; }

private:
    LONG ref_ = 1;
    ULONGLONG bytes_ = 0;
};

const CLSID CLSID_DectalkTtsEngine =
    { 0xE877CE12, 0xF153, 0x40DF, { 0xAE, 0xAC, 0x2A, 0xFA, 0x4D, 0x4A, 0x3D, 0xE8 } };

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

int g_failures = 0;

void check(bool ok, const char* what)
{
    wprintf(L"  %hs %hs\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// Logging = value, or no Logging value at all for -1.
void set_logging(int value)
{
    if (value >= 0) {
        dectalk::settings::write_global_int(dectalk::settings::LOGGING, value);
        return;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, dectalk::settings::ROOT_KEY, 0, KEY_SET_VALUE, &key) ==
        ERROR_SUCCESS) {
        RegDeleteValueW(key, dectalk::settings::LOGGING);
        RegCloseKey(key);
    }
}

// The whole log, or "" if there is none. The engine keeps the file open for
// appending with every kind of sharing allowed, so it can be read meanwhile.
std::string read_file(const std::wstring& path)
{
    std::string out;
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return out;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) out.append(chunk, n);
    fclose(f);
    return out;
}

int count(const std::string& text, const char* needle)
{
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

// One Speak() call of one fragment; true if it succeeded with audio.
bool speak(ISpTTSEngine* engine, WAVEFORMATEX* wfx, const std::wstring& text)
{
    SPVTEXTFRAG frag = {};
    frag.State.eAction = SPVA_Speak;
    frag.State.LangID = 0x409;
    frag.State.Volume = 100;
    frag.pTextStart = text.c_str();
    frag.ulTextLen = static_cast<ULONG>(text.size());

    auto* site = new CountingSite();
    const HRESULT hr = engine->Speak(SPF_DEFAULT, SPDFID_WaveFormatEx, wfx, &frag, site);
    const bool spoke = SUCCEEDED(hr) && site->bytes() > 0;
    site->Release();
    return spoke;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        wprintf(L"usage: test_log_engine <DectalkDtc01SAPI.dll>\n");
        return 2;
    }

    const dectalk::test::SettingsBackup backup;
    if (!backup.ok()) {
        wprintf(L"FAIL: could not back up HKCU\\%s\n", dectalk::settings::ROOT_KEY);
        return 1;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, dectalk::settings::ROOT_KEY);

    // Before the engine loads, so nothing it logs can reach a real log.
    wchar_t temp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp) == 0) {
        wprintf(L"FAIL: no temp folder\n");
        return 1;
    }
    const std::wstring scratch = std::wstring(temp) + L"dectalk-test-log-engine";
    CreateDirectoryW(scratch.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
    const std::wstring log_path = scratch + L"\\DECtalkDTC01\\dectalk-sapi.log";
    DeleteFileW(log_path.c_str());
    DeleteFileW((scratch + L"\\DECtalkDTC01\\dectalk-sapi.1.log").c_str());

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE mod = LoadLibraryW(argv[1]);
    auto get_class = mod ? reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"))
                         : nullptr;
    if (!get_class) {
        wprintf(L"FAIL: could not load %s\n", argv[1]);
        return 1;
    }

    IClassFactory* factory = nullptr;
    ISpTTSEngine* engine = nullptr;
    ISpObjectWithToken* with_token = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    GUID fmt = {};
    auto* token = new ProbeToken(L"paul", L"v20");
    const bool ready = SUCCEEDED(get_class(CLSID_DectalkTtsEngine, IID_IClassFactory, (void**)&factory)) &&
                       SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), (void**)&engine)) &&
                       SUCCEEDED(engine->QueryInterface(__uuidof(ISpObjectWithToken), (void**)&with_token)) &&
                       SUCCEEDED(with_token->SetObjectToken(token)) &&
                       SUCCEEDED(engine->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmt, &wfx));
    if (factory) factory->Release();
    token->Release();
    if (!ready) {
        wprintf(L"FAIL: could not create the engine for paul/v20\n");
        return 1;
    }

    // The same engine throughout, as a screen reader would keep it.
    const struct {
        const wchar_t* text;
        const char* word;
        int logging;  // -1: no Logging value
    } steps[] = {
        { L"alpha", "alpha", -1 },
        { L"bravo", "bravo", 1 },
        { L"charlie", "charlie", 0 },
        { L"delta", "delta", 1 },
    };
    int logged = 0;
    for (const auto& step : steps) {
        set_logging(step.logging);
        const bool spoke = speak(engine, wfx, step.text);
        const bool on = step.logging == 1;
        if (on) ++logged;

        const std::string log = read_file(log_path);
        const bool has_word = log.find(step.word) != std::string::npos;
        const int done = count(log, "DectalkTtsEngine: Speak done");
        char what[128];
        _snprintf_s(what, _TRUNCATE, "%s, %s: %s", step.word,
                    step.logging < 0 ? "no Logging value" : on ? "Logging = 1" : "Logging = 0",
                    on ? "logged" : "not logged");
        const bool ok = spoke && has_word == on && done == logged;
        check(ok, what);
        if (!ok) {
            wprintf(L"       spoke=%d, word in log=%d, utterances logged=%d (expected %d)\n", spoke ? 1 : 0,
                    has_word ? 1 : 0, done, logged);
        }
        if (step.logging < 0) {
            check(GetFileAttributesW(log_path.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "no log file while there is no Logging value");
        }
    }

    CoTaskMemFree(wfx);
    with_token->Release();
    engine->Release();
    FreeLibrary(mod);
    CoUninitialize();
    wprintf(L"test_log_engine: %hs\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
