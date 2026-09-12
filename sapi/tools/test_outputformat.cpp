// test_outputformat.cpp - TDD coverage for DectalkTtsEngine::GetOutputFormat.
//
// Instantiates the coclass directly through com.hpp's object<> helper (no COM
// registration, no token, no Machine -- GetOutputFormat is a fixed format,
// design spec §4.2), queries ISpTTSEngine, and asserts the returned
// WAVEFORMATEX is mono 16-bit PCM at 10 kHz. Speak is exercised separately by
// the B4 sapi_probe.
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>

#include <cassert>
#include <cstdio>

#include "com.hpp"
#include "DectalkTtsEngine.hpp"

int wmain()
{
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    assert(SUCCEEDED(hrInit) && "CoInitializeEx failed");

    {
        dectalk::com::object<dtc01::sapi::DectalkTtsEngine> obj;

        ISpTTSEngine* engine = nullptr;
        const HRESULT hrQI = obj->QueryInterface(__uuidof(ISpTTSEngine),
                                                 reinterpret_cast<void**>(&engine));
        assert(SUCCEEDED(hrQI) && engine && "QueryInterface(ISpTTSEngine) failed");

        GUID fmtid = GUID_NULL;
        WAVEFORMATEX* pwfx = nullptr;
        const HRESULT hr = engine->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmtid, &pwfx);
        assert(SUCCEEDED(hr) && "GetOutputFormat failed");
        assert(pwfx != nullptr && "GetOutputFormat returned a null WAVEFORMATEX");

        assert(IsEqualGUID(fmtid, SPDFID_WaveFormatEx) && "format id must be SPDFID_WaveFormatEx");
        assert(pwfx->wFormatTag == WAVE_FORMAT_PCM && "wFormatTag must be WAVE_FORMAT_PCM");
        assert(pwfx->nChannels == 1 && "nChannels must be 1 (mono)");
        assert(pwfx->nSamplesPerSec == 10000 && "nSamplesPerSec must be 10000");
        assert(pwfx->wBitsPerSample == 16 && "wBitsPerSample must be 16");
        assert(pwfx->nBlockAlign == 2 && "nBlockAlign must be 2");
        assert(pwfx->nAvgBytesPerSec == 20000 && "nAvgBytesPerSec must be 20000");
        assert(pwfx->cbSize == 0 && "cbSize must be 0");

        CoTaskMemFree(pwfx);
        engine->Release();
    }

    CoUninitialize();
    std::puts("test_outputformat: PASS");
    return 0;
}
