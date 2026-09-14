// latency_probe.cpp - measures what a screen reader user waits for when moving
// quickly through items: each utterance is interrupted shortly after its first
// sound is heard, the way the next key press interrupts it.
//
// Drives the real DectalkDtc01SAPI.dll through DllGetClassObject (no
// registration), like cancel_probe.cpp. The site plays the stream back on a
// virtual real-time clock that starts at the first Write and -- like SAPI's
// audio queue -- holds Write while more than half a second is queued ahead of
// playback, so an abort lands where a real host's would: mid-stream.
//
//   latency_probe <DectalkDtc01SAPI.dll> <voice> <firmware>
//
// The engine resolves its ROM dir / core DLL from DTC01_ROM_DIR /
// DTC01_CORE_DLL, as for the other probes. Per utterance it reports:
//   lead   audio written ahead of the first audible sample (ms of audio)
//   sound  Speak() called -> that sample heard (ms)
//   stop   abort -> Speak() returned (ms)
//   key    stop of the previous item + sound of this one: the wait between
//          pressing a key and hearing the next item.

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kAudible = 256;          // |sample| that counts as sound
constexpr double kBufferMs = 500.0;    // audio a host queues ahead of playback
constexpr double kDwellMs = 100.0;     // how long the user listens before moving on

double now_ms()
{
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// --- a data key holding just the attributes SetObjectToken reads -----------
// (the same stand-in cancel_probe.cpp and sapi_probe.cpp use.)
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

// --- a token whose only job is to hand back that attributes key ------------
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

// --- the site: real-time playback on a virtual clock -----------------------
class PlaybackSite : public ISpTTSEngineSite
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
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override { *interest = 0; return S_OK; }

    STDMETHOD_(DWORD, GetActions)() override { return abort_ ? SPVES_ABORT : SPVES_CONTINUE; }

    STDMETHOD(Write)(const void* data, ULONG count, ULONG* written) override
    {
        const double now = now_ms();
        const auto* pcm = static_cast<const int16_t*>(data);
        const long long n = count / sizeof(int16_t);
        if (samples_ == 0) {
            first_write_ms_ = now;
        }
        for (long long i = 0; i < n; ++i) {
            if (std::abs(static_cast<int>(pcm[i])) > kAudible) {
                if (first_audible_ < 0) {
                    first_audible_ = samples_ + i;
                    // Heard when playback reaches it -- or now, if it arrived late.
                    heard_ms_ = std::max(first_write_ms_ + first_audible_ / 10.0, now);
                }
                last_audible_ = samples_ + i;
            }
        }
        samples_ += n;
        if (written) *written = count;

        // Like SAPI's audio queue: hold the engine while too much is buffered.
        while (!abort_ && samples_ - (now_ms() - first_write_ms_) * 10.0 > kBufferMs * 10.0) {
            Sleep(1);
        }
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

    void abort() { abort_ = true; }
    double heard_ms() const { return heard_ms_; }
    long long samples() const { return samples_; }
    long long first_audible() const { return first_audible_; }
    long long last_audible() const { return last_audible_; }

private:
    LONG ref_ = 1;
    std::atomic<bool> abort_{false};
    std::atomic<double> heard_ms_{0.0};
    double first_write_ms_ = 0.0;
    long long samples_ = 0;
    long long first_audible_ = -1;
    long long last_audible_ = -1;
};

// CLSID of dtc01::sapi::DectalkTtsEngine (sapi/src/DectalkTtsEngine.hpp).
const CLSID CLSID_DectalkTtsEngine =
    { 0xE877CE12, 0xF153, 0x40DF, { 0xAE, 0xAC, 0x2A, 0xFA, 0x4D, 0x4A, 0x3D, 0xE8 } };

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

struct Engine {
    ISpTTSEngine* tts = nullptr;
    WAVEFORMATEX* wfx = nullptr;
};

bool make_engine(DllGetClassObjectFn get_class, const std::wstring& voice,
                 const std::wstring& firmware, Engine& out)
{
    IClassFactory* factory = nullptr;
    if (FAILED(get_class(CLSID_DectalkTtsEngine, IID_IClassFactory, (void**)&factory))) return false;
    HRESULT hr = factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), (void**)&out.tts);
    factory->Release();
    if (FAILED(hr)) return false;

    ISpObjectWithToken* with_token = nullptr;
    if (FAILED(out.tts->QueryInterface(__uuidof(ISpObjectWithToken), (void**)&with_token))) return false;
    auto* token = new ProbeToken(voice, firmware);
    hr = with_token->SetObjectToken(token);
    token->Release();
    with_token->Release();
    if (FAILED(hr)) return false;

    GUID fmt = {};
    return SUCCEEDED(out.tts->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmt, &out.wfx));
}

struct Result {
    double sound_ms = -1.0;  // Speak() -> first audible sample heard
    double lead_ms = -1.0;   // audio ahead of that sample
    double stop_ms = 0.0;    // abort -> Speak() returned; 0 if it had already finished
    double trail_ms = -1.0;  // audio after the last audible sample
    double speak_ms = 0.0;   // Speak() wall time
};

// interrupt: abort once the first sound has been heard for kDwellMs.
Result speak(Engine& e, const std::wstring& text, bool interrupt)
{
    SPVTEXTFRAG frag = {};
    frag.State.eAction = SPVA_Speak;
    frag.State.LangID = 0x409;
    frag.State.Volume = 100;
    frag.pTextStart = text.c_str();
    frag.ulTextLen = static_cast<ULONG>(text.size());

    auto* site = new PlaybackSite();
    std::atomic<bool> done{false};
    std::atomic<double> returned_ms{0.0};
    const double start_ms = now_ms();
    std::thread worker([&] {
        e.tts->Speak(SPF_DEFAULT, SPDFID_WaveFormatEx, e.wfx, &frag, site);
        returned_ms = now_ms();
        done = true;
    });

    Result r;
    double abort_ms = 0.0;
    bool finished_first = !interrupt;
    if (interrupt) {
        while (!done) {
            const double heard = site->heard_ms();
            if (heard > 0.0 && now_ms() >= heard + kDwellMs) break;
            if (now_ms() - start_ms > 15000.0) break;
            Sleep(1);
        }
        finished_first = done;
        abort_ms = now_ms();
        site->abort();
    }
    worker.join();

    if (site->first_audible() >= 0) {
        r.sound_ms = site->heard_ms() - start_ms;
        r.lead_ms = site->first_audible() / 10.0;
        r.trail_ms = (site->samples() - site->last_audible() - 1) / 10.0;
    }
    r.stop_ms = finished_first ? 0.0 : returned_ms - abort_ms;
    r.speak_ms = returned_ms - start_ms;
    site->Release();
    return r;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) {
        wprintf(L"usage: latency_probe <DectalkDtc01SAPI.dll> <voice> <firmware>\n");
        return 2;
    }
    const std::wstring voice = argv[2];
    const std::wstring firmware = argv[3];

    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE mod = LoadLibraryW(argv[1]);
    if (!mod) {
        wprintf(L"FAIL: LoadLibrary -> %lu\n", GetLastError());
        return 1;
    }
    auto get_class = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"));
    Engine e;
    if (!get_class || !make_engine(get_class, voice, firmware, e)) {
        wprintf(L"FAIL: could not create the engine\n");
        return 1;
    }

    // What NVDA says moving down a File Explorer list (from a real log).
    const std::vector<std::wstring> items = {
        L"Desktop  list", L"This PC  2 of 42", L"Joshua Kennedy  1 of 42", L"Documents  3 of 42",
        L"Downloads  4 of 42", L"Music  5 of 42", L"Pictures  6 of 42", L"Videos  7 of 42",
        L"Recycle Bin  8 of 42", L"Control Panel  9 of 42", L"o", L"Alfa",
    };

    const double first_ms = speak(e, L"Ready.", false).sound_ms;  // includes the boot
    wprintf(L"%s/%s: first utterance (boots the machine): sound %.0f ms\n\n",
            voice.c_str(), firmware.c_str(), first_ms);

    wprintf(L"  %-26s %7s %7s %7s %7s\n", L"item", L"lead", L"sound", L"stop", L"key");
    double key_sum = 0.0, key_max = 0.0;
    int key_n = 0;
    double prev_stop = -1.0;
    for (const std::wstring& item : items) {
        const Result r = speak(e, item, true);
        double key = -1.0;
        if (prev_stop >= 0.0 && r.sound_ms >= 0.0) {
            key = prev_stop + r.sound_ms;
            key_sum += key;
            key_max = std::max(key_max, key);
            ++key_n;
        }
        wprintf(L"  %-26s %7.0f %7.0f %7.1f %7.0f\n", item.c_str(), r.lead_ms, r.sound_ms, r.stop_ms, key);
        prev_stop = r.stop_ms;
    }
    wprintf(L"\n  key press to next item's sound: mean %.0f ms, worst %.0f ms\n",
            key_n ? key_sum / key_n : -1.0, key_max);

    const Result full = speak(e, L"Desktop  list", false);
    wprintf(L"  uninterrupted \"Desktop  list\": sound %.0f ms, silence after last sound %.0f ms, "
            L"Speak() %.0f ms\n", full.sound_ms, full.trail_ms, full.speak_ms);

    // A host creates a new engine instance for every voice change.
    Engine other;
    const std::wstring other_voice = voice == L"betty" ? L"paul" : L"betty";
    if (make_engine(get_class, other_voice, firmware, other)) {
        const Result r = speak(other, L"Beautiful Betty", true);
        wprintf(L"  new engine instance (%s): sound %.0f ms\n", other_voice.c_str(), r.sound_ms);
        CoTaskMemFree(other.wfx);
        other.tts->Release();
    }

    CoTaskMemFree(e.wfx);
    e.tts->Release();
    FreeLibrary(mod);
    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
