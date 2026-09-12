// cancel_probe.cpp - Task E2 cancel soak: proves the engine survives repeated
// mid-stream SPVES_ABORT cancellation without hanging and without leaving
// residual emulator state that corrupts the utterance that follows.
//
// Modeled on sapi_probe.cpp (Task B4): same in-memory ProbeToken/ProbeDataKey
// stand-in for ISpObjectToken/ISpDataKey (so SetObjectToken's real contract is
// satisfied with no registry involved), the same CLSID_DectalkTtsEngine
// literal, and the same DllGetClassObject load path (no regsvr32, no admin
// rights) -- see sapi_probe.cpp's header comment for the rationale.
//
// Unlike sapi_probe, the site here (AbortingSite) is a *cancelling* site: its
// GetActions() starts returning SPVES_ABORT once the accumulated PCM byte
// count crosses a configurable threshold, so Speak's abort checks (fragment
// boundary, audio pump, rate-boost drain -- DectalkTtsEngine.cpp) fire
// mid-stream, exactly like a SAPI host does when the user keeps moving focus.
// SPVES_SKIP is intentionally not exercised -- this engine does not handle it
// (documented in DectalkTtsEngine.cpp / the design spec); only SPVES_ABORT is
// in scope for this soak.
//
// One ISpTTSEngine instance is kept alive across all 60 iterations (the
// realistic reuse pattern -- the persistent Machine inside the engine is what
// must recover cleanly), and every Speak() call runs on a worker thread with
// a watchdog timeout on the main thread, since the engine holds a
// process-wide exec mutex and a truly stuck call would otherwise hang this
// tool forever. Speak calls are still issued strictly sequentially (the
// watchdog is only about detecting a single stuck call, never concurrency).
//
//   cancel_probe <DectalkDtc01SAPI.dll>
//
// The engine resolves its ROM dir / core DLL from DTC01_ROM_DIR /
// DTC01_CORE_DLL (see DectalkTtsEngine.cpp's resolve_rom_dir/resolve_core_dll);
// the caller (test_cancel_soak.sh) sets both before running this.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

// --- a data key holding just the attributes SetObjectToken reads -----------
// (identical to sapi_probe.cpp's ProbeDataKey; duplicated rather than shared
// since each tool is a standalone translation unit with no shared header).
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

// --- the abortable site: accepts all audio, fires SPVES_ABORT mid-stream ---
class AbortingSite : public ISpTTSEngineSite
{
public:
    // SIZE_MAX (well, ULLONG_MAX) means "never abort" -- a plain full render.
    explicit AbortingSite(ULONGLONG abort_after_bytes)
        : abort_after_bytes_(abort_after_bytes) {}

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

    // ISpEventSink -- the soak only needs audio byte counts.
    STDMETHOD(AddEvents)(const SPEVENT*, ULONG) override { return S_OK; }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override { *interest = 0; return S_OK; }

    // ISpTTSEngineSite
    STDMETHOD_(DWORD, GetActions)() override
    {
        return (bytes_ >= abort_after_bytes_) ? SPVES_ABORT : SPVES_CONTINUE;
    }
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
    ULONGLONG abort_after_bytes_;
    ULONGLONG bytes_ = 0;
};

// CLSID of dtc01::sapi::DectalkTtsEngine (sapi/src/DectalkTtsEngine.hpp).
// Hardcoded here (rather than pulling in DectalkTtsEngine.hpp/dtc01common) so
// this probe only needs DllGetClassObject from the built DLL, exactly like a
// real SAPI host that never links against the engine's sources.
const CLSID CLSID_DectalkTtsEngine =
    { 0xE877CE12, 0xF153, 0x40DF, { 0xAE, 0xAC, 0x2A, 0xFA, 0x4D, 0x4A, 0x3D, 0xE8 } };

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

// Reports a failure attributed to a specific iteration and exits the process
// with a non-zero code -- called only from the main thread.
[[noreturn]] void fail(int iteration, const char* what)
{
    wprintf(L"FAIL at iteration %d: %hs\n", iteration, what);
    fflush(stdout);
    std::exit(1);
}

// Runs one Speak() call on a worker thread and waits up to timeout_ms on the
// main thread. If Speak doesn't return in time, prints the hang and exits
// non-zero immediately -- the thread can't be safely killed, so std::exit is
// used deliberately: it runs static destructors but never unwinds this
// function's stack, so the still-joinable (and still-running) worker thread
// object is simply abandoned rather than triggering std::terminate.
HRESULT speak_with_watchdog(ISpTTSEngine* engine, const SPVTEXTFRAG* frag,
                             AbortingSite* site, const WAVEFORMATEX* wfx,
                             int iteration, DWORD timeout_ms)
{
    std::promise<HRESULT> prom;
    std::future<HRESULT> fut = prom.get_future();

    std::thread worker([&]() {
        const HRESULT hr = engine->Speak(SPF_DEFAULT, SPDFID_WaveFormatEx, wfx, frag, site);
        prom.set_value(hr);
    });

    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::timeout) {
        wprintf(L"HANG at iteration %d (Speak did not return within %lu ms)\n",
                iteration, static_cast<unsigned long>(timeout_ms));
        fflush(stdout);
        std::exit(1);
    }
    const HRESULT hr = fut.get();
    worker.join();
    return hr;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        wprintf(L"usage: cancel_probe <DectalkDtc01SAPI.dll>\n");
        return 2;
    }
    const wchar_t* dll_path = argv[1];

    constexpr DWORD kWatchdogMs = 30000;
    constexpr int kRounds = 60;
    const std::wstring text =
        L"The quick brown fox jumps over the lazy dog, testing cancellation recovery.";

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) {
        wprintf(L"FAIL: CoInitializeEx -> 0x%08X\n", hrInit);
        return 1;
    }

    HMODULE mod = LoadLibraryW(dll_path);
    if (!mod) {
        wprintf(L"FAIL: LoadLibrary(%s) -> %lu\n", dll_path, GetLastError());
        CoUninitialize();
        return 1;
    }
    auto get_class = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"));
    if (!get_class) {
        wprintf(L"FAIL: DllGetClassObject not exported\n");
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(CLSID_DectalkTtsEngine, IID_IClassFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        wprintf(L"FAIL: DllGetClassObject -> 0x%08X\n", hr);
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    // ONE engine instance for the whole soak (kept alive across all 60
    // iterations) -- this is the realistic reuse pattern and the only way the
    // recovery assertion means anything: it's the persistent Machine inside
    // this single instance that must survive repeated aborts cleanly.
    ISpTTSEngine* engine = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), (void**)&engine);
    factory->Release();
    if (FAILED(hr) || !engine) {
        wprintf(L"FAIL: CreateInstance(ISpTTSEngine) -> 0x%08X\n", hr);
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    ISpObjectWithToken* with_token = nullptr;
    hr = engine->QueryInterface(__uuidof(ISpObjectWithToken), (void**)&with_token);
    if (FAILED(hr) || !with_token) {
        wprintf(L"FAIL: QueryInterface(ISpObjectWithToken) -> 0x%08X\n", hr);
        engine->Release();
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    auto* token = new ProbeToken(L"paul", L"v20");
    hr = with_token->SetObjectToken(token);
    if (FAILED(hr)) {
        wprintf(L"FAIL: SetObjectToken(paul/v20) -> 0x%08X\n", hr);
        token->Release();
        with_token->Release();
        engine->Release();
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    GUID fmt_id = {};
    WAVEFORMATEX* wfx = nullptr;
    hr = engine->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmt_id, &wfx);
    if (FAILED(hr) || !wfx) {
        wprintf(L"FAIL: GetOutputFormat -> 0x%08X\n", hr);
        token->Release();
        with_token->Release();
        engine->Release();
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }
    wprintf(L"GetOutputFormat: tag=%u channels=%u rate=%lu bits=%u (want 1/1/10000/16)\n",
            wfx->wFormatTag, wfx->nChannels, wfx->nSamplesPerSec, wfx->wBitsPerSample);
    assert(wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->nChannels == 1 &&
           wfx->nSamplesPerSec == 10000 && wfx->wBitsPerSample == 16 &&
           "GetOutputFormat must be 1/1/10000/16");

    SPVTEXTFRAG frag = {};
    frag.pNext = nullptr;
    frag.State.eAction = SPVA_Speak;
    frag.State.LangID = 0x409;
    frag.State.EmphAdj = 0;
    frag.State.RateAdj = 0;
    frag.State.Volume = 100;
    frag.State.PitchAdj.MiddleAdj = 0;
    frag.State.PitchAdj.RangeAdj = 0;
    frag.State.SilenceMSecs = 0;
    frag.pTextStart = text.c_str();
    frag.ulTextLen = static_cast<ULONG>(text.size());
    frag.ulTextSrcOffset = 0;

    constexpr ULONGLONG kNeverAbort = ~static_cast<ULONGLONG>(0);

    // --- baseline: one full render, never aborted -------------------------
    ULONGLONG FULL = 0;
    {
        auto* site = new AbortingSite(kNeverAbort);
        hr = speak_with_watchdog(engine, &frag, site, wfx, -1, kWatchdogMs);
        FULL = site->bytes();
        wprintf(L"baseline: Speak -> 0x%08X | %llu bytes\n", hr,
                static_cast<unsigned long long>(FULL));
        site->Release();
        if (FAILED(hr)) fail(-1, "baseline Speak failed");
        if (FULL == 0) fail(-1, "baseline produced no audio");
    }

    // Abort thresholds to cycle through on the non-recovery iterations: a
    // spread across the utterance (1/4, 1/2, 3/4) plus a couple of very small
    // thresholds that abort almost immediately (200, 500 bytes).
    const ULONGLONG variants[] = { FULL / 4, FULL / 2, (FULL * 3) / 4, 200, 500 };
    size_t variant_idx = 0;

    const ULONGLONG lower_bound = static_cast<ULONGLONG>(FULL * 0.9);
    const ULONGLONG upper_bound = static_cast<ULONGLONG>(FULL * 1.1);

    int aborted_count = 0;
    int recovery_count = 0;

    // --- 60 iterations: mostly aborted, every 5th a full recovery check ---
    for (int i = 0; i < kRounds; ++i) {
        const bool is_recovery = (i % 5 == 4);
        const ULONGLONG threshold = is_recovery ? kNeverAbort : variants[variant_idx++ % 5];

        auto* site = new AbortingSite(threshold);
        hr = speak_with_watchdog(engine, &frag, site, wfx, i, kWatchdogMs);
        const ULONGLONG bytes = site->bytes();
        site->Release();

        wprintf(L"iter %2d | mode=%hs | bytes=%8llu | hr=0x%08X\n",
                i, is_recovery ? "full" : "abort", static_cast<unsigned long long>(bytes), hr);
        fflush(stdout);

        if (FAILED(hr)) fail(i, "Speak returned a failure HRESULT");

        if (is_recovery) {
            ++recovery_count;
            if (bytes < lower_bound || bytes > upper_bound) {
                fail(i, "recovery utterance was not full-length after prior aborts "
                        "(residual emulator state from an abort likely truncated/corrupted it)");
            }
        } else {
            ++aborted_count;
            if (bytes == 0) fail(i, "aborted utterance produced no audio at all");
            if (bytes >= FULL) fail(i, "aborted utterance was not actually truncated");
        }
    }

    // --- final full render after the loop ----------------------------------
    {
        auto* site = new AbortingSite(kNeverAbort);
        hr = speak_with_watchdog(engine, &frag, site, wfx, kRounds, kWatchdogMs);
        const ULONGLONG bytes = site->bytes();
        wprintf(L"final: Speak -> 0x%08X | %llu bytes\n", hr,
                static_cast<unsigned long long>(bytes));
        site->Release();
        if (FAILED(hr)) fail(kRounds, "final Speak failed");
        if (bytes < lower_bound || bytes > upper_bound) {
            fail(kRounds, "final render was not full-length after the soak");
        }
    }

    // --- multi-fragment fragment-boundary abort case -----------------------
    // Everything above uses a single SPVTEXTFRAG per Speak() call, so every
    // real abort fires inside the audio-pump loop; the *other* abort check in
    // Speak() -- at the top of the per-fragment for-loop, evaluated when
    // advancing from one fragment to the next -- is never exercised. That
    // path is exactly where the fragment-boundary `aborted = true` fix
    // (DectalkTtsEngine.cpp, ~line 457) matters: without it, an abort that
    // lands between fragments (after an earlier fragment already fed real
    // speech to the persistent machine_) would skip the reset-and-recover
    // cleanup entirely. Build a two-fragment chain and land the abort right
    // on that boundary.
    //
    // Reasoning for why threshold == F1 (frag 1's own standalone byte total)
    // lands the abort *exactly* on the boundary rather than mid-pump: a
    // never-aborting site's fragment ends via the pump's bottom-of-loop
    // idle_runs check, never via a GetActions() abort check -- so the engine
    // never queries GetActions() again after the write that brings the
    // running total to F1. The *next* time GetActions() is queried is the
    // top-of-for-loop check for fragment 2, which is therefore the first
    // point where "bytes_ >= F1" can be observed. If that reasoning holds,
    // the aborted two-fragment call's byte count should come out exactly F1
    // (not a single fragment-2 block more) -- checked below.
    const std::wstring frag1_text = L"First fragment of speech.";
    const std::wstring frag2_text = L"Second fragment here.";

    SPVTEXTFRAG frag1_only = frag;
    frag1_only.pTextStart = frag1_text.c_str();
    frag1_only.ulTextLen = static_cast<ULONG>(frag1_text.size());
    frag1_only.ulTextSrcOffset = 0;

    SPVTEXTFRAG frag2b = frag;
    frag2b.pNext = nullptr;
    frag2b.pTextStart = frag2_text.c_str();
    frag2b.ulTextLen = static_cast<ULONG>(frag2_text.size());
    frag2b.ulTextSrcOffset = static_cast<ULONG>(frag1_text.size()) + 1;

    SPVTEXTFRAG frag1b = frag;
    frag1b.pNext = &frag2b;
    frag1b.pTextStart = frag1_text.c_str();
    frag1b.ulTextLen = static_cast<ULONG>(frag1_text.size());
    frag1b.ulTextSrcOffset = 0;

    // Measure fragment 1 standalone (to pick the boundary threshold) and the
    // full two-fragment render (to know what "not truncated" would be).
    ULONGLONG F1 = 0;
    {
        auto* site = new AbortingSite(kNeverAbort);
        hr = speak_with_watchdog(engine, &frag1_only, site, wfx, kRounds + 1, kWatchdogMs);
        F1 = site->bytes();
        wprintf(L"boundary-setup: frag1-only Speak -> 0x%08X | %llu bytes\n", hr,
                static_cast<unsigned long long>(F1));
        site->Release();
        if (FAILED(hr)) fail(kRounds + 1, "frag1-only measurement Speak failed");
        if (F1 == 0) fail(kRounds + 1, "frag1-only measurement produced no audio");
    }

    ULONGLONG TWO_FRAG_FULL = 0;
    {
        auto* site = new AbortingSite(kNeverAbort);
        hr = speak_with_watchdog(engine, &frag1b, site, wfx, kRounds + 2, kWatchdogMs);
        TWO_FRAG_FULL = site->bytes();
        wprintf(L"boundary-setup: two-fragment full Speak -> 0x%08X | %llu bytes\n", hr,
                static_cast<unsigned long long>(TWO_FRAG_FULL));
        site->Release();
        if (FAILED(hr)) fail(kRounds + 2, "two-fragment full measurement Speak failed");
        if (TWO_FRAG_FULL <= F1) {
            fail(kRounds + 2, "two-fragment full render was not longer than fragment 1 alone");
        }
    }

    // The actual boundary-abort case: threshold == F1 exactly.
    bool hit_boundary = false;
    {
        auto* site = new AbortingSite(F1);
        hr = speak_with_watchdog(engine, &frag1b, site, wfx, kRounds + 3, kWatchdogMs);
        const ULONGLONG bytes = site->bytes();
        site->Release();

        // Exact equality is the real guarantee, not just a likely outcome: a
        // never-aborting fragment ends via the pump loop's bottom-of-loop
        // idle_runs check, never a GetActions() abort check, so the engine
        // never re-queries GetActions() once a fragment's own total reaches
        // F1 -- the next query is the top-of-for-loop check for fragment 2.
        // A site that aborts once bytes_ >= F1 therefore CANNOT fire inside
        // fragment 1's own pump; the threshold is only reachable exactly at
        // the fragment-boundary check, with zero fragment-2 bytes written.
        // Measured empirically (see the E2 report): bytes came back == F1
        // exactly (60000 == 60000). This is enforced as a hard assertion,
        // not just logged -- a regression where the abort instead lands
        // mid-pump (e.g. if a future change removed the fragment-boundary
        // `aborted = true` fix and the abort silently fell through to fire
        // somewhere else) must fail this case, not pass it silently.
        hit_boundary = (bytes == F1);
        wprintf(L"boundary: F1=%llu TWO_FRAG_FULL=%llu bytes=%llu hr=0x%08X hit_boundary=%hs\n",
                static_cast<unsigned long long>(F1), static_cast<unsigned long long>(TWO_FRAG_FULL),
                static_cast<unsigned long long>(bytes), hr, hit_boundary ? "yes" : "no");
        fflush(stdout);

        if (FAILED(hr)) fail(kRounds + 3, "multi-fragment boundary-abort Speak failed");
        if (bytes == 0) fail(kRounds + 3, "multi-fragment boundary-abort utterance produced no audio");
        if (bytes >= TWO_FRAG_FULL) {
            fail(kRounds + 3, "multi-fragment boundary-abort utterance was not actually truncated");
        }
        if (!hit_boundary) {
            fail(kRounds + 3, "multi-fragment abort did not land exactly on the fragment boundary "
                              "(bytes != F1 -- either fragment 1 was cut short, or fragment 2 "
                              "leaked audio before the abort was seen; the fragment-boundary "
                              "GetActions() path this case exists to cover was not exercised)");
        }
    }

    // The point of this whole case: prove the fragment-boundary `aborted =
    // true` fix (DectalkTtsEngine.cpp) means a boundary-landing abort still
    // gets the reset-and-recover cleanup, exactly like a mid-pump abort does
    // -- so the *next* clean utterance is still full-length.
    {
        auto* site = new AbortingSite(kNeverAbort);
        hr = speak_with_watchdog(engine, &frag, site, wfx, kRounds + 4, kWatchdogMs);
        const ULONGLONG bytes = site->bytes();
        wprintf(L"boundary-recovery: Speak -> 0x%08X | %llu bytes\n", hr,
                static_cast<unsigned long long>(bytes));
        site->Release();
        if (FAILED(hr)) fail(kRounds + 4, "post-boundary-abort recovery Speak failed");
        if (bytes < lower_bound || bytes > upper_bound) {
            fail(kRounds + 4, "post-boundary-abort recovery utterance was not full-length "
                              "(fragment-boundary abort left residual state behind)");
        }
    }

    wprintf(L"cancel_probe: PASS | FULL=%llu bytes | %d rounds | %d aborted | %d recovery checks | "
            L"fragment-boundary case hit_boundary=%hs\n",
            static_cast<unsigned long long>(FULL), kRounds, aborted_count, recovery_count,
            hit_boundary ? "yes" : "no");

    CoTaskMemFree(wfx);
    token->Release();
    with_token->Release();
    engine->Release();
    FreeLibrary(mod);
    CoUninitialize();
    return 0;
}
