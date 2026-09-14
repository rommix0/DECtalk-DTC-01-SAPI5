// dtc01_core.hpp - native binding to dtc01_*.dll's C ABI (design spec §3),
// mirroring addon/synthDrivers/dectalkDtc01/emu/native.py::NativeMachine for
// the SAPI5 driver, which has no Python runtime to fall back on.
//
// The vendored Musashi 68000 core keeps CPU state in process globals (see
// native.py's _EXEC_LOCK comment and design spec §3's "Concurrency
// constraint"), so the DLL can only ever execute one machine at a time in
// this process; two threads driving even two *different* Machine handles at
// once corrupts that shared state and crashes the process. exec_mutex()
// below is that single process-wide lock.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dtc01 {

// The process-wide emulator lock. The caller must hold this for the
// duration of every Machine method call below, including Machine::create()
// and ~Machine() -- the class itself does not take it, so a caller that
// needs several calls to happen atomically (e.g. feed_text then a run of
// run_block) can do that under one lock/unlock pair instead of paying for
// it per call. A function-local static keeps this a single instance no
// matter how many translation units call it.
//
// Guard rail for future tool authors: this mutex lives in the static
// dtc01common lib, but the emulator's 68000 state lives in the single native
// dtc01_*.dll (one HMODULE, i.e. one set of process globals, per process).
// A process must therefore drive that native core through EXACTLY ONE path
// -- either the SAPI DLL's own exports, or a second copy of dtc01common
// linked directly into a standalone tool, but never both in the same
// process at once. Two independent copies of dtc01common (and so two
// independent exec_mutex() instances) would not serialize against each
// other, and Musashi's global state would corrupt (0xC00000FF) under
// concurrent access.
std::mutex& exec_mutex();

// RAII wrapper around one dtc01_*.dll machine handle. Every method
// (including construction via create() and destruction) requires
// exec_mutex() held by the caller.
class Machine {
 public:
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    Machine(Machine&&) = delete;
    Machine& operator=(Machine&&) = delete;

    // Loads dll_path, resolves every entry point in the C ABI (spec §3),
    // copies main_img/dsp_words (the DLL also copies them internally, but
    // ownership is kept here too so a future zero-copy change on the DLL
    // side can't silently dangle these -- mirrors native.py's rationale),
    // and calls dtc01_create. Returns nullptr if the DLL can't be loaded,
    // any required symbol is missing, or dtc01_create itself fails (bad ROM
    // sizes or out of memory).
    static std::unique_ptr<Machine> create(const std::vector<uint8_t>& main_img,
                                            const std::vector<uint16_t>& dsp_words,
                                            const std::wstring& dll_path);

    ~Machine();

    // Queue raw bytes for the firmware's host-link (UART) input.
    void feed_text(const std::string& bytes);

    // Run the emulator until `max` signed 16-bit PCM samples (10 kHz, mono)
    // have been produced or it stalls. Returns the number of samples
    // written to `out`.
    int run_block(int16_t* out, int max);

    // Speech-pipeline idle: both SPC FIFOs empty and no host text queued.
    bool is_idle();

    // Nothing queued for the firmware to say (dtc01_input_idle): is_idle()
    // without its output-FIFO condition, which Whispery Wendy's DSP program
    // never meets after speaking. Pair it with the audio being silent. Falls
    // back to is_idle() with a core DLL that predates the export.
    bool input_idle();

    void set_volume(int percent);

    void reset();

    // Play out and discard the firmware's power-on announcement (speech
    // begins ~0.8s after a reset and runs ~2.6s) so it is never heard in
    // place of, or queued ahead of, the caller's first real utterance.
    // Ports __init__.py::_consumeBootAnnouncement's two-phase rule: wait for
    // speech to actually start (give up after ~6s of silence -- some
    // configurations may not announce at all), then wait for ~0.8s of
    // sustained silence with the pipeline idle to call it finished, with a
    // ~25s hard ceiling regardless. Call this once, right after create(),
    // before feeding any real text.
    void consume_boot_announcement();

    // State snapshots (native dtc01_state_*). save_state copies this
    // machine's complete dynamic state into `out`; restore_state rewinds it
    // to a state saved from any machine running the same ROM images in this
    // process, in microseconds. Both return false -- restore_state leaving
    // the machine untouched -- if the core DLL predates snapshots or rejects
    // the state; callers then fall back to reset() +
    // consume_boot_announcement().
    bool save_state(std::vector<uint8_t>& out);
    bool restore_state(const std::vector<uint8_t>& state);

 private:
    Machine() = default;

    // -- DLL entry points (spec §3), resolved once in create() -----------
    using Create_t = void*(__cdecl*)(const uint8_t*, int, const uint16_t*, int);
    using Destroy_t = void(__cdecl*)(void*);
    using Reset_t = void(__cdecl*)(void*);
    using FeedText_t = int(__cdecl*)(void*, const uint8_t*, int);
    using RunSamples_t = int(__cdecl*)(void*, int16_t*, int);
    using ReadHostTx_t = int(__cdecl*)(void*, uint8_t*, int);
    using IsIdle_t = int(__cdecl*)(const void*);
    using SetVolume_t = void(__cdecl*)(void*, int);
    using GetVolume_t = int(__cdecl*)(const void*);
    using TimeSeconds_t = double(__cdecl*)(const void*);
    using Version_t = const char*(__cdecl*)();
    using IntrospectInt_t = int(__cdecl*)(const void*);
    using ReadRam32_t = int(__cdecl*)(const void*, uint32_t, uint32_t*);
    using StateSize_t = int(__cdecl*)();
    using StateSave_t = int(__cdecl*)(void*, uint8_t*, int);
    using StateRestore_t = int(__cdecl*)(void*, const uint8_t*, int);

    void* module_ = nullptr;  // HMODULE, stored as void* to keep <windows.h> out of the header
    void* handle_ = nullptr;  // dtc01_t*

    Create_t fn_create_ = nullptr;
    Destroy_t fn_destroy_ = nullptr;
    Reset_t fn_reset_ = nullptr;
    FeedText_t fn_feed_text_ = nullptr;
    RunSamples_t fn_run_samples_ = nullptr;
    ReadHostTx_t fn_read_host_tx_ = nullptr;
    IsIdle_t fn_is_idle_ = nullptr;
    SetVolume_t fn_set_volume_ = nullptr;
    GetVolume_t fn_get_volume_ = nullptr;
    TimeSeconds_t fn_time_seconds_ = nullptr;
    Version_t fn_version_ = nullptr;
    IntrospectInt_t fn_get_led_ = nullptr;
    IntrospectInt_t fn_infifo_count_ = nullptr;
    IntrospectInt_t fn_outfifo_count_ = nullptr;
    IntrospectInt_t fn_pending_text_ = nullptr;
    IntrospectInt_t fn_unmapped_accesses_ = nullptr;
    ReadRam32_t fn_read_ram32_ = nullptr;
    // Optional exports (null with an older core DLL); see create().
    IsIdle_t fn_input_idle_ = nullptr;
    StateSize_t fn_state_size_ = nullptr;
    StateSave_t fn_state_save_ = nullptr;
    StateRestore_t fn_state_restore_ = nullptr;

    // Kept alive for the Machine's lifetime; see create()'s comment above.
    std::vector<uint8_t> main_img_;
    std::vector<uint16_t> dsp_words_;
};

}  // namespace dtc01
