// DectalkTtsEngine.cpp - see DectalkTtsEngine.hpp for the design rationale.
#include "DectalkTtsEngine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "rom_images.hpp"
#include "text_pipeline.hpp"
#include "utils.hpp"

namespace dtc01 {
namespace sapi {

namespace {

// Fixed native output format (design spec §4.2): mono, signed 16-bit PCM at
// the DTC-01 DAC rate. SAPI's audio layer resamples to the device as needed.
constexpr WORD  AUDIO_FORMAT_TAG   = WAVE_FORMAT_PCM;  // 1
constexpr WORD  AUDIO_CHANNELS     = 1;
constexpr DWORD AUDIO_SAMPLE_RATE  = 10000;
constexpr WORD  AUDIO_BITS         = 16;

// SAPI rate/pitch arrive on a -10..+10 scale.
constexpr int SAPI_RATE_MIN = -10;
constexpr int SAPI_RATE_MAX =  10;

// The DTC-01 word-per-minute range the firmware honours (text_pipeline clamps
// to the same window; keep in step with commands.py RATE_MIN/MAX_WPM).
constexpr int DEFAULT_WPM = 180;

#ifdef BUILD_X64
constexpr const wchar_t* kCoreArch = L"x64";
#else
constexpr const wchar_t* kCoreArch = L"x86";
#endif

// A function whose address lands in THIS module, so GetModuleHandleExW resolves
// to the SAPI dll (or the test/probe exe) rather than to whoever called us.
void module_anchor() {}

// Directory of the module that contains module_anchor(), with a trailing '\'.
// Empty on failure.
std::wstring this_module_dir()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_anchor), &self)) {
        return {};
    }
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash + 1);
}

std::wstring env_var(const wchar_t* name)
{
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
        return {};
    }
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got);
    return v;
}

bool file_exists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// core DLL: env DTC01_CORE_DLL, else dtc01_<arch>.dll beside this module, else
// walk up from the module directory looking for build\dtc01_<arch>.dll (so the
// test/probe exe under sapi\build_*\Release finds the repo's build\ output).
std::wstring resolve_core_dll()
{
    const std::wstring override = env_var(L"DTC01_CORE_DLL");
    if (!override.empty()) {
        return override;
    }

    const std::wstring core_name = std::wstring(L"dtc01_") + kCoreArch + L".dll";

    const std::wstring dir = this_module_dir();
    if (!dir.empty()) {
        const std::wstring beside = dir + core_name;
        if (file_exists(beside)) {
            return beside;
        }

        // Walk up a handful of levels looking for <ancestor>\build\<core>.
        std::wstring anc = dir;
        while (!anc.empty() && anc.back() == L'\\') {
            anc.pop_back();
        }
        for (int i = 0; i < 8 && !anc.empty(); ++i) {
            const std::wstring candidate = anc + L"\\build\\" + core_name;
            if (file_exists(candidate)) {
                return candidate;
            }
            const size_t slash = anc.find_last_of(L"\\/");
            if (slash == std::wstring::npos) {
                break;
            }
            anc = anc.substr(0, slash);
        }
        // Nothing found; hand back the beside-the-module path so the failure
        // names the most likely install location.
        return beside;
    }

    return core_name;
}

// ROM dir: env DTC01_ROM_DIR, else <module dir>\roms.
std::wstring resolve_rom_dir()
{
    const std::wstring override = env_var(L"DTC01_ROM_DIR");
    if (!override.empty()) {
        return override;
    }
    std::wstring dir = this_module_dir();
    if (dir.empty()) {
        return L"roms";
    }
    return dir + L"roms";
}

// SAPI rate -10..+10 -> words per minute, 0 == DEFAULT_WPM. rate_command()
// clamps to the firmware's real window, so out-of-band values are safe here.
int sapi_rate_to_wpm(int sapi_rate)
{
    const int clamped = std::clamp(sapi_rate, SAPI_RATE_MIN, SAPI_RATE_MAX);
    const double wpm = DEFAULT_WPM * std::pow(2.0, clamped / 10.0);
    return static_cast<int>(std::lround(wpm));
}

// Right-trim ASCII whitespace, matching text_pipeline flush_suffix's contract
// that the caller strips trailing whitespace before appending the suffix.
void rstrip(std::string& s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

void add_event(ISpTTSEngineSite* site, SPEVENTENUM id, ULONGLONG offset,
               WPARAM wparam, LPARAM lparam, SPEVENTLPARAMTYPE ltype)
{
    SPEVENT ev = {};
    ev.eEventId = id;
    ev.elParamType = ltype;
    ev.ullAudioStreamOffset = offset;
    ev.ulStreamNum = 0;
    ev.wParam = wparam;
    ev.lParam = lparam;
    site->AddEvents(&ev, 1);
}

// Emit approximate SPEI_WORD_BOUNDARY events for one spoken fragment: words are
// spread evenly across the audio the fragment produced. The DTC-01 firmware has
// no index/phoneme reporting (design spec §4.4), so this is best-effort and
// documented as such; highlighting in screen readers degrades gracefully.
void emit_word_boundaries(ISpTTSEngineSite* site, const std::wstring& raw,
                          ULONG src_offset, ULONGLONG start_bytes, ULONGLONG produced_bytes)
{
    // Collect (char position, length) of each word.
    struct Word { ULONG pos; ULONG len; };
    std::vector<Word> words;
    bool in_word = false;
    ULONG word_start = 0;
    for (ULONG i = 0; i <= raw.size(); ++i) {
        const bool is_word = (i < raw.size()) &&
            (iswalnum(raw[i]) || raw[i] == L'\'' || raw[i] == L'-');
        if (is_word && !in_word) {
            word_start = i;
            in_word = true;
        } else if (!is_word && in_word) {
            words.push_back({word_start, i - word_start});
            in_word = false;
        }
    }
    if (words.empty()) {
        return;
    }
    for (size_t i = 0; i < words.size(); ++i) {
        const ULONGLONG off = start_bytes + (produced_bytes * i) / words.size();
        add_event(site, SPEI_WORD_BOUNDARY, off,
                  words[i].len, static_cast<LPARAM>(src_offset + words[i].pos),
                  SPET_LPARAM_IS_UNDEFINED);
    }
}

}  // namespace

DectalkTtsEngine::DectalkTtsEngine() = default;

DectalkTtsEngine::~DectalkTtsEngine()
{
    // The Machine touches the shared 68000 core on destruction, so tear it down
    // under the same process-wide lock every other Machine call holds. Test
    // machine_ INSIDE the lock: with ThreadingModel=Both the host may race this
    // against an in-flight Speak() mutating machine_ on another thread.
    std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
    if (machine_) {
        machine_.reset();
    }
}

STDMETHODIMP DectalkTtsEngine::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }

    try {
        ISpDataKeyPtr attr;
        if (FAILED(pToken->OpenKey(L"Attributes", &attr)) || !attr) {
            return E_INVALIDARG;
        }

        dectalk::utils::out_ptr<wchar_t> voice_val(CoTaskMemFree);
        dectalk::utils::out_ptr<wchar_t> firmware_val(CoTaskMemFree);
        if (FAILED(attr->GetStringValue(L"DtcVoice", voice_val.address())) || !voice_val.get() ||
            FAILED(attr->GetStringValue(L"DtcFirmware", firmware_val.address())) || !firmware_val.get()) {
            return E_INVALIDARG;
        }

        const std::string voice_key = dectalk::utils::wstring_to_string(voice_val.get());
        const std::string firmware  = dectalk::utils::wstring_to_string(firmware_val.get());

        const dtc01::VoiceDef* voice = dtc01::find_voice(voice_key, firmware);
        if (!voice) {
            return E_INVALIDARG;
        }

        // A token change invalidates any Machine built for the previous voice's
        // firmware; drop it so ensure_machine() rebuilds under the new ROMs.
        // Test machine_ INSIDE the lock (ThreadingModel=Both: a concurrent
        // Speak() on another thread may be mutating it).
        {
            std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
            if (machine_) {
                machine_.reset();
            }
        }

        voice_key_ = voice_key;
        firmware_  = firmware;
        mnemonic_  = voice->mnemonic;
        voice_resolved_ = true;
        token_ = pToken;
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP DectalkTtsEngine::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;
    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP DectalkTtsEngine::GetOutputFormat(
    const GUID* /*pTargetFmtId*/,
    const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pOutputFormatId,
    WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }
    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = nullptr;

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfx) {
        return E_OUTOFMEMORY;
    }

    // Fixed format, independent of any token or Machine (design spec §4.2).
    wfx->wFormatTag      = AUDIO_FORMAT_TAG;
    wfx->nChannels       = AUDIO_CHANNELS;
    wfx->nSamplesPerSec  = AUDIO_SAMPLE_RATE;
    wfx->wBitsPerSample  = AUDIO_BITS;
    wfx->nBlockAlign     = static_cast<WORD>(wfx->nChannels * wfx->wBitsPerSample / 8);
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize          = 0;

    *ppCoMemOutputWaveFormatEx = wfx;
    return S_OK;
}

// Caller must hold exec_mutex().
bool DectalkTtsEngine::ensure_machine()
{
    if (machine_) {
        return true;
    }
    if (!voice_resolved_) {
        return false;
    }

    try {
        const std::wstring rom_dir  = resolve_rom_dir();
        const std::wstring dll_path = resolve_core_dll();
        const std::wstring firmware_w = dectalk::utils::string_to_wstring(firmware_);

        RomImages images = dtc01::load_rom_images(rom_dir, firmware_w);
        machine_ = dtc01::Machine::create(images.main, images.dsp, dll_path);
        if (!machine_) {
            return false;
        }
        // Never let the host hear the power-on "DECtalk, version ..." (§4.3 step 2).
        machine_->consume_boot_announcement();
        return true;
    }
    catch (...) {
        machine_.reset();
        return false;
    }
}

STDMETHODIMP DectalkTtsEngine::Speak(
    DWORD /*dwSpeakFlags*/,
    REFGUID /*rguidFormatId*/,
    const WAVEFORMATEX* /*pWaveFormatEx*/,
    const SPVTEXTFRAG* pTextFragList,
    ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }

    try {
        // One lock for the whole render: the 68000 core is single-instance and
        // process-global (design spec §3 / §4.3 step 1).
        std::lock_guard<std::mutex> lk(dtc01::exec_mutex());

        if (!ensure_machine()) {
            return E_FAIL;  // ROMs/core DLL unresolved, or the core failed to start
        }

        ULONGLONG event_interest = 0;
        pOutputSite->GetEventInterest(&event_interest);
        const bool want_word     = (event_interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_sentence = (event_interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        const bool want_bookmark = (event_interest & SPFEI(SPEI_TTS_BOOKMARK)) != 0;

        long sapi_rate = 0;
        pOutputSite->GetRate(&sapi_rate);
        USHORT sapi_volume = 100;
        pOutputSite->GetVolume(&sapi_volume);

        // Emit the voice-select prefix only when the voice changes within the
        // utterance; a voice change forces a firmware pause. Starts unset so the
        // first spoken fragment always states the voice.
        std::string current_mnemonic;

        ULONGLONG stream_bytes = 0;   // audio written so far, for event offsets
        bool aborted = false;

        constexpr int  kBlockSamples = 1000;                 // 0.1s at 10 kHz
        constexpr long kFragCapSamples = 120L * AUDIO_SAMPLE_RATE;  // 120s safety
        int16_t buf[kBlockSamples];

        for (const SPVTEXTFRAG* frag = pTextFragList; frag && !aborted; frag = frag->pNext) {
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                break;
            }
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&sapi_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&sapi_volume);
            }

            switch (frag->State.eAction) {
            case SPVA_Bookmark: {
                if (!want_bookmark) {
                    break;
                }
                const std::wstring mark = (frag->ulTextLen && frag->pTextStart)
                    ? std::wstring(frag->pTextStart, frag->ulTextLen) : std::wstring();
                const long id = mark.empty() ? 0 : _wtol(mark.c_str());
                add_event(pOutputSite, SPEI_TTS_BOOKMARK, stream_bytes,
                          static_cast<WPARAM>(id),
                          reinterpret_cast<LPARAM>(mark.c_str()),
                          mark.empty() ? SPET_LPARAM_IS_UNDEFINED : SPET_LPARAM_IS_STRING);
                break;
            }

            case SPVA_Silence: {
                // Approximate: write a run of zeros for the requested duration.
                const size_t samples =
                    static_cast<size_t>(AUDIO_SAMPLE_RATE) * frag->State.SilenceMSecs / 1000u;
                std::vector<int16_t> zeros(std::min<size_t>(samples, kBlockSamples), 0);
                size_t remaining = samples;
                while (remaining > 0 && !zeros.empty()) {
                    if (pOutputSite->GetActions() & SPVES_ABORT) { aborted = true; break; }
                    const size_t chunk = std::min(remaining, zeros.size());
                    ULONG written = 0;
                    if (FAILED(pOutputSite->Write(zeros.data(),
                                                  static_cast<ULONG>(chunk * sizeof(int16_t)),
                                                  &written))) {
                        break;
                    }
                    stream_bytes += written;
                    remaining -= chunk;
                }
                break;
            }

            case SPVA_Speak:
            case SPVA_SpellOut:
            case SPVA_Pronounce: {
                if (frag->ulTextLen == 0 || !frag->pTextStart) {
                    break;
                }
                const std::wstring raw(frag->pTextStart, frag->ulTextLen);
                const std::string utf8 = dectalk::utils::wstring_to_string(raw);

                // SPVA_SpellOut: minimal/approximate -- the text is fed as-is
                // (letter-by-letter spelling is a documented follow-up concern);
                // does not block the utterance.
                std::string sanitized = dtc01::sanitize_text(utf8);
                rstrip(sanitized);
                if (sanitized.empty()) {
                    break;
                }

                if (want_sentence) {
                    add_event(pOutputSite, SPEI_SENTENCE_BOUNDARY, stream_bytes,
                              frag->ulTextLen, frag->ulTextSrcOffset, SPET_LPARAM_IS_UNDEFINED);
                }

                // Per-fragment rate/volume ride on top of the stream values.
                const int frag_rate = std::clamp<int>(
                    static_cast<int>(sapi_rate) + frag->State.RateAdj, SAPI_RATE_MIN, SAPI_RATE_MAX);
                const int wpm = sapi_rate_to_wpm(frag_rate);
                const int volume = std::clamp<int>(
                    static_cast<int>(sapi_volume) * frag->State.Volume / 100, 0, 100);

                std::string fed;
                if (current_mnemonic != mnemonic_) {
                    fed += dtc01::voice_command(mnemonic_.c_str());
                    fed += " ";
                    current_mnemonic = mnemonic_;
                }
                fed += dtc01::rate_command(wpm);
                fed += dtc01::dv_command(voice_key_, dtc01::DvParams{});
                fed += sanitized;
                fed += dtc01::flush_suffix(sanitized);

                machine_->set_volume(volume);
                machine_->feed_text(fed);

                // Pump this fragment to idle, writing 0.1s blocks and honouring
                // host actions each block (design spec §4.3 step 6).
                const ULONGLONG frag_start_bytes = stream_bytes;
                long produced = 0;
                int idle_runs = 0;
                int zero_runs = 0;
                while (produced < kFragCapSamples) {
                    const DWORD a = pOutputSite->GetActions();
                    if (a & SPVES_ABORT) { aborted = true; break; }
                    if (a & SPVES_RATE)   { pOutputSite->GetRate(&sapi_rate); }
                    if (a & SPVES_VOLUME) {
                        pOutputSite->GetVolume(&sapi_volume);
                        const int v = std::clamp<int>(
                            static_cast<int>(sapi_volume) * frag->State.Volume / 100, 0, 100);
                        machine_->set_volume(v);
                    }

                    const int got = machine_->run_block(buf, kBlockSamples);
                    if (got > 0) {
                        ULONG written = 0;
                        const HRESULT hr = pOutputSite->Write(
                            buf, static_cast<ULONG>(got * sizeof(int16_t)), &written);
                        if (FAILED(hr)) {
                            break;
                        }
                        stream_bytes += written;
                        produced += got;
                        zero_runs = 0;
                    } else if (++zero_runs > 50) {
                        break;  // stalled without going idle; give up on this fragment
                    }

                    if (machine_->is_idle() && produced > 0) {
                        if (++idle_runs > 3) {
                            break;
                        }
                    } else {
                        idle_runs = 0;
                    }
                }

                if (want_word && !aborted) {
                    emit_word_boundaries(pOutputSite, raw, frag->ulTextSrcOffset,
                                         frag_start_bytes, stream_bytes - frag_start_bytes);
                }
                break;
            }

            default:
                break;  // SPVA_ParseUnknown and friends: nothing to speak.
            }
        }

        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace dtc01
