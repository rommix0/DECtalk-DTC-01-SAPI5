// DectalkTtsEngine.cpp - see DectalkTtsEngine.hpp for the design rationale.
#include "DectalkTtsEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "debug_log.h"
#include "ratebooster.hpp"
#include "rom_images.hpp"
#include "speech_shaper.hpp"
#include "text_pipeline.hpp"
#include "user_settings.hpp"
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

// The pump runs the emulator 25 ms at a time. Block size has no effect on
// what the firmware synthesizes (verified bit-identical from 1 to 2000
// samples, both firmwares), so it is picked for responsiveness: an abort, a
// volume change or the first sound of an utterance is noticed within 25 ms of
// emulated time -- around a millisecond of wall time.
constexpr size_t kBlockSamples = 250;

// Longest single Write; the host is polled for an abort between writes.
constexpr size_t kWriteChunkSamples = 1000;

// The firmware silently discards an input line past ~134 bytes, command
// prefix and ",\r" included (DESIGN.md s19); 120 is the margin the NVDA
// driver settled on.
constexpr size_t kFirmwareLineBytes = 120;
constexpr size_t kFlushBytes = 2;  // flush_suffix() appends at most ",\r"
constexpr size_t kMinPieceBytes = 32;

// How long the pipeline must stay idle after speech before a piece counts as
// finished. The firmware can go quiet mid-utterance while it works out how
// to say something -- 450 ms before a long number (DESIGN.md s20) -- which
// short text has not been seen to do. Waiting costs compute time only: the
// idle audio is never written (SpeechShaper).
constexpr size_t kEndIdleShortSamples = 4000;   // 400 ms
constexpr size_t kEndIdleLongSamples = 10000;   // 1 s
constexpr size_t kShortPieceBytes = 40;

// A piece that has been idle this long without making a sound has nothing to
// say (lone punctuation, for instance). Letter-to-sound on long words holds
// the pipeline idle for up to ~1.9 s before the first sound on v1.8
// (DESIGN.md s23). Before this existed such a piece ran to the 120 s cap.
constexpr size_t kSilentGiveUpSamples = 40000;  // 4 s

// Safety net against a firmware that never settles.
constexpr size_t kPieceCapSamples = 120 * AUDIO_SAMPLE_RATE;

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

// Post-boot machine states, keyed by firmware, ROM directory and core DLL and
// shared by every engine instance in the process. Hosts create a new engine
// instance on every voice change; with this only the first instance for a
// firmware pays for the boot. Guarded by exec_mutex().
std::map<std::wstring, std::vector<uint8_t>>& boot_states()
{
    static std::map<std::wstring, std::vector<uint8_t>> states;
    return states;
}

// SAPI rate -10..+10 -> words per minute, 0 == DEFAULT_WPM. rate_command()
// clamps to the firmware's real window, so out-of-band values are safe here.
int sapi_rate_to_wpm(int sapi_rate)
{
    const int clamped = std::clamp(sapi_rate, SAPI_RATE_MIN, SAPI_RATE_MAX);
    const double wpm = DEFAULT_WPM * std::pow(2.0, clamped / 10.0);
    return static_cast<int>(std::lround(wpm));
}

// A carriage return or line feed inside a piece would end the firmware's
// input line early, leaving the rest of the piece unflushed.
void flatten_controls(std::string& s)
{
    for (char& c : s) {
        if (c == '\r' || c == '\n' || c == '\t' || c == '\v' || c == '\f') {
            c = ' ';
        }
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

// Emit approximate SPEI_WORD_BOUNDARY events for one spoken piece: words are
// spread evenly across the audio the piece produced. The DTC-01 firmware has
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

// The 0..100 percent applied to the samples. While the host controls volume
// (the default) that is the host's volume, ISpVoice::SetVolume, scaled by the
// fragment's own from <volume> markup; with "Allow SAPI5 apps to control rate,
// pitch and volume" unticked it is the configuration utility's VolumeDB as a
// gain on full volume, and the host's requests are ignored.
struct Loudness {
    bool app_control = true;
    USHORT host = 100;   // re-read on SPVES_VOLUME
    ULONG fragment = 100;
    int volume_db = dectalk::settings::VOLUME_DB_DEF;

    [[nodiscard]] int percent() const
    {
        if (app_control) {
            return std::clamp<int>(static_cast<int>(host) * fragment / 100, 0, 100);
        }
        // 20*log10(percent/100) = volume_db; 0 dB is full volume.
        const double gained = 100.0 * std::pow(10.0, volume_db / 20.0);
        return std::clamp(static_cast<int>(std::lround(gained)), 0, 100);
    }
};

// Everything on its way from the pump to the host. Applies the volume --
// exactly as dtc01_run_samples did, before the pump took it over so that it
// judges speech on the firmware's own level -- time-compresses it when rate
// boost is on, and writes in bounded chunks with an abort check before each.
class SiteWriter
{
public:
    SiteWriter(ISpTTSEngineSite* site, double rate_boost) : site_(site), booster_(rate_boost) {}

    void set_volume(int percent) { volume_ = percent; }

    // Bytes written so far: the stream offset for events.
    [[nodiscard]] ULONGLONG bytes() const { return bytes_; }

    // False once the host has aborted or a Write failed.
    bool write(const int16_t* samples, size_t count)
    {
        scaled_.assign(samples, samples + count);
        if (volume_ < 100) {
            for (int16_t& s : scaled_) {
                s = static_cast<int16_t>((static_cast<int32_t>(s) * volume_) / 100);
            }
        }
        if (!booster_.active()) {
            return send(scaled_.data(), scaled_.size());
        }
        boosted_.clear();
        booster_.push(scaled_.data(), scaled_.size(), boosted_);
        return send(boosted_.data(), boosted_.size());
    }

    bool write_silence(size_t samples)
    {
        silence_.assign(std::min(samples, kWriteChunkSamples), 0);
        while (samples > 0) {
            const size_t n = std::min(samples, silence_.size());
            if (!send(silence_.data(), n)) {
                return false;
            }
            samples -= n;
        }
        return true;
    }

    // End of a fragment: releases what the rate booster is still holding.
    bool finish_fragment()
    {
        if (!booster_.active()) {
            return true;
        }
        boosted_.clear();
        booster_.finish(boosted_);
        return send(boosted_.data(), boosted_.size());
    }

private:
    bool send(const int16_t* pcm, size_t count)
    {
        for (size_t offset = 0; offset < count;) {
            if (site_->GetActions() & SPVES_ABORT) {
                return false;
            }
            const size_t chunk = std::min(count - offset, kWriteChunkSamples);
            ULONG written = 0;
            // A failed Write is treated exactly like SPVES_ABORT.
            if (FAILED(site_->Write(pcm + offset, static_cast<ULONG>(chunk * sizeof(int16_t)),
                                    &written))) {
                return false;
            }
            bytes_ += written;
            offset += chunk;
        }
        return true;
    }

    ISpTTSEngineSite* site_;
    dtc01::TimeCompressor booster_;
    int volume_ = 100;
    ULONGLONG bytes_ = 0;
    std::vector<int16_t> scaled_;
    std::vector<int16_t> boosted_;
    std::vector<int16_t> silence_;
};

enum class PieceEnd {
    Spoken,   // made a sound and went idle: done
    Silent,   // never made a sound
    Stuck,    // hit the safety cap; the firmware may still hold speech
    Aborted,  // the host aborted or a Write failed
};

// Pumps one fed line until the firmware is done with it, writing only the
// audio SpeechShaper keeps: the first sound of a Speak call arrives as soon
// as the firmware makes it, and the silence the pump waits through afterwards
// never reaches the host. Honours host actions every block (design spec §4.3
// step 6). Caller holds exec_mutex().
PieceEnd pump_piece(Machine& machine, ISpTTSEngineSite* site, SiteWriter& out,
                    Loudness& loudness, long& sapi_rate, bool trim_lead, size_t end_idle)
{
    SpeechShaper shaper(trim_lead);
    std::vector<int16_t> emit;
    int16_t block[kBlockSamples];
    size_t produced = 0;

    for (;;) {
        const DWORD actions = site->GetActions();
        if (actions & SPVES_ABORT) {
            return PieceEnd::Aborted;
        }
        if (actions & SPVES_RATE) {
            site->GetRate(&sapi_rate);
        }
        if (actions & SPVES_VOLUME) {
            site->GetVolume(&loudness.host);
            out.set_volume(loudness.percent());
        }

        if (produced >= kPieceCapSamples) {
            return PieceEnd::Stuck;
        }
        const int got = machine.run_block(block, static_cast<int>(kBlockSamples));
        if (got <= 0) {
            return PieceEnd::Stuck;
        }
        produced += static_cast<size_t>(got);

        // The firmware is idle over a block when nothing is queued for it to
        // say and its output is silent: the DAC holding a value, or parked
        // within a few LSBs (v1.8 parks at small levels and steps between
        // them). Machine::is_idle() also waits for the DSP's output FIFO to
        // drain, which Whispery Wendy's DSP never allows after speaking --
        // judged by that, her utterances ran to the 120 s cap.
        constexpr int kQuietSpan = 160;
        const auto [lo, hi] = std::minmax_element(block, block + got);
        const bool idle = machine.input_idle() && *hi - *lo <= kQuietSpan;

        emit.clear();
        shaper.add(block, static_cast<size_t>(got), idle, emit);
        if (!emit.empty() && !out.write(emit.data(), emit.size())) {
            return PieceEnd::Aborted;
        }

        if (shaper.speech_started()) {
            if (shaper.idle_samples() >= end_idle) {
                break;
            }
        } else if (shaper.idle_samples() >= kSilentGiveUpSamples) {
            return PieceEnd::Silent;
        }
    }

    emit.clear();
    shaper.finish(emit);
    if (!emit.empty() && !out.write(emit.data(), emit.size())) {
        return PieceEnd::Aborted;
    }
    return PieceEnd::Spoken;
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
        // The member publication below (voice_key_/firmware_/mnemonic_/
        // voice_resolved_/token_) is done in this SAME guarded region: Speak()
        // reads all of these under exec_mutex(), so publishing them outside
        // the lock would let a concurrent Speak() (ThreadingModel=Both) observe
        // a torn mix of old and new values.
        {
            std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
            if (machine_) {
                machine_.reset();
            }
            boot_key_.clear();
            voice_key_ = voice_key;
            firmware_  = firmware;
            mnemonic_  = voice->mnemonic;
            voice_resolved_ = true;
            token_ = pToken;
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

        // Guarded explicitly (rather than relying on DECTALK_LOG/Log()'s own
        // Enabled() check) because wstring_to_string(rom_dir) heap-allocates;
        // a plain function-call macro evaluates its arguments regardless of
        // whether Log() ends up doing anything with them, and logging OFF
        // must not cost an allocation on this path.
        if (DebugLog::Enabled()) {
            DECTALK_LOG("DectalkTtsEngine: ensure_machine voice=%s firmware=%s rom_dir=%s",
                        voice_key_.c_str(), firmware_.c_str(),
                        dectalk::utils::wstring_to_string(rom_dir).c_str());
        }

        RomImages images = dtc01::load_rom_images(rom_dir, firmware_w);
        machine_ = dtc01::Machine::create(images.main, images.dsp, dll_path);
        if (!machine_) {
            DECTALK_LOG("DectalkTtsEngine: ensure_machine failed to create the Machine "
                        "(voice=%s firmware=%s)", voice_key_.c_str(), firmware_.c_str());
            return false;
        }

        boot_key_ = firmware_w + L"|" + rom_dir + L"|" + dll_path;
        auto& states = boot_states();
        const auto cached = states.find(boot_key_);
        if (cached != states.end() && machine_->restore_state(cached->second)) {
            DECTALK_LOG("DectalkTtsEngine: ensure_machine restored the booted state "
                        "(firmware=%s), no boot needed", firmware_.c_str());
            return true;
        }

        // Never let the host hear the power-on "DECtalk, version ..." (§4.3 step 2).
        machine_->consume_boot_announcement();
        std::vector<uint8_t> state;
        if (machine_->save_state(state)) {
            states[boot_key_] = std::move(state);
        }
        return true;
    }
    catch (...) {
        DECTALK_LOG("DectalkTtsEngine: ensure_machine threw while creating the Machine "
                    "(voice=%s firmware=%s)", voice_key_.c_str(), firmware_.c_str());
        machine_.reset();
        return false;
    }
}

// Caller must hold exec_mutex().
bool DectalkTtsEngine::rewind_machine()
{
    const auto& states = boot_states();
    const auto it = states.find(boot_key_);
    return it != states.end() && machine_->restore_state(it->second);
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

        // Every utterance starts from the post-boot state. That is what makes
        // cancelling one free: the DTC-01 firmware has no abort command (design
        // spec §4.3), so whatever an interrupted utterance fed stays queued in
        // it -- and rather than draining or resetting and replaying the boot
        // (4-8 s of emulated time, the old cost of every cancelled keystroke),
        // this simply rewinds past it. It also means an utterance sounds the
        // same whatever was spoken before it. Only a core DLL without snapshots
        // falls back to resetting after an abort (end of this function).
        const bool rewound = rewind_machine();

        // The pump applies the volume itself (SiteWriter), so that it judges
        // silence and speech on the firmware's own output level -- a peak test
        // on scaled audio never saw speech at low volume, and ran every
        // utterance to the safety cap.
        machine_->set_volume(100);

        // HKCU settings (Task C4's writer, Task C2 here). Read once per Speak
        // call so a config-utility change takes effect on the very next
        // utterance, without re-hitting the registry per fragment/block.
        // voice_key_/firmware_ are fixed for the lifetime of this engine
        // instance (set once by SetObjectToken; SAPI never changes voice
        // mid-utterance for a single ISpTTSEngine), so loading the voice's
        // sliders once here covers "the current voice" for the whole call.
        const dectalk::settings::GlobalSettings global = dectalk::settings::load_global();
        const dectalk::settings::VoiceSettings voice_cfg =
            dectalk::settings::load_voice(voice_key_.c_str(), firmware_.c_str());

        // 1:1 slider copy (VoiceSettings -> DvParams); dv_command() itself
        // omits any slider still at 50 (this voice's own default), so an
        // all-default VoiceSettings still yields the "" fast path.
        dtc01::DvParams dvp;
        dvp.inflection       = voice_cfg.inflection;
        dvp.head_size        = voice_cfg.head_size;
        dvp.breathiness      = voice_cfg.breathiness;
        dvp.richness         = voice_cfg.richness;
        dvp.smoothness       = voice_cfg.smoothness;
        dvp.loudness         = voice_cfg.loudness;
        dvp.laryngealization = voice_cfg.laryngealization;
        dvp.assertiveness    = voice_cfg.assertiveness;
        dvp.pitch            = voice_cfg.pitch;

        // RateBoost is a percent of *extra* time-compression on top of
        // whatever rate the SAPI client/RatePercent already produced: 0 ->
        // factor 1.0 (no-op passthrough), 100 -> factor 2.0 (half the
        // duration), clamped to the 0..200 the settings model itself enforces
        // (RATE_BOOST_MIN/MAX).
        const double rate_boost_factor =
            1.0 + std::clamp(global.rate_boost,
                              dectalk::settings::RATE_BOOST_MIN,
                              dectalk::settings::RATE_BOOST_MAX) / 100.0;

        ULONGLONG event_interest = 0;
        pOutputSite->GetEventInterest(&event_interest);
        const bool want_word     = (event_interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_sentence = (event_interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        const bool want_bookmark = (event_interest & SPFEI(SPEI_TTS_BOOKMARK)) != 0;

        // Rate, pitch and volume are the host's -- ISpVoice::SetRate and
        // SetVolume, and markup such as <rate absspeed>, <volume level> and
        // <pitch absmiddle>, which is how NVDA sends all three -- unless "Allow
        // SAPI5 apps to control rate, pitch and volume" is unticked. Then the
        // host's requests are ignored, and the configuration utility's
        // RatePercent, VolumeDB and this voice's Pitch slider decide instead.
        const bool app_control = global.app_control;

        long sapi_rate = 0;
        pOutputSite->GetRate(&sapi_rate);
        Loudness loudness;
        loudness.app_control = app_control;
        loudness.volume_db = global.volume_db;
        pOutputSite->GetVolume(&loudness.host);

        // Emit the voice-select prefix only when the voice changes within the
        // utterance; a voice change forces a firmware pause. Starts unset so the
        // first spoken fragment always states the voice.
        std::string current_mnemonic;
        // The Design Voice values the firmware holds once the voice is selected.
        // A [:dv] value sticks until the voice is selected again, so each
        // fragment sends only what differs from what the firmware holds --
        // including the way back after a fragment whose pitch was raised.
        const dtc01::DvValues voice_defaults = dtc01::dv_defaults(voice_key_);
        dtc01::DvValues held = voice_defaults;

        SiteWriter out(pOutputSite, rate_boost_factor);
        bool aborted = false;
        bool stuck = false;
        // Until the first sound of this call, silence is nothing but latency --
        // the host is waiting on it -- so the first speaking piece has its
        // lead-in trimmed back to the waveform. Later pieces keep the pause the
        // firmware puts ahead of a phrase.
        bool spoken = false;

        for (const SPVTEXTFRAG* frag = pTextFragList; frag && !aborted; frag = frag->pNext) {
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                aborted = true;
                break;
            }
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&sapi_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&loudness.host);
            }

            switch (frag->State.eAction) {
            case SPVA_Bookmark: {
                if (!want_bookmark) {
                    break;
                }
                const std::wstring mark = (frag->ulTextLen && frag->pTextStart)
                    ? std::wstring(frag->pTextStart, frag->ulTextLen) : std::wstring();
                const long id = mark.empty() ? 0 : _wtol(mark.c_str());
                add_event(pOutputSite, SPEI_TTS_BOOKMARK, out.bytes(),
                          static_cast<WPARAM>(id),
                          reinterpret_cast<LPARAM>(mark.c_str()),
                          mark.empty() ? SPET_LPARAM_IS_UNDEFINED : SPET_LPARAM_IS_STRING);
                break;
            }

            case SPVA_Silence: {
                // Approximate: write a run of zeros for the requested duration.
                const size_t samples =
                    static_cast<size_t>(AUDIO_SAMPLE_RATE) * frag->State.SilenceMSecs / 1000u;
                if (!out.write_silence(samples)) {
                    aborted = true;
                }
                break;
            }

            case SPVA_Speak:
            case SPVA_SpellOut:
            case SPVA_Pronounce: {
                if (frag->ulTextLen == 0 || !frag->pTextStart) {
                    break;
                }
                // SPVA_SpellOut: minimal/approximate -- the text is fed as-is
                // (letter-by-letter spelling is a documented follow-up concern);
                // does not block the utterance.
                //
                // sanitize_text() only ever replaces an ASCII character with a
                // space, so the sanitized text lines up with the fragment unit
                // for unit: pieces and word boundaries index both alike.
                const std::wstring text = dectalk::utils::string_to_wstring(dtc01::sanitize_text(
                    dectalk::utils::wstring_to_string(std::wstring(frag->pTextStart, frag->ulTextLen))));

                // Rate, volume and pitch: the host's, or the configuration
                // utility's when app_control is off (see its comment above).
                int wpm = 0;
                dtc01::DvParams sliders = dvp;
                if (app_control) {
                    const int frag_rate = std::clamp<int>(
                        static_cast<int>(sapi_rate) + frag->State.RateAdj, SAPI_RATE_MIN, SAPI_RATE_MAX);
                    wpm = sapi_rate_to_wpm(frag_rate);
                    // SAPI pitch is -10..+10 by convention; NVDA sends its pitch
                    // setting as percent / 2 - 25, and more for capital letters.
                    // Two slider steps per unit lands NVDA's setting on this
                    // voice's own Pitch slider: 50 is the voice's own pitch, 0
                    // and 100 the lowest and highest the firmware will make.
                    constexpr int kPitchStepsPerUnit = 2;
                    sliders.pitch = std::clamp(
                        dectalk::settings::SLIDER_DEF +
                            kPitchStepsPerUnit * static_cast<int>(frag->State.PitchAdj.MiddleAdj),
                        dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX);
                } else {
                    // RatePercent of the firmware's default rate; rate_command()
                    // clamps to its [120, 350].
                    wpm = static_cast<int>(std::lround(DEFAULT_WPM * global.rate_percent / 100.0));
                }
                loudness.fragment = frag->State.Volume;
                out.set_volume(loudness.percent());

                const dtc01::DvValues wanted = dtc01::dv_values(voice_key_, sliders);
                bool select_voice = current_mnemonic != mnemonic_;
                bool needs_voice = false;
                std::string dv = dtc01::dv_change_command(select_voice ? voice_defaults : held,
                                                          wanted, &needs_voice);
                if (needs_voice) {
                    // A default [:dv] refuses (Kit's pitch, 306) comes back only
                    // by selecting the voice again.
                    select_voice = true;
                    dv = dtc01::dv_change_command(voice_defaults, wanted, nullptr);
                }

                std::string prefix;
                if (select_voice) {
                    prefix += dtc01::voice_command(mnemonic_.c_str());
                    prefix += " ";
                }
                prefix += dtc01::rate_command(wpm);
                prefix += dv;

                const size_t first_budget =
                    (prefix.size() + kFlushBytes + kMinPieceBytes <= kFirmwareLineBytes)
                        ? kFirmwareLineBytes - kFlushBytes - prefix.size()
                        : kMinPieceBytes;
                const std::vector<dtc01::TextPiece> pieces = dtc01::split_for_firmware(
                    text, first_budget, kFirmwareLineBytes - kFlushBytes);
                if (pieces.empty()) {
                    break;
                }

                if (want_sentence) {
                    add_event(pOutputSite, SPEI_SENTENCE_BOUNDARY, out.bytes(),
                              frag->ulTextLen, frag->ulTextSrcOffset, SPET_LPARAM_IS_UNDEFINED);
                }

                bool prefix_sent = false;
                for (const dtc01::TextPiece& piece : pieces) {
                    const std::wstring piece_text = text.substr(piece.begin, piece.end - piece.begin);
                    std::string line = dectalk::utils::wstring_to_string(piece_text);
                    flatten_controls(line);

                    // The firmware drops an over-long line, and holds only
                    // about two, so each piece is spoken before the next is fed
                    // (DESIGN.md s19). Pieces never end in whitespace, so the
                    // flush suffix sees the real last character.
                    std::string fed;
                    if (!prefix_sent) {
                        fed = prefix;
                        prefix_sent = true;
                        current_mnemonic = mnemonic_;
                        held = wanted;
                    }
                    fed += line;
                    fed += dtc01::flush_suffix(line);

                    DECTALK_LOG("DectalkTtsEngine: Speak voice=%s firmware=%s wpm=%d pitch=%d "
                                "(host %ld) volume=%d fed=[%s]", voice_key_.c_str(),
                                firmware_.c_str(), wpm, sliders.pitch,
                                static_cast<long>(frag->State.PitchAdj.MiddleAdj),
                                loudness.percent(), fed.c_str());

                    const ULONGLONG piece_start = out.bytes();
                    machine_->feed_text(fed);
                    const size_t end_idle = line.size() <= kShortPieceBytes
                        ? kEndIdleShortSamples : kEndIdleLongSamples;
                    const PieceEnd end = pump_piece(*machine_, pOutputSite, out, loudness,
                                                    sapi_rate, !spoken, end_idle);
                    if (end == PieceEnd::Aborted) {
                        aborted = true;
                        break;
                    }
                    if (end == PieceEnd::Stuck) {
                        stuck = true;
                    }
                    if (end == PieceEnd::Spoken) {
                        spoken = true;
                    }
                    if (want_word) {
                        emit_word_boundaries(pOutputSite, piece_text,
                                             frag->ulTextSrcOffset + static_cast<ULONG>(piece.begin),
                                             piece_start, out.bytes() - piece_start);
                    }
                }

                if (!aborted && !out.finish_fragment()) {
                    aborted = true;
                }
                break;
            }

            default:
                break;  // SPVA_ParseUnknown and friends: nothing to speak.
            }
        }

        if ((aborted || stuck) && !rewound) {
            // No state snapshots (a core DLL built before them), so the next
            // Speak() cannot rewind past what this one left queued in the
            // firmware. A block-by-block "run until is_idle()" drain is NOT
            // reliable -- the FIFOs look transiently idle during letter-to-sound
            // before the fed text has become queued speech -- so reset and
            // re-consume the boot announcement, as every abort used to.
            machine_->reset();
            machine_->consume_boot_announcement();
            DECTALK_LOG("DectalkTtsEngine: Speak aborted without state snapshots; reset the "
                        "machine so the next utterance starts clean");
        }

        DECTALK_LOG("DectalkTtsEngine: Speak done, aborted=%d, rewound=%d, %llu audio bytes written",
                    aborted ? 1 : 0, rewound ? 1 : 0, static_cast<unsigned long long>(out.bytes()));
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
