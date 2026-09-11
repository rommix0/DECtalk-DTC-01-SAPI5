# DECtalk DTC-01 SAPI5 Interface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship 32-bit and 64-bit SAPI5 COM speech engines that voice the DTC-01 emulator, plus an accessible config utility and an Inno Setup installer.

**Architecture:** Each bitness is an in-process COM server implementing `ISpTTSEngine` + `ISpObjectWithToken`, calling the matching-bitness `dtc01_*.dll` directly (no helper/pipe). A shared static lib (`dtc01common`) holds the C-ABI binding, ROM assembly, the 18-voice table, the DECtalk command pipeline, and HKCU settings — all mirrors of the existing Python (`emu/`, `protocol/commands.py`). Settings are read per utterance so the config utility applies live. Adapted from `BstSpeech-sapi-master/`.

**Tech Stack:** C++17, MSVC 2022 (VS Build Tools), CMake ≥ 3.15 (configure once per arch: `-A Win32`, `-A x64`), Win32 + SAPI5 (`sapi.h`, `sapiddk.h`), Inno Setup 6 (`ISCC.exe`). Python 3 only for cross-check tests.

**Spec:** `docs/superpowers/specs/2026-09-11-dectalk-dtc01-sapi5-design.md` (read it alongside this plan).

## Global Constraints

- **Voices:** exactly 18 tokens — 10 from firmware `v20`, 8 from `v18` (v18 excludes `dennis`, `wendy`). US English only (`Language` attr = `409`).
- **Voice mnemonics** (DECtalk `[:n…]`): paul=`np`, betty=`nb`, harry=`nh`, frank=`nf`, dennis=`nd`, kit=`nk`, rita=`nr`, ursula=`nu`, wendy=`nw`, val=`nv`.
- **Audio format:** mono, signed 16-bit PCM, **10000 Hz** (`SPDFID_WaveFormatEx`). SAPI resamples.
- **Concurrency:** one process-wide mutex guards *all* emulator execution (Musashi keeps 68000 state in process globals — concurrent execution crashes `0xC00000FF`).
- **Boot announcement** must be consumed+discarded before the first utterance of any emulator instance (procedure: `addon/synthDrivers/dectalkDtc01/__init__.py::_consumeBootAnnouncement`).
- **DSP preference:** ROM assembly prefers `409/410`, falls back to `204/205` (already in `rom_loader.py`; mirror it).
- **Settings root:** `HKCU\Software\DECtalkDTC01`. Read per utterance; absent value ⇒ built-in default.
- **Ranges/defaults** come from `protocol/commands.py` (`DV_PARAMS`, `VOICE_PARAM_DEFAULTS`, `scale_from_default`, `clamp`, `RATE_MIN_WPM=120`, `RATE_MAX_WPM=350`). That file is the source of truth; the C++ pipeline must match it (Task E1).
- **CLSID (fixed, this project):** `{D7C01A5E-0001-4A11-9C7A-DEC7A1KDTC01}` is a placeholder — generate a real GUID in Task B1 and use it verbatim everywhere after.
- **ROMs:** bundled by the installer into `{app}\roms\`, not marked private (owner's decision). Native cores loaded from `{app}` / `{app}\x64`.
- **Commit** after every task's tests pass. Branch: `sapi5-interface` (already created).

---

## Phase A — shared core: binding, ROMs, voices, pipeline

Deliverable: a console tool that speaks a chosen voice to a WAV through the C++ core — proving binding + ROM + pipeline before any COM.

### Task A1: CMake skeleton + static lib target

**Files:**
- Create: `sapi/CMakeLists.txt`
- Create: `sapi/src/.keep`

**Interfaces:**
- Produces: CMake targets `dtc01common` (STATIC), and per-tool executables added later. Build dirs `sapi/build_x86`, `sapi/build_x64`.

- [ ] **Step 1: Write `sapi/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.15)
project(DectalkDtc01SAPI CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_compile_definitions(BUILD_X64)
endif()
add_library(dtc01common STATIC
  src/rom_images.cpp
  src/text_pipeline.cpp
  src/dtc01_core.cpp)
target_include_directories(dtc01common PUBLIC src)
```

- [ ] **Step 2: Verify configure works for both arches**

Run:
```bash
cmake -S sapi -B sapi/build_x64 -A x64
cmake -S sapi -B sapi/build_x86 -A Win32
```
Expected: configure succeeds (build fails — no sources yet; that's fine, next tasks add them). If you prefer, create empty stubs for the three .cpp so configure+build both pass.

- [ ] **Step 3: Commit**

```bash
git add sapi/CMakeLists.txt sapi/src/.keep
git commit -m "sapi: CMake skeleton for dtc01common"
```

### Task A2: native core binding (`dtc01_core`)

**Files:**
- Create: `sapi/src/dtc01_core.hpp`, `sapi/src/dtc01_core.cpp`
- Create: `sapi/tools/test_core.cpp`
- Modify: `sapi/CMakeLists.txt` (add `test_core` exe)

**Interfaces:**
- Consumes: `dtc01_*.dll` (from `build/` during dev; `{app}` at runtime), C ABI in spec §3.
- Produces:
  ```cpp
  namespace dtc01 {
    std::mutex& exec_mutex();                       // process-wide; hold around ALL calls below
    class Machine {
     public:
      static std::unique_ptr<Machine> create(const std::vector<uint8_t>& main_img,
                                             const std::vector<uint16_t>& dsp_words,
                                             const std::wstring& dll_path);  // nullptr on failure
      void feed_text(const std::string& bytes);
      int  run_block(int16_t* out, int max);        // signed PCM, returns count
      bool is_idle();
      void set_volume(int percent);
      void reset();
      void consume_boot_announcement();             // pump+discard until boot utterance ends
      ~Machine();
    };
  }
  ```

- [ ] **Step 1: Write the failing test `sapi/tools/test_core.cpp`**

```cpp
// Loads dtc01_x64.dll, builds ROM images via rom_images (Task A3 provides
// load_rom_images), creates a Machine, consumes boot, speaks "[:np] test.\r",
// asserts it produced non-silent audio.
#include "dtc01_core.hpp"
#include "rom_images.hpp"
#include <cassert>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    auto imgs = dtc01::load_rom_images(argv[1], L"v20");   // argv[1] = staged roms_v20
    auto m = dtc01::Machine::create(imgs.main, imgs.dsp, argv[2]); // argv[2] = dtc01_x64.dll
    assert(m);
    std::lock_guard<std::mutex> lk(dtc01::exec_mutex());
    m->consume_boot_announcement();
    m->feed_text("[:np] Hello test.\r");
    int16_t buf[8192]; long total = 0; int peak = 0; int got; int idleRuns = 0;
    while (total < 14 * 10000) {
        got = m->run_block(buf, 8192);
        if (got <= 0) break;
        total += got;
        for (int i = 0; i < got; ++i) peak = std::max(peak, std::abs((int)buf[i]));
        if (m->is_idle() && total > 10000) { if (++idleRuns > 4) break; } else idleRuns = 0;
    }
    printf("total=%ld peak=%d\n", total, peak);
    assert(peak > 2000);
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — build fails (no `dtc01_core.*`, `rom_images.*` yet). Expected: compile error. (A3 lands the ROM side; if executing strictly in order, stub `load_rom_images` to read the images so A2's binding is exercised first, then delete the stub in A3.)

- [ ] **Step 3: Implement `dtc01_core.hpp/.cpp`**

Key points to implement:
- `exec_mutex()` returns a function-local `static std::mutex`.
- `Machine::create` does `LoadLibraryW(dll_path)`, `GetProcAddress` for every symbol in spec §3, stores fn pointers, copies ROM buffers (keep them alive), calls `dtc01_create`. Return `nullptr` if any step fails; log via `debug_log` once available (Task D1) — for now `OutputDebugStringW`.
- `run_block` calls `dtc01_run_samples(h, out, max)`.
- `consume_boot_announcement`: port `_consumeBootAnnouncement` — pump `run_block` in ~0.2s chunks; track "speech started" (peak>120 and not flat) then stop after ~0.8s sustained silence with `is_idle()`, hard cap ~25s; give up waiting for start after ~6s.
- Caller holds `exec_mutex()` (as the test does). Document that every method requires the lock held.

- [ ] **Step 4: Run test to verify it passes**

Run (from repo root, after building x64):
```bash
cmake --build sapi/build_x64 --target test_core --config Release
./sapi/build_x64/Release/test_core.exe "<scratch>/roms_v20" "build/dtc01_x64.dll"
```
Expected: prints `peak=` well above 2000, exit 0.

- [ ] **Step 5: Commit**

```bash
git add sapi/src/dtc01_core.* sapi/tools/test_core.cpp sapi/CMakeLists.txt
git commit -m "sapi: native dtc01 core binding + boot-announcement consume"
```

### Task A3: ROM assembly (`rom_images`)

**Files:**
- Create: `sapi/src/rom_images.hpp`, `sapi/src/rom_images.cpp`
- Create: `sapi/tools/test_rom_images.cpp`
- Modify: `sapi/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace dtc01 {
    struct RomImages { std::vector<uint8_t> main; std::vector<uint16_t> dsp; };
    RomImages load_rom_images(const std::wstring& rom_dir, const std::wstring& version); // "v20"/"v18"
    std::vector<std::wstring> available_versions(const std::wstring& rom_dir);
  }
  ```
- Mirrors: `addon/synthDrivers/dectalkDtc01/emu/rom_loader.py` — chip SHA1 tables, `MAIN_CPU_IMAGE_SIZE=0x40000`, `DSP_IMAGE_SIZE=0x1000`, even/odd byte interleave, **DSP candidate order 409/410 then 204/205**.

- [ ] **Step 1: Write the failing test `sapi/tools/test_rom_images.cpp`**

```cpp
// Builds v20 images from a dir holding both DSP pairs; asserts main is 0x40000
// bytes, dsp is 2048 words, and dsp[1] matches the 409 pair's word (0x00d9),
// proving 409/410 is preferred (204's word[1] is 0x00e1).
#include "rom_images.hpp"
#include <cassert>
int wmain(int argc, wchar_t** argv) {
    auto im = dtc01::load_rom_images(argv[1], L"v20");   // argv[1] = roms_v20 (both pairs)
    assert(im.main.size() == 0x40000);
    assert(im.dsp.size() == 2048);
    assert(im.dsp[1] == 0x00d9);   // 409/410 preferred (see rom_loader diff)
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — compile/link error (no `rom_images`). Expected: FAIL.

- [ ] **Step 3: Implement `rom_images.hpp/.cpp`**

- SHA1: use a small vendored SHA1 (public-domain single-file) under `sapi/src/sha1.h`, or Win32 `bcrypt` (`BCryptHashData`). Prefer `bcrypt` (link `bcrypt.lib`) to avoid vendoring.
- Copy the exact SHA1 hex strings, sizes, offsets, and labels from `rom_loader.py`: `MAIN_CPU_ROMS_V20/V18`, `DSP_ROMS_V20_409`, `DSP_ROMS_V20_204`, `DSP_ROMS_V18`.
- Index the dir by file-content SHA1 (non-recursive, like `_index_dir_by_sha1`). Assemble main; assemble the first DSP candidate pair that is complete. Build dsp words `(hi<<8)|lo`.
- `available_versions`: which versions fully assemble (main + some DSP pair).
- Throw `std::runtime_error` with a chip list on failure (mirror `RomValidationError`).

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build sapi/build_x64 --target test_rom_images --config Release
./sapi/build_x64/Release/test_rom_images.exe "<scratch>/roms_v20"
```
Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add sapi/src/rom_images.* sapi/tools/test_rom_images.cpp sapi/CMakeLists.txt
git commit -m "sapi: C++ ROM assembly (409/410 preferred), mirrors rom_loader"
```

### Task A4: the 18-voice table (`voices.hpp`)

**Files:**
- Create: `sapi/src/voices.hpp`
- Create: `sapi/tools/test_voices.cpp`
- Modify: `sapi/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace dtc01 {
    struct VoiceDef { const char* key; const char* mnemonic; const wchar_t* display;
                      const wchar_t* gender; const char* firmware; }; // firmware "v20"/"v18"
    extern const VoiceDef VOICES[18];
    int voice_count();                              // 18
    const VoiceDef* find_voice(const std::string& key, const std::string& firmware);
  }
  ```

- [ ] **Step 1: Write the failing test `sapi/tools/test_voices.cpp`**

```cpp
#include "voices.hpp"
#include <cassert>
#include <cstring>
int wmain() {
    assert(dtc01::voice_count() == 18);
    int v20 = 0, v18 = 0;
    for (int i = 0; i < 18; ++i)
        (std::strcmp(dtc01::VOICES[i].firmware, "v20") == 0 ? v20 : v18)++;
    assert(v20 == 10 && v18 == 8);
    assert(dtc01::find_voice("dennis", "v18") == nullptr);   // dennis is v20-only
    assert(std::strcmp(dtc01::find_voice("paul","v20")->mnemonic, "np") == 0);
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails.** Expected: compile error.

- [ ] **Step 3: Implement `voices.hpp`** — the table: 10 v20 entries (paul…val) + 8 v18 entries (paul,betty,harry,frank,kit,rita,ursula,val). Genders from `VOICE_PARAM_DEFAULTS`/manual (paul/harry/frank/dennis male; betty/rita/ursula/wendy/kit female; val neutral→male). Display names e.g. `L"Perfect Paul"`.

- [ ] **Step 4: Run test to verify it passes.** Build+run `test_voices`, expect exit 0.

- [ ] **Step 5: Commit** — `git commit -m "sapi: 18-voice table (10 v20 + 8 v18)"`.

### Task A5: DECtalk command pipeline (`text_pipeline`)

**Files:**
- Create: `sapi/src/text_pipeline.hpp`, `sapi/src/text_pipeline.cpp`
- Create: `sapi/tools/test_pipeline.cpp`
- Modify: `sapi/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace dtc01 {
    struct DvParams {   // slider 0..100, 50 = voice default; -1 = leave unset
      int inflection=50, head_size=50, breathiness=50, richness=50,
          smoothness=50, loudness=50, laryngealization=50, assertiveness=50, pitch=50;
    };
    std::string rate_command(int wpm);                       // "[:ra 180]", clamps 120..350
    std::string voice_command(const char* mnemonic);         // "[:np]"
    std::string dv_command(const std::string& voice_key, const DvParams& p); // "[:dv ...]" or ""
    std::string sanitize_text(const std::string& utf8);
    std::string flush_suffix(const std::string& sanitized);  // trailing punctuation/\r
  }
  ```
- Mirrors: `protocol/commands.py` (`rate_command`, `voice_command`, `design_voice_command`, `scale_from_default`, `clamp`, `sanitize_text`) and the flush rules in `__init__.py`.

- [ ] **Step 1: Write the failing test `sapi/tools/test_pipeline.cpp`**

```cpp
#include "text_pipeline.hpp"
#include <cassert>
int wmain() {
    assert(dtc01::rate_command(999) == "[:ra 350]");         // clamp high
    assert(dtc01::rate_command(50)  == "[:ra 120]");         // clamp low
    assert(dtc01::voice_command("nh") == "[:nh]");
    assert(dtc01::sanitize_text("a[b]c") == "a b c");        // brackets -> space
    dtc01::DvParams def;                                     // all 50 -> voice defaults
    // paul head-size default 100 (%). At slider 50 dv must emit hs 100 (or omit if all default).
    auto s = dtc01::dv_command("paul", def);
    assert(s.empty() || s.find("hs 100") != std::string::npos);
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails.** Expected: compile error.

- [ ] **Step 3: Implement `text_pipeline.hpp/.cpp`** — port the math from `commands.py`. Embed `VOICE_PARAM_DEFAULTS` (per-voice default/min/max for ap,pr,hs,br,ri,sm,la,as,g5) as a C++ table; implement `scale_from_default` (piecewise-linear, 50=default) and `clamp`. `dv_command` maps DvParams→`[:dv ap.. pr.. hs.. br.. ri.. sm.. g5.. la.. as..]` for values that differ from default (or emit all; match Python's `design_voice_command`). `sanitize_text` and `flush_suffix` ported verbatim.

- [ ] **Step 4: Run test to verify it passes.** Build+run `test_pipeline`, exit 0.

- [ ] **Step 5: Commit** — `git commit -m "sapi: DECtalk command pipeline, mirrors commands.py"`.

### Task A6: console speaker (Phase-A deliverable)

**Files:**
- Create: `sapi/tools/dtc01_speak.cpp`
- Modify: `sapi/CMakeLists.txt`

**Interfaces:**
- Consumes: A2–A5.
- Produces: `dtc01_speak.exe <rom_dir> <dll> <firmware> <voice_key> <text> <out.wav>`.

- [ ] **Step 1: Write the failing test** — a shell check `sapi/tools/test_speak.sh` (or a `.bat`) that runs `dtc01_speak` for paul(v20) and harry(v20), asserts each WAV > 20 KB and RMS differs (voices distinct). Keep it a script asserting file size + a tiny Python RMS compare.

- [ ] **Step 2: Run to verify it fails** — exe not built. Expected: FAIL.

- [ ] **Step 3: Implement `dtc01_speak.cpp`** — load ROMs, create Machine, consume boot, build `voice+rate+dv+sanitized+flush`, feed, pump to a `std::vector<int16_t>`, trim trailing silence, write a 10 kHz mono WAV. Reuse the render loop shape from `render_samples.py`.

- [ ] **Step 4: Run test to verify it passes** — script prints two WAV sizes and "voices differ: True"; exit 0. **Listen check:** optionally play the WAVs to confirm clean speech.

- [ ] **Step 5: Commit** — `git commit -m "sapi: dtc01_speak console tool (Phase A deliverable)"`.

---

## Phase B — COM server and voice tokens

Deliverable: 18 SAPI voices that speak through the real COM object (via `sapi_probe`), registerable into HKLM.

### Task B1: COM scaffolding + self-registration

**Files:**
- Copy+adapt from template: `sapi/src/com.hpp`, `sapi/src/registry.hpp` (reuse as-is from `BstSpeech-sapi-master/src/`, change namespace to `dectalk`).
- Create: `sapi/src/sapi_main.cpp`, `sapi/src/dectalk_sapi.def`
- Modify: `sapi/CMakeLists.txt` (add SHARED lib target `DectalkDtc01SAPI` linking `dtc01common`, `sapi.lib`/`ole32`/`bcrypt`, using the `.def`).

**Interfaces:**
- Produces: `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer`, exported via `.def`. Fixed CLSID for coclass `DectalkTtsEngine`.

- [ ] **Step 1: Generate the real CLSID** — run `powershell -c [guid]::NewGuid()` and record it. Replace the placeholder in Global Constraints and use it in `sapi_main.cpp` and the installer.

- [ ] **Step 2: Write the failing test** — `sapi/tools/test_register.cpp` (or reuse `sapi_probe` in B4). Minimal here: a build check that the DLL links and exports the four entry points (`dumpbin /exports`). Expected FAIL: DLL not built.

- [ ] **Step 3: Implement `sapi_main.cpp`** — adapt `BstSpeech-sapi-master/src/sapi_main.cpp`: class factory returning `DectalkTtsEngine` (Task B2), `DllRegisterServer` writes the CLSID→InprocServer32 and calls `write_voice_tokens` (Task B3) into `HKLM` (respecting the bitness's view); `DllUnregisterServer` calls `remove_voice_tokens` and removes the CLSID. Keep the "register into HKCU for tests" hook the template has.

- [ ] **Step 4: Run** `dumpbin /exports` on the built DLL → all four exports present. Expected PASS.

- [ ] **Step 5: Commit** — `git commit -m "sapi: COM scaffolding + self-registration"`.

### Task B2: `DectalkTtsEngine` (ISpTTSEngine + ISpObjectWithToken)

**Files:**
- Create: `sapi/src/DectalkTtsEngine.hpp`, `sapi/src/DectalkTtsEngine.cpp`
- Modify: `sapi/CMakeLists.txt`

**Interfaces:**
- Consumes: A2–A5; `voice_info`/token attrs from B3.
- Produces: coclass implementing `Speak`, `GetOutputFormat`, `SetObjectToken`, `GetObjectToken`.

- [ ] **Step 1: Write the failing test** — covered by `sapi_probe` (B4). Here, add a unit `sapi/tools/test_outputformat.cpp` that instantiates the class (via the class factory, no registration) and asserts `GetOutputFormat` yields `wFormatTag=WAVE_FORMAT_PCM, nSamplesPerSec=10000, wBitsPerSample=16, nChannels=1`. Expected FAIL (class not implemented).

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `DectalkTtsEngine`** — adapt `ISpTTSEngineImpl`:
  - `SetObjectToken`: read `Attributes\DtcVoice` and `Attributes\DtcFirmware`; resolve ROM dir (from `{module dir}\roms`, or `DTC01_ROM_DIR`); `load_rom_images`; create `Machine` lazily; `consume_boot_announcement`.
  - `GetOutputFormat`: return 10 kHz mono PCM.
  - `Speak`: the render loop in spec §4.3 — lock `exec_mutex()`, read HKCU settings (Task C2 wires them; until then use defaults), build command bytes for each `SPVTEXTFRAG` (handle `SPVA_Speak`, `SPVA_Bookmark`, `SPVA_SpellOut`, `SPVA_Silence`), `set_volume`, `feed_text`, pump blocks to `pOutputSite->Write`, check `GetActions()` each block (`SPVES_ABORT`→stop, rate/volume changes→reapply), emit `SPEI_TTS_BOOKMARK` and start/end events via `AddEvents`. Approximate word-boundary events from fragment lengths + elapsed samples (spec §4.4).

- [ ] **Step 4: Run** `test_outputformat` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "sapi: DectalkTtsEngine Speak/GetOutputFormat/token"`.

### Task B3: voice token registration (18 tokens)

**Files:**
- Create: `sapi/src/voice_registry.hpp`
- Modify: `sapi/src/sapi_main.cpp` (call it)
- Create: `sapi/tools/test_tokens.cpp`

**Interfaces:**
- Consumes: `voices.hpp`.
- Produces: `write_voice_tokens(HKEY root, const std::wstring& clsid)`, `remove_voice_tokens(HKEY root)` under `Software\Microsoft\Speech\Voices\Tokens`.

- [ ] **Step 1: Write the failing test `test_tokens.cpp`** — call `write_voice_tokens(HKCU-test-root, clsid)`, then read back: 18 token subkeys exist; token for paul/v20 has `Attributes\DtcVoice=paul`, `DtcFirmware=v20`, `Language=409`, `CLSID=<clsid>`; `remove_voice_tokens` leaves none. Expected FAIL.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `voice_registry.hpp`** — adapt BstSpeech `voice_registry.hpp`: loop `VOICES[18]`, token id `DECtalk_DTC01_<firmware>_<key>`, display `DECtalk DTC-01 - <Display> (<v2.0|v1.8>)`, attrs Name/Gender/Age=Adult/Language=409/Vendor=DECtalk + `DtcVoice`,`DtcFirmware`. No custom-voice tokens (out of scope for v1).

- [ ] **Step 4: Run** `test_tokens` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "sapi: register 18 voice tokens"`.

### Task B4: `sapi_probe` (Phase-B deliverable)

**Files:**
- Create: `sapi/tools/sapi_probe.cpp`
- Modify: `sapi/CMakeLists.txt`

**Interfaces:**
- Produces: `sapi_probe.exe <DectalkDtc01SAPI.dll> <voice_key> <firmware> <out.wav> "<text>"` — creates the COM object via `DllGetClassObject` (no registration), sets the token, calls `Speak`, writes the WAV.

- [ ] **Step 1: Write the failing test** — `sapi/tools/test_probe.sh`: run `sapi_probe` for paul/v20 and betty/v20; assert both WAVs non-trivial and differ. Expected FAIL (exe absent).

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `sapi_probe.cpp`** — adapt BstSpeech `tools/sapi_probe.cpp`: build an in-memory token (or a temp HKCU token via `write_voice_tokens`) so `SetObjectToken` finds `DtcVoice`/`DtcFirmware`; implement a minimal `ISpTTSEngineSite` that captures `Write` to a buffer; drive one utterance; write WAV.

- [ ] **Step 4: Run** `test_probe.sh` → both WAVs differ; exit 0. **Listen check** on the WAVs.

- [ ] **Step 5: Commit** — `git commit -m "sapi: sapi_probe end-to-end COM speech (Phase B deliverable)"`.

---

## Phase C — parameters and configuration utility

Deliverable: a config utility whose changes take effect on the next utterance of any running SAPI host.

### Task C1: HKCU settings model (`user_settings.hpp`)

**Files:**
- Create: `sapi/src/user_settings.hpp`
- Create: `sapi/tools/test_settings.cpp`

**Interfaces:**
- Produces (namespace `dectalk::settings`):
  ```cpp
  struct GlobalSettings { int rate_percent=100; int volume_db=0; int rate_boost=0; std::string default_firmware="v20"; };
  struct VoiceSettings  { int inflection=50, head_size=50, breathiness=50, richness=50,
                              smoothness=50, loudness=50, laryngealization=50, assertiveness=50, pitch=50; };
  GlobalSettings load_global();
  VoiceSettings  load_voice(const char* voice_key, const char* firmware);
  bool write_global_int(const wchar_t* name, int value);
  bool write_voice_int(const char* voice_key, const char* firmware, const wchar_t* name, int value);
  void reset_voice(const char* voice_key, const char* firmware);
  void reset_all();
  ```
  Root `HKCU\Software\DECtalkDTC01`; voice subkeys `Voices\<firmware>_<key>`. Ranges clamped; absent ⇒ default (50 for sliders).

- [ ] **Step 1: Write the failing test `test_settings.cpp`** — write `RatePercent=150`; `load_global().rate_percent==150`; write a voice `HeadSize=70`; `load_voice(...).head_size==70`; `reset_all()`; `load_global().rate_percent==100`. Expected FAIL.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `user_settings.hpp`** — adapt BstSpeech `user_settings.hpp` (same `get_int`/clamp/write helpers), retargeted to DTC-01 params and `Software\DECtalkDTC01`.

- [ ] **Step 4: Run** `test_settings` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "sapi: HKCU settings model"`.

### Task C2: wire settings into `Speak`

**Files:**
- Modify: `sapi/src/DectalkTtsEngine.cpp`

**Interfaces:**
- Consumes: C1, A5 (`DvParams`), Task C3 (`rate_boost`).

- [ ] **Step 1: Write the failing test** — extend `sapi_probe` to accept `--dump-cmd` printing the exact command bytes; `sapi/tools/test_settings_apply.sh` sets `HeadSize=80` for paul/v20 via `test_settings` writer, runs `sapi_probe --dump-cmd`, asserts the emitted `[:dv ...]` contains a head-size above the default. Expected FAIL (settings not read yet).

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement** — in `Speak`, `load_global()` + `load_voice()` each utterance; map `VoiceSettings`→`DvParams`; apply `rate_percent` to the wpm and `volume_db`/SAPI volume to `set_volume`; apply `rate_boost` (Task C3) to the pumped audio; pitch slider → `[:dv ap]`.

- [ ] **Step 4: Run** `test_settings_apply.sh` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "sapi: apply HKCU settings per utterance"`.

### Task C3: rate-boost (C++ port of `ratebooster.py`)

**Files:**
- Create: `sapi/src/ratebooster.hpp`, `sapi/src/ratebooster.cpp`
- Create: `sapi/tools/test_ratebooster.cpp`

**Interfaces:**
- Produces: `std::vector<int16_t> time_compress(const std::vector<int16_t>& in, double factor);` (factor>1 shortens, pitch preserved), mirroring `addon/synthDrivers/dectalkDtc01/ratebooster.py`.

- [ ] **Step 1: Write the failing test** — compress 10000 samples by 2.0×; assert output ≈ 5000 samples (±5%) and not silent. Expected FAIL.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement** — port the overlap-add/WSOLA (or whatever method) from `ratebooster.py` exactly; keep 10 kHz.

- [ ] **Step 4: Run** `test_ratebooster` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "sapi: rate-boost time-compression (ports ratebooster.py)"`.

### Task C4: configuration utility (accessible Win32 dialog)

**Files:**
- Create: `sapi/tools/dectalk_config.cpp`, `sapi/tools/dectalk_config.rc`, `sapi/tools/dectalk_config_res.h`
- Modify: `sapi/CMakeLists.txt` (add GUI exe `DectalkConfig`)

**Interfaces:**
- Consumes: C1, `voices.hpp`, and SAPI (for "Play sample").

- [ ] **Step 1: Write the failing test** — accessibility is manual, but add `sapi/tools/test_config_persist.cpp` that drives the config's save routine (factor the "on change → write" logic into a testable function `apply_change(control_id, value)`) and asserts HKCU reflects it. Expected FAIL.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement `dectalk_config.cpp`** — adapt `BstSpeech-sapi-master/tools/bestspeech_config.cpp`. Requirements (hard): every control labelled with `&`-accelerator, in tab order, grouped; voice combo (18), per-voice sliders (pitch, inflection, head size, breathiness, richness, smoothness, loudness; advanced: laryngealization, assertiveness), global rate%/volume/rate-boost/default-firmware, "Reset this voice", "Reset all", "Play sample". On any change → write to HKCU immediately (`apply_change`). Close/Esc/Alt-F4 persist (already-saved). "Play sample" uses SAPI `ISpVoice` selecting the current token.

- [ ] **Step 4: Run** `test_config_persist` → PASS. **Manual:** tab through with a screen reader; confirm labels, live apply, persistence.

- [ ] **Step 5: Commit** — `git commit -m "sapi: accessible configuration utility (Phase C deliverable)"`.

---

## Phase D — logging, build, installer

Deliverable: a working installer that registers both servers, bundles ROMs + utility, adds a desktop icon.

### Task D1: engine + utility logging

**Files:**
- Create: `sapi/src/debug_log.h` (adapt from template)
- Modify: `sapi/src/DectalkTtsEngine.cpp`, `sapi/src/dtc01_core.cpp`

**Interfaces:**
- Produces: `DECTALK_LOG(...)` → `%LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log`, shared-append, 4 MB cap + one rollover, lines tagged process/bitness/pid. Toggle `HKCU\Software\DECtalkDTC01\Logging` DWORD.

- [ ] **Step 1: Write the failing test** — `test_log.cpp`: enable logging, emit a line, assert the file exists and contains it; disable, assert no new line. Expected FAIL.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement** — adapt `BstSpeech-sapi-master/src/debug_log.h`; log in `Speak` the token, firmware, ROM dir, resolved rate/pitch/volume, exact command bytes, audio bytes written; in `dtc01_core` log create/boot/failures.
- [ ] **Step 4: Run** `test_log` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "sapi: diagnostic logging for engine and core"`.

### Task D2: build script staging `output/`

**Files:**
- Create: `sapi/build_all.bat`
- Create: `sapi/tools/dectalk_diag.cpp` (diagnostics: walk all 18 tokens through SAPI, write a report) + CMake entry

**Interfaces:**
- Produces: `sapi/output/DectalkDtc01SAPI.dll` (+ `dtc01_x86.dll`, `DectalkConfig.exe`, `DectalkDiagnostics.exe`), `sapi/output/x64/…`, ready for ISCC.

- [ ] **Step 1: Write the failing test** — `sapi/tools/test_build_output.sh`: after running `build_all.bat`, assert all expected files exist in `output/` and `output/x64/`. Expected FAIL.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement `build_all.bat`** — `vcvarsall`; configure+build `build_x64` (`-A x64`) and `build_x86` (`-A Win32`) Release; also build the native cores (`tools\build_native.bat`) or copy existing `build/dtc01_*.dll`; stage into `output/` layout; then (if ISCC present) invoke Task D3's `.iss`.
- [ ] **Step 4: Run** `build_all.bat` then `test_build_output.sh` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "sapi: build_all.bat staging + diagnostics tool"`.

### Task D3: Inno Setup installer

**Files:**
- Create: `sapi/installer/DectalkDtc01SAPI.iss`

**Interfaces:**
- Consumes: `sapi/output/…`, staged ROMs.

- [ ] **Step 1: Write the failing test** — `sapi/tools/test_installer_build.sh`: run `ISCC.exe DectalkDtc01SAPI.iss`; assert `sapi/output/DectalkDtc01_SAPI_Setup.exe` is produced. (Install/uninstall correctness verified manually + by E-phase probes after install.) Expected FAIL.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Implement `.iss`** — adapt `BstSpeech-sapi-master/installer/BestspeechSAPI.iss`:
  - `[Files]`: 32-bit `DectalkDtc01SAPI.dll` + `dtc01_x86.dll` → `{app}`; 64-bit pair → `{app}\x64` (`Check: Is64BitInstallMode`); `DectalkConfig.exe`, `DectalkDiagnostics.exe`; **ROMs** `sapi\output\roms\*` → `{app}\roms` (recurse).
  - `[Components]`: `firmware\v20` (10 voices), `firmware\v18` (8 voices), independently tickable; write chosen set to `{app}\voices.ini`; engine reads it to decide which tokens to register.
  - `[Tasks]` desktop icon (own page); `[Icons]` config + diagnostics + uninstall + optional desktop.
  - `[Code]`: per-bitness `regsvr32` (32-bit via `{syswow64}` in 64-bit mode, 64-bit via `{sys}`), unregister-before-register, uninstall reverses; `PrivilegesRequired=admin`, `ArchitecturesInstallIn64BitMode=x64compatible`, `WizardStyle=modern`, `AlwaysShowComponentsList`. **No** DO-NOT-DISTRIBUTE banner (owner's decision). Enable Inno's `[Setup] SetupLogging=yes`.
- [ ] **Step 4: Run** `test_installer_build.sh` → setup exe produced. **Manual:** install on a clean-ish machine/VM, confirm voices appear in Narrator/Balabolka and speak, config utility works, uninstall is clean.
- [ ] **Step 5: Commit** — `git commit -m "sapi: Inno Setup installer (Phase D deliverable)"`.

---

## Phase E — verification harness

Deliverable: automated checks guarding the engine.

### Task E1: command-parity (C++ pipeline vs `commands.py`)

**Files:**
- Create: `sapi/tools/dump_pipeline.cpp` (prints command bytes for voice/rate/params/text)
- Create: `sapi/tools/verify_pipeline.py` (drives both, diffs)

- [ ] **Step 1: Write the failing test** — `verify_pipeline.py` builds a corpus (voices × sample texts × a few param sets), gets expected strings from `protocol/commands.py`, compares to `dump_pipeline` output; must be byte-identical. Expected FAIL until aligned.
- [ ] **Step 2: Run to verify it fails** (surfaces any drift).
- [ ] **Step 3: Fix** discrepancies in `text_pipeline.cpp` until identical.
- [ ] **Step 4: Run** `python sapi/tools/verify_pipeline.py` → all match.
- [ ] **Step 5: Commit** — `git commit -m "sapi: command-parity test vs commands.py"`.

### Task E2: cancel soak

**Files:**
- Create: `sapi/tools/cancel_probe.cpp`

- [ ] **Step 1: Write the failing test** — speak 60 utterances, aborting most partway (`SPVES_ABORT` mid-stream via the test site); assert no hang, and every post-cancel utterance still produces audio. Expected FAIL until robust.
- [ ] **Step 2: Run to verify it fails** (or passes; if it passes, tighten to expose the abort path).
- [ ] **Step 3: Fix** any abort/drain issues in `Speak`.
- [ ] **Step 4: Run** `cancel_probe.exe` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "sapi: cancel soak test"`.

### Task E3: audio identity + boot-announcement

**Files:**
- Create: `sapi/tools/verify_audio.py`

- [ ] **Step 1: Write the failing test** — for a few voices, render via `sapi_probe` and compare envelope + RMS to the Phase-0 native render / Python oracle for the same text+defaults (tolerance from `compare_native.py`); and assert the first utterance contains no power-on announcement (no speech in the first ~0.8s beyond the fed text's own onset; or compare against a known boot fingerprint). Expected FAIL until matched.
- [ ] **Step 2: Run to verify it fails.**
- [ ] **Step 3: Fix** as needed.
- [ ] **Step 4: Run** `python sapi/tools/verify_audio.py` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "sapi: audio-identity + boot-announcement verification"`.

---

## Self-review notes

- **Spec coverage:** §3 core→A2; §4 architecture→B1/B2; §4.4 events→B2; §5 pipeline→A4/A5/E1; §6 params→A5/C1/C2/C3; §7 config→C4; §8 installer→D2/D3; §9 logging→D1; §10 verification→B4/E1/E2/E3. All sections mapped.
- **Firmware-version selection** (which of v20/v18 a token uses) is per-token (`DtcFirmware`), set at registration (B3) and read in `SetObjectToken` (B2) — no runtime switch needed beyond choosing the voice.
- **Deferred:** Custom Voice tokens (BstSpeech had them) are out of scope for v1; add later if wanted.
- **Type consistency:** `Machine`, `RomImages`, `DvParams`, `GlobalSettings`/`VoiceSettings`, `write_voice_tokens/remove_voice_tokens` names are used consistently across tasks.
