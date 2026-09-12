// dtc01_core.cpp - see dtc01_core.hpp for the contract this implements, and
// addon/synthDrivers/dectalkDtc01/emu/native.py (NativeMachine) and
// addon/synthDrivers/dectalkDtc01/__init__.py (_pump / _consumeBootAnnouncement)
// for the reference behaviour this ports to C++.
#include "dtc01_core.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace dtc01 {

std::mutex& exec_mutex() {
    static std::mutex m;
    return m;
}

namespace {

// Logs to the debugger (DebugView, or Visual Studio's Output window) since
// Task D1's shared debug_log isn't available yet. ASCII is enough for the
// fixed strings this file emits.
void debug_log(const std::string& msg) {
    OutputDebugStringA((msg + "\r\n").c_str());
}

// Resolves one DLL export by name into a function-pointer member. Logs and
// returns false (rather than throwing) on a miss so create() can fail
// cleanly with nullptr, matching every other failure path here.
template <typename FnPtr>
bool resolve(HMODULE mod, const char* name, FnPtr& out) {
    out = reinterpret_cast<FnPtr>(GetProcAddress(mod, name));
    if (!out) {
        debug_log(std::string("dtc01_core: missing DLL export: ") + name);
        return false;
    }
    return true;
}

// consume_boot_announcement()'s pacing. Chosen to match the design spec's
// measured boot behaviour (speech starts ~0.8s after reset, runs ~2.6s) with
// coarse enough blocks to keep this loop cheap: ~0.2s chunks, give up
// waiting for speech to start after ~6s, call it finished after ~0.8s of
// sustained silence once speech has been heard, ~25s hard ceiling regardless.
constexpr int kSampleRate = 10000;
constexpr int kBootChunkSamples = kSampleRate / 5;                     // 0.2s
constexpr int kBootGiveUpBlocks = 6 * kSampleRate / kBootChunkSamples;  // ~6s
constexpr int kBootMaxBlocks = 25 * kSampleRate / kBootChunkSamples;    // ~25s
constexpr int kBootSilenceBlocksNeeded =
    (8 * kSampleRate / 10 + kBootChunkSamples - 1) / kBootChunkSamples;  // ceil(0.8s / chunk)

// Mirrors native.py's SILENCE_THRESHOLD: a block whose peak stays below this
// (or whose samples are all identical -- the DAC holding its last value, not
// speech; see NativeMachine.is_flat) is silence, not speech.
constexpr int kSilenceThreshold = 120;

}  // namespace

std::unique_ptr<Machine> Machine::create(const std::vector<uint8_t>& main_img,
                                          const std::vector<uint16_t>& dsp_words,
                                          const std::wstring& dll_path) {
    HMODULE mod = LoadLibraryW(dll_path.c_str());
    if (!mod) {
        debug_log("dtc01_core: LoadLibraryW failed for the emulator DLL");
        return nullptr;
    }

    std::unique_ptr<Machine> m(new Machine());
    m->module_ = mod;

    bool ok = true;
    ok &= resolve(mod, "dtc01_create", m->fn_create_);
    ok &= resolve(mod, "dtc01_destroy", m->fn_destroy_);
    ok &= resolve(mod, "dtc01_reset", m->fn_reset_);
    ok &= resolve(mod, "dtc01_feed_text", m->fn_feed_text_);
    ok &= resolve(mod, "dtc01_run_samples", m->fn_run_samples_);
    ok &= resolve(mod, "dtc01_read_host_tx", m->fn_read_host_tx_);
    ok &= resolve(mod, "dtc01_is_idle", m->fn_is_idle_);
    ok &= resolve(mod, "dtc01_set_volume", m->fn_set_volume_);
    ok &= resolve(mod, "dtc01_get_volume", m->fn_get_volume_);
    ok &= resolve(mod, "dtc01_time_seconds", m->fn_time_seconds_);
    ok &= resolve(mod, "dtc01_version", m->fn_version_);
    ok &= resolve(mod, "dtc01_get_led", m->fn_get_led_);
    ok &= resolve(mod, "dtc01_infifo_count", m->fn_infifo_count_);
    ok &= resolve(mod, "dtc01_outfifo_count", m->fn_outfifo_count_);
    ok &= resolve(mod, "dtc01_pending_text", m->fn_pending_text_);
    ok &= resolve(mod, "dtc01_unmapped_accesses", m->fn_unmapped_accesses_);
    ok &= resolve(mod, "dtc01_read_ram32", m->fn_read_ram32_);
    if (!ok) {
        return nullptr;  // ~Machine() frees module_ (== mod) exactly once
    }

    // Keep our own copies alive for the Machine's lifetime -- see the
    // header's comment on why, even though dtc01_create() itself copies
    // both buffers before returning (native/dtc01.c: memcpy for the main
    // image, tms_init for the DSP words).
    m->main_img_ = main_img;
    m->dsp_words_ = dsp_words;

    m->handle_ = m->fn_create_(m->main_img_.data(), static_cast<int>(m->main_img_.size()),
                                m->dsp_words_.data(), static_cast<int>(m->dsp_words_.size()));
    if (!m->handle_) {
        debug_log("dtc01_core: dtc01_create failed (bad ROM sizes or out of memory)");
        return nullptr;  // ~Machine() frees module_ (== mod) exactly once
    }

    return m;
}

Machine::~Machine() {
    if (handle_ && fn_destroy_) {
        fn_destroy_(handle_);
    }
    handle_ = nullptr;
    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

void Machine::feed_text(const std::string& bytes) {
    fn_feed_text_(handle_, reinterpret_cast<const uint8_t*>(bytes.data()),
                  static_cast<int>(bytes.size()));
}

int Machine::run_block(int16_t* out, int max) {
    return fn_run_samples_(handle_, out, max);
}

bool Machine::is_idle() {
    return fn_is_idle_(handle_) != 0;
}

void Machine::set_volume(int percent) {
    fn_set_volume_(handle_, percent);
}

void Machine::reset() {
    fn_reset_(handle_);
}

void Machine::consume_boot_announcement() {
    // Ports __init__.py::_pump()'s two-phase rule (used by
    // _consumeBootAnnouncement) with this file's coarser ~0.2s blocks:
    //   1. Wait for speech to actually start -- right after reset the
    //      pipeline is briefly idle and silent while the firmware gets
    //      going, and treating that as "finished" would skip the
    //      announcement instead of consuming it.
    //   2. Only once speech has been heard, treat sustained idle+silence as
    //      the end.
    // Either phase can also end on a hard block ceiling so a firmware that
    // never settles can't hang the caller forever.
    bool heard = false;
    int quietBlocks = 0;
    int leadInBlocks = 0;
    int16_t buf[kBootChunkSamples];

    for (int block = 0; block < kBootMaxBlocks; ++block) {
        int got = run_block(buf, kBootChunkSamples);
        if (got <= 0) {
            return;  // engine stalled / produced nothing further
        }

        int peak = 0;
        bool flat = true;
        const int16_t first = buf[0];
        for (int i = 0; i < got; ++i) {
            const int a = std::abs(static_cast<int>(buf[i]));
            if (a > peak) peak = a;
            if (buf[i] != first) flat = false;
        }
        // A flat (all-identical-sample) block is the DAC holding its last
        // value between utterances, not speech -- see native.py's is_flat.
        const bool isSpeech = (peak >= kSilenceThreshold) && !flat;

        if (isSpeech) {
            heard = true;
            quietBlocks = 0;
            continue;
        }

        if (!heard) {
            ++leadInBlocks;
            if (leadInBlocks >= kBootGiveUpBlocks && is_idle()) {
                return;  // nothing to announce (or it never started) -- give up waiting
            }
            continue;
        }

        if (is_idle()) {
            if (++quietBlocks >= kBootSilenceBlocksNeeded) {
                return;  // sustained silence after speech: announcement is over
            }
        } else {
            quietBlocks = 0;
        }
    }
    // Hit the hard ceiling; stop regardless so a caller can never hang here.
}

}  // namespace dtc01
