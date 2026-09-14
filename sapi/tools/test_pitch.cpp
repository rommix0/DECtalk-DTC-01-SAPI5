// test_pitch.cpp - the host's control of rate, pitch and volume, through the
// real DectalkDtc01SAPI.dll (DllGetClassObject, no registration) like
// cancel_probe.cpp.
//
// One Speak() call carries fragments whose SAPI state differs the way host
// markup makes it differ -- NVDA raises the pitch for a capital letter and
// brings it back down, and sends its rate and volume the same way -- and each
// fragment's audio is measured: median F0 by autocorrelation, how long its
// speech lasts, and its RMS level over that span. Then "Allow SAPI5 apps to
// control rate, pitch and volume" is unticked, and the same call has to
// ignore the host and take the voice's own Pitch slider instead.
//
// Fragments are compared with a neighbour spoken in the same situation: the
// same text rendered twice varies by a few percent in level with what the
// firmware was doing before it, so a fragment is never held to the first one.
//
//   test_pitch <DectalkDtc01SAPI.dll>
//
// The engine resolves its ROM dir / core DLL from DTC01_ROM_DIR /
// DTC01_CORE_DLL; the ROM dir must hold both firmwares. The test runs from the
// default settings: whatever is saved under HKCU\Software\DECtalkDTC01 is put
// aside and restored afterwards (settings_backup.hpp).

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// --- a site that keeps the audio and where each fragment's audio starts -----
class CaptureSite : public ISpTTSEngineSite
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

    // The engine marks the start of every speaking fragment with a sentence
    // boundary at the stream offset its audio begins.
    STDMETHOD(AddEvents)(const SPEVENT* events, ULONG count) override
    {
        for (ULONG i = 0; i < count; ++i) {
            if (events[i].eEventId == SPEI_SENTENCE_BOUNDARY) {
                starts_.push_back(static_cast<size_t>(events[i].ullAudioStreamOffset / 2));
            }
        }
        return S_OK;
    }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override
    {
        *interest = SPFEI(SPEI_SENTENCE_BOUNDARY);
        return S_OK;
    }

    STDMETHOD_(DWORD, GetActions)() override { return SPVES_CONTINUE; }
    STDMETHOD(Write)(const void* data, ULONG count, ULONG* written) override
    {
        const auto* pcm = static_cast<const int16_t*>(data);
        pcm_.insert(pcm_.end(), pcm, pcm + count / 2);
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

    const std::vector<int16_t>& pcm() const { return pcm_; }
    const std::vector<size_t>& starts() const { return starts_; }

private:
    LONG ref_ = 1;
    std::vector<int16_t> pcm_;
    std::vector<size_t> starts_;
};

const CLSID CLSID_DectalkTtsEngine =
    { 0xE877CE12, 0xF153, 0x40DF, { 0xAE, 0xAC, 0x2A, 0xFA, 0x4D, 0x4A, 0x3D, 0xE8 } };

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

// Median F0 of the voiced 40 ms frames, by normalized autocorrelation over
// 50-450 Hz; 0 if nothing is voiced.
double median_f0(const int16_t* x, size_t n)
{
    constexpr size_t kFrame = 400, kHop = 100;
    constexpr int kMinLag = 10000 / 450, kMaxLag = 10000 / 50;
    std::vector<double> f0s;
    std::vector<double> w(kFrame);
    for (size_t s = 0; s + kFrame <= n; s += kHop) {
        double mean = 0.0;
        for (size_t i = 0; i < kFrame; ++i) mean += x[s + i];
        mean /= kFrame;
        double energy = 0.0;
        for (size_t i = 0; i < kFrame; ++i) {
            w[i] = x[s + i] - mean;
            energy += w[i] * w[i];
        }
        if (energy / kFrame < 400.0 * 400.0) continue;
        int best_lag = kMinLag;
        double best = -1.0;
        for (int lag = kMinLag; lag < kMaxLag; ++lag) {
            double r = 0.0;
            for (size_t i = 0; i + lag < kFrame; ++i) r += w[i] * w[i + lag];
            r /= energy;
            if (r > best) {
                best = r;
                best_lag = lag;
            }
        }
        if (best > 0.6) f0s.push_back(10000.0 / best_lag);
    }
    if (f0s.empty()) return 0.0;
    std::sort(f0s.begin(), f0s.end());
    return f0s[f0s.size() / 2];
}

struct Measure {
    double f0 = 0.0;
    double speech_s = 0.0;  // first to last sample louder than 256
    double rms = 0.0;       // over that span
};

struct FragState {
    long pitch;   // PitchAdj.MiddleAdj
    long rate;    // RateAdj
    USHORT volume;
};

const std::wstring kText = L"The rain in Spain stays mainly in the plain.";

std::vector<Measure> speak(ISpTTSEngine* engine, WAVEFORMATEX* wfx, const std::vector<FragState>& states)
{
    std::vector<SPVTEXTFRAG> frags(states.size());
    for (size_t i = 0; i < states.size(); ++i) {
        SPVTEXTFRAG& f = frags[i];
        f = {};
        f.pNext = i + 1 < states.size() ? &frags[i + 1] : nullptr;
        f.State.eAction = SPVA_Speak;
        f.State.LangID = 0x409;
        f.State.PitchAdj.MiddleAdj = states[i].pitch;
        f.State.RateAdj = states[i].rate;
        f.State.Volume = states[i].volume;
        f.pTextStart = kText.c_str();
        f.ulTextLen = static_cast<ULONG>(kText.size());
        f.ulTextSrcOffset = static_cast<ULONG>(i * (kText.size() + 1));
    }

    auto* site = new CaptureSite();
    const HRESULT hr = engine->Speak(SPF_DEFAULT, SPDFID_WaveFormatEx, wfx, frags.data(), site);
    std::vector<Measure> out;
    std::vector<size_t> bounds = site->starts();
    if (FAILED(hr) || bounds.size() != states.size()) {
        wprintf(L"  Speak -> 0x%08X with %zu fragment starts for %zu fragments\n", hr,
                bounds.size(), states.size());
        site->Release();
        return out;
    }
    const std::vector<int16_t>& pcm = site->pcm();
    bounds.push_back(pcm.size());
    for (size_t i = 0; i < states.size(); ++i) {
        Measure m;
        const int16_t* x = pcm.data() + bounds[i];
        const size_t n = bounds[i + 1] - bounds[i];
        m.f0 = median_f0(x, n);
        size_t first = n, last = 0;
        for (size_t k = 0; k < n; ++k) {
            if (std::abs(static_cast<int>(x[k])) > 256) {
                first = std::min(first, k);
                last = k;
            }
        }
        if (first < last) {
            m.speech_s = (last - first) / 10000.0;
            double sum = 0.0;
            for (size_t k = first; k <= last; ++k) sum += static_cast<double>(x[k]) * x[k];
            m.rms = std::sqrt(sum / (last - first + 1));
        }
        out.push_back(m);
    }
    site->Release();
    return out;
}

void print(const std::vector<Measure>& m, const std::vector<FragState>& s)
{
    for (size_t i = 0; i < m.size(); ++i) {
        wprintf(L"    pitch %+3ld rate %+2ld volume %3u : F0 %6.1f Hz, speech %5.2f s, RMS %6.0f\n",
                s[i].pitch, s[i].rate, s[i].volume, m[i].f0, m[i].speech_s, m[i].rms);
    }
}

int g_failures = 0;

void check(bool ok, const char* what)
{
    wprintf(L"  %hs %hs\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool near_ratio(double value, double reference, double tolerance)
{
    return reference > 0.0 && std::fabs(value - reference) <= reference * tolerance;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        wprintf(L"usage: test_pitch <DectalkDtc01SAPI.dll>\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE mod = LoadLibraryW(argv[1]);
    auto get_class = mod ? reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"))
                         : nullptr;
    if (!get_class) {
        wprintf(L"FAIL: could not load %s\n", argv[1]);
        return 1;
    }

    // The checks need the default settings: the ones already saved are put
    // aside first, and back when the test ends (settings_backup.hpp).
    const dectalk::test::SettingsBackup backup;
    if (!backup.ok()) {
        wprintf(L"FAIL: could not back up HKCU\\%s\n", dectalk::settings::ROOT_KEY);
        return 1;
    }

    // Normal, a capital letter's raised pitch, back to normal, lowered pitch,
    // half volume, faster. Fragments 2, 4 and 5 all follow a fragment that
    // changed the pitch and are spoken at the voice's own pitch, so they are
    // like for like.
    const std::vector<FragState> states = {
        {0, 0, 100}, {15, 0, 100}, {0, 0, 100}, {-10, 0, 100}, {0, 0, 50}, {0, 5, 100},
    };

    for (const wchar_t* firmware : {L"v20", L"v18"}) {
        RegDeleteTreeW(HKEY_CURRENT_USER, dectalk::settings::ROOT_KEY);
        IClassFactory* factory = nullptr;
        ISpTTSEngine* engine = nullptr;
        ISpObjectWithToken* with_token = nullptr;
        WAVEFORMATEX* wfx = nullptr;
        GUID fmt = {};
        auto* token = new ProbeToken(L"paul", firmware);
        bool ready = SUCCEEDED(get_class(CLSID_DectalkTtsEngine, IID_IClassFactory, (void**)&factory)) &&
                     SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), (void**)&engine)) &&
                     SUCCEEDED(engine->QueryInterface(__uuidof(ISpObjectWithToken), (void**)&with_token)) &&
                     SUCCEEDED(with_token->SetObjectToken(token)) &&
                     SUCCEEDED(engine->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmt, &wfx));
        if (factory) factory->Release();
        token->Release();
        if (!ready) {
            wprintf(L"FAIL: could not create the engine for paul/%s\n", firmware);
            return 1;
        }
        const char* fw = firmware[1] == L'2' ? "v20" : "v18";

        wprintf(L"paul/%s, the host in control (the default):\n", firmware);
        const std::vector<Measure> host = speak(engine, wfx, states);
        check(host.size() == states.size(), "every fragment spoke");
        if (host.size() == states.size()) {
            print(host, states);
            check(host[1].f0 > host[0].f0 * 1.25, "pitch +15 raises the voice");
            check(near_ratio(host[2].f0, host[0].f0, 0.06), "pitch back to 0 restores the voice's own pitch");
            check(host[3].f0 < host[0].f0 * 0.9, "pitch -10 lowers the voice");
            check(near_ratio(host[4].rms, host[2].rms * 0.5, 0.1), "volume 50 halves the level");
            check(host[5].speech_s < host[2].speech_s * 0.85, "rate +5 speaks faster");
        }

        dectalk::settings::write_global_int(dectalk::settings::APP_CONTROL, 0);
        wprintf(L"paul/%s, \"Allow SAPI5 apps to control rate, pitch and volume\" unticked:\n", firmware);
        const std::vector<Measure> fixed = speak(engine, wfx, states);
        check(fixed.size() == states.size(), "every fragment spoke");
        if (fixed.size() == states.size()) {
            print(fixed, states);
            // With the host ignored, fragments 2-6 are the same command and
            // must come out alike; the first differs only by following the
            // voice selection.
            bool same = near_ratio(fixed[0].f0, fixed[1].f0, 0.06) &&
                        near_ratio(fixed[0].speech_s, fixed[1].speech_s, 0.1);
            for (size_t i = 2; i < fixed.size(); ++i) {
                same = same && near_ratio(fixed[i].f0, fixed[1].f0, 0.04) &&
                       near_ratio(fixed[i].rms, fixed[1].rms, 0.05) &&
                       near_ratio(fixed[i].speech_s, fixed[1].speech_s, 0.05);
            }
            check(same, "the host's pitch, volume and rate are ignored");
        }

        dectalk::settings::write_voice_int("paul", fw, L"Pitch", 80);
        const std::vector<Measure> slider = speak(engine, wfx, {{0, 0, 100}});
        check(slider.size() == 1 && !fixed.empty() && slider[0].f0 > fixed[0].f0 * 1.25,
              "the voice's own Pitch slider (80) applies instead");

        CoTaskMemFree(wfx);
        with_token->Release();
        engine->Release();
    }

    FreeLibrary(mod);
    CoUninitialize();
    wprintf(L"test_pitch: %hs\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
