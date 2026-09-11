# DECtalk DTC-01 SAPI5 interface — design

**Status:** draft for review · **Date:** 2026-09-11 · **Target:** new `sapi/` subtree

## 1. Goal and scope

Expose the existing DTC-01 hardware emulator as a Microsoft **SAPI5** text-to-speech
engine, in **both 32-bit and 64-bit** COM servers, so any SAPI5 host (Narrator,
NVDA, JAWS, Balabolka, Bookworm, Window-Eyes, etc.) can speak with the real 1984
DECtalk voices. Ship a configuration utility and an accessible Inno Setup installer.

**In scope**

- Two in-process COM servers (`x86`, `x64`) implementing `ISpTTSEngine` +
  `ISpObjectWithToken`, each driving the matching-bitness `dtc01_*.dll` **in
  process** (no helper process, no pipe — see §4).
- **All 18 voices** as static SAPI voice tokens: 10 from firmware v2.0 + 8 from
  v1.8. English (US) only — the DTC-01 firmware has no other language.
- Every adjustable DTC-01 parameter surfaced to SAPI and to the config utility
  (§6): rate, pitch, volume, the Design-Voice parameters, rate-boost, and firmware
  selection.
- A Win32 configuration utility, fully keyboard/screen-reader accessible, live-apply,
  settings persisted in `HKCU` (§7).
- Inno Setup installer bundling both servers, both native cores, the ROMs, the
  utility, a desktop icon; accessible wizard (§8).
- Detailed debug logging in both servers and the installer (§9).
- Verification harness: SAPI probe (x86/x64), cancel soak, byte-identity vs. the
  Python oracle (§10).

**Explicitly out of scope**

- Any language other than US English (the firmware has none).
- SAPI4. (SAPI5 only.)
- Redistribution of the installer — see §8.4 (ROM copyright).

## 2. Background: what already exists

- `native/` — the C emulator core, built by `tools/build_native.bat` into
  `build/dtc01_x64.dll` and `build/dtc01_x86.dll`. Flat C ABI (see §3).
- `addon/synthDrivers/dectalkDtc01/emu/` — the pure-Python reference emulator and
  `rom_loader.py` (ROM assembly by content hash; now prefers the clean 409/410 DSP,
  falls back to 204/205).
- `addon/synthDrivers/dectalkDtc01/protocol/commands.py` — the DECtalk in-line
  command language: voice mnemonics, rate, and the Design-Voice (`[:dv …]`) params
  with per-voice defaults/ranges read from the ROM. **The SAPI text pipeline mirrors
  this file** (§5); it is the single source of truth for command syntax and ranges.
- `BstSpeech-sapi-master/` — the SAPI5 wrapper this design adapts. We reuse its
  COM scaffolding, token-registration model, HKCU settings pattern, config-utility
  shape, and installer structure; we replace its 32-bit-engine + helper/pipe bridge
  with direct in-process calls into `dtc01_*.dll`.

## 3. The native core's C ABI (already implemented, from `emu/native.py`)

```
void*  dtc01_create(const uint8_t* main_img, int main_len,
                    const uint16_t* dsp_words, int dsp_len);   // NULL on failure
void   dtc01_destroy(void* h);
void   dtc01_reset(void* h);
int    dtc01_feed_text(void* h, const uint8_t* data, int len);
int    dtc01_run_samples(void* h, int16_t* out, int max);       // signed 16-bit PCM, 10 kHz
int    dtc01_read_host_tx(void* h, uint8_t* out, int max);
int    dtc01_is_idle(void* h);
void   dtc01_set_volume(void* h, int percent);                  // 0..100, applied in-DLL
int    dtc01_get_volume(void* h);
double dtc01_time_seconds(void* h);
const char* dtc01_version(void);
/* plus introspection: dtc01_get_led / _infifo_count / _outfifo_count /
   _pending_text / _unmapped_accesses / _read_ram32 */
```

Output is **mono, signed 16-bit PCM at 10 000 Hz** — the hardware DAC rate.

**Concurrency constraint (load-bearing):** the vendored Musashi 68000 core keeps CPU
state in process globals, so **only one machine may execute at a time per process**.
Two threads running different machines corrupt state and crash (`0xC00000FF`). The
Python binding serializes on a process lock; the SAPI DLL must do the same (§4.3).

## 4. Architecture

### 4.1 One in-process COM server per bitness

`sapi/build_x86/` → `DectalkDtc01SAPI.dll` (32-bit), `sapi/build_x64/` →
`DectalkDtc01SAPI.dll` (64-bit). Each is a self-registering in-proc COM server
exposing one coclass (the TTS engine) under a fixed CLSID. Each links/loads the
`dtc01_*.dll` of its **own** bitness from the install directory.

BstSpeech needs a `b32_helper.exe` + named-pipe bridge only because its engine DLLs
are 32-bit-only; a 64-bit host cannot load them in-process. **Our core builds
natively as both x86 and x64**, so each server calls its own-bitness core directly.
This removes the helper process, the pipe protocol, and their failure modes entirely.
`helper_client.*`, `pipe_client.*`, `pipe_protocol.h`, `b32_wrapper.*`,
`bestspeech_server.cpp` from the template are **dropped**.

### 4.2 COM object

`class DectalkTtsEngine : public ISpTTSEngine, public ISpObjectWithToken`
(adapted from `ISpTTSEngineImpl`). Methods:

- `SetObjectToken(token)` — read `Attributes\DtcVoice` and `Attributes\DtcFirmware`
  (see §5.2) to fix which voice + firmware this instance speaks; resolve and cache
  the ROM images for that firmware; create the emulator lazily.
- `GetOutputFormat(...)` — return `SPDFID_WaveFormatEx`, mono PCM **10 000 Hz**.
  (SAPI's audio layer resamples to the device as needed.)
- `Speak(flags, fmtId, wfx, fragList, site)` — the render loop (§4.3).

### 4.3 The `Speak` render loop

1. Acquire the **process-wide emulator mutex** (mirrors `_EXEC_LOCK`).
2. Ensure this instance's emulator exists; if just created, **consume and discard the
   power-on announcement** (pump audio until the boot utterance finishes — the exact
   procedure in `__init__.py::_consumeBootAnnouncement`) so hosts never hear
   "DECtalk, version …".
3. Read HKCU settings (§7) — cheap, done per utterance so config changes apply next
   utterance with nothing restarted.
4. Build the command byte stream from the SAPI fragments via the text pipeline (§5):
   voice-select prefix (only when it changed), rate, Design-Voice `[:dv …]` prefix,
   sanitized text, trailing flush.
5. `dtc01_set_volume(...)`; `dtc01_feed_text(...)`.
6. Pump: loop `dtc01_run_samples` in ~50–100 ms blocks →
   `site->Write(pcm, bytes, …)`. **Each block**, check `site->GetActions()`:
   `SPVES_ABORT` → stop and return; `SPVES_RATE_CHANGE`/`SPVES_VOLUME_CHANGE` →
   re-read and apply. Fire bookmark and stream events via `site->AddEvents` (§4.4).
7. Stop when the emulator reports idle + sustained silence (the driver's
   end-of-utterance debounce), or an utterance-length ceiling is hit. Release lock.

Instance reuse: keep the emulator for the life of the COM object; between utterances
it is already idle. On abort mid-utterance, drain or `dtc01_reset` (reset re-triggers
the boot announcement, so it must be re-consumed — prefer draining).

### 4.4 Events and their limits

This firmware has **no index/phoneme reporting** (`machine.py`: "no `[:index]` to
report"). Therefore:

- **Bookmarks** (`SPVA_Bookmark` fragments) — supported: emitted as
  `SPEI_TTS_BOOKMARK` at the audio offset where the fragment sits in the stream.
- **Stream/sentence start & end** — supported.
- **Word boundary events** — *best-effort*, approximated from fragment text lengths
  and elapsed audio, not from the firmware. Documented as approximate. (Most SAPI
  screen-reader use — highlighting — degrades gracefully without them.)

## 5. Text pipeline and command mapping

New `sapi/src/text_pipeline.*`, a C++ port of the rules in `protocol/commands.py`
(the Python file stays the reference; both must agree, checked in §10).

### 5.1 Voice, rate, Design-Voice, sanitisation

- **Voice select:** `[:np]` (Paul), `[:nb]`, `[:nh]`, `[:nf]`, `[:nd]`, `[:nk]`,
  `[:nr]`, `[:nu]`, `[:nw]`, `[:nv]`. Emitted only when the voice actually changes
  (a voice change forces a firmware pause). `dennis`/`wendy` exist only in v2.0.
- **Rate:** `[:ra <wpm>]`, clamped 120–350 wpm.
- **Design-Voice prefix:** `[:dv <abbr> <value> …]` built from the parameters in §6,
  each clamped/scaled per `commands.py` (`scale_from_default`, `clamp`).
- **Sanitisation:** `sanitize_text` — strip `[`/`]`, drop isolated parentheses the
  firmware would speak aloud. Ported verbatim.
- **Flush:** trailing punctuation/`\r` handling that actually flushes the firmware
  buffer (ported from `__init__.py`'s flush rules).

### 5.2 Voice-token identity

Each of the 18 tokens carries, under its `Attributes`:

- `DtcVoice` — the mnemonic key (`paul`…`val`).
- `DtcFirmware` — `v20` or `v18`.
- Standard SAPI attrs: `Name`, `Gender`, `Age`, `Language` = `409` (US English),
  `Vendor` = `DECtalk`.

`SetObjectToken` reads `DtcVoice`/`DtcFirmware` back, so the exact voice+firmware is
recovered without parsing the display name. Token display names, e.g.
`DECtalk DTC-01 - Perfect Paul (v2.0)`.

## 6. Parameters exposed

SAPI carries only rate/pitch/volume natively; everything else is a per-voice setting
in HKCU that the engine reads each utterance (like BstSpeech). Ranges/defaults come
from `commands.py` (ROM-measured), and Design-Voice sliders use `scale_from_default`
so **50 = this voice's factory default**.

| Parameter | Source | DTC-01 realisation |
|---|---|---|
| Rate | SAPI `Rate` (−10..10) × HKCU RatePercent | `[:ra wpm]`, then **rate-boost** time-compression above 350 wpm (`ratebooster.py`) |
| Pitch | SAPI `Pitch` (−10..10) + HKCU per-voice | `[:dv ap …]` (average pitch), scaled from default |
| Volume | SAPI `Volume` (0..100) + HKCU VolumeDB | `dtc01_set_volume` and/or gain trim |
| Inflection / pitch range | HKCU per-voice | `[:dv pr …]` |
| Head size | HKCU per-voice | `[:dv hs …]` |
| Breathiness | HKCU per-voice | `[:dv br …]` |
| Richness | HKCU per-voice | `[:dv ri …]` |
| Smoothness | HKCU per-voice | `[:dv sm …]` |
| Loudness / formant gain | HKCU per-voice | `[:dv g5 …]` (in-chain; clears head-size clipping) |
| Laryngealization, assertiveness | HKCU per-voice (advanced) | `[:dv la …]`, `[:dv as …]` |
| Rate boost | HKCU global | post-synthesis time-compression, pitch unchanged |
| Firmware | per-token (`DtcFirmware`) | selects ROM set v20/v18 |

Variable Val keeps whatever parameters it was last given (matching the hardware slot);
fixed voices reset to their own ROM defaults, exactly as the NVDA driver does.

## 7. Configuration utility

`sapi/tools/dectalk_config.cpp` — a plain Win32 dialog (adapted from
`bestspeech_config.cpp`). **Accessibility is a hard requirement:** every control
labelled, has an access key, and sits in the tab order (this is what a screen reader
handles best).

- **Voice** picker (18 voices) + per-voice sliders: pitch, inflection, head size,
  breathiness, richness, smoothness, loudness, (advanced: laryngealization,
  assertiveness). "Reset this voice" restores ROM defaults.
- **Global:** rate %, volume dB, rate-boost, default firmware.
- **Play sample** — speaks the selected voice **through SAPI**, current settings
  included, so the user hears exactly what applications will get.
- **Persistence & live-apply:** every change is written to
  `HKCU\Software\DECtalkDTC01` the moment it is made (`user_settings.hpp` pattern);
  the engine re-reads per utterance, so a change lands on the next spoken utterance of
  any running host, and everything is already saved when the dialog closes (Close /
  Esc / Alt-F4 all persist).

## 8. Installer (Inno Setup)

`sapi/installer/DectalkDtc01SAPI.iss`, adapted from `BestspeechSAPI.iss`.

### 8.1 Payload

- 32-bit `DectalkDtc01SAPI.dll` + `dtc01_x86.dll` → `{app}`.
- 64-bit `DectalkDtc01SAPI.dll` + `dtc01_x64.dll` → `{app}\x64` (64-bit install mode).
- `DectalkConfig.exe`, `DectalkDiagnostics.exe`.
- **ROMs** → `{app}\roms\` (see §8.4).

### 8.2 Registration

`PrivilegesRequired=admin` (voice tokens + COM class live in HKLM).
`ArchitecturesInstallIn64BitMode=x64compatible`. Register each server with a
`regsvr32` of its **own bitness** so the 32-bit server writes tokens under
`WOW6432Node` (32-bit hosts) and the 64-bit under the native view (Narrator).
Unregister-before-register on reinstall/upgrade. Uninstall reverses both.

### 8.3 Components, tasks, icons

- Components: **Firmware v2.0 (10 voices)** and **Firmware v1.8 (8 voices)**, tickable
  independently (unlike BstSpeech there is no language×voice cross — voices are fixed
  per firmware). Selection written to `{app}\voices.ini`, read at registration so the
  wizard and registry can't drift.
- Task: optional **desktop icon** for the config utility (its own wizard page, screen-
  reader reachable). Start-menu icons for config + diagnostics + uninstall.
- Accessible wizard: `WizardStyle=modern`, `AlwaysShowComponentsList`, labelled custom
  pages, Ready page lists the voice count. (Inno's wizard is already screen-reader
  friendly; we keep custom text minimal and labelled.)

### 8.4 ROMs

The installer **bundles the ROMs** into `{app}\roms\` so it works out of the box with
no separate firmware setup. Both firmware sets (v2.0 and v1.8, and both v2.0 DSP pairs)
are staged; the engine's `rom_images` picks the right chips by content hash and prefers
409/410.

Per the project owner's decision (2026-09-11), the artifact is **not** marked private:
the DTC-01 ROMs are treated as proprietary abandonware (Digital Equipment Corp and
Fonix long defunct), so the installer carries no `DO-NOT-DISTRIBUTE` banner. This is a
deliberate departure from the NVDA add-on, which never ships firmware. (The
distribution decision rests with the project owner; the build simply includes the
files it is given.)

## 9. Logging

- **Engine:** `%LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log`, shared-append, capped
  (4 MB + one rollover), each line tagged process/bitness/pid. Records token chosen,
  firmware, ROM dir, resolved rate/pitch/volume, the **exact command bytes** fed to
  the firmware, and audio bytes written. Toggle via
  `HKCU\Software\DECtalkDTC01\Logging` (DWORD; on by default while young, per the
  BstSpeech precedent). (Adapts `debug_log.h`.)
- **Installer:** Inno's own `/LOG`, plus explicit log lines around ROM validation and
  each regsvr32 (success/failure surfaced to the user).

## 10. Verification

- `sapi/tools/sapi_probe.cpp` (x86 + x64): create the COM object via
  `DllGetClassObject`, feed each of the 18 tokens, capture audio to WAV — the path a
  screen reader takes, no registration/elevation needed.
- **Command-parity test:** the C++ pipeline vs. `protocol/commands.py` on a corpus —
  byte-identical command output (mirrors BstSpeech's translit-parity approach).
- **Cancel soak** (`cancel_probe`): many utterances abandoned mid-way; must never hang
  or return empty after a cancel.
- **Audio identity:** SAPI-rendered voice vs. the Phase-0 native render / Python oracle
  for the same text+params — envelope + level match (`compare_native.py` approach).
- **Boot-announcement test:** first utterance never contains the power-on speech.

## 11. Proposed layout

```
sapi/
  CMakeLists.txt            # both bitnesses; links dtc01 core by bitness
  build_all.bat             # build x86+x64, stage output/, compile installer
  src/
    sapi_main.cpp           # DllGetClassObject/Register/Unregister, token (un)registration
    DectalkTtsEngine.{hpp,cpp}   # ISpTTSEngine + ISpObjectWithToken (from ISpTTSEngineImpl)
    dtc01_core.{hpp,cpp}    # ctypes-equivalent C++ loader/binding for dtc01_*.dll
    rom_images.{hpp,cpp}    # ROM assembly (C++ mirror of rom_loader; 409/410 preferred)
    text_pipeline.{hpp,cpp} # command language (C++ mirror of commands.py)
    voices.hpp              # the 18-voice table (mirror of VOICES + firmware split)
    user_settings.hpp       # HKCU settings (from BstSpeech user_settings.hpp)
    voice_registry.hpp      # token (un)registration (from BstSpeech)
    registry.hpp, com.hpp, debug_log.h   # reused from BstSpeech
  tools/
    dectalk_config.cpp      # the configuration utility
    dectalk_diag.cpp        # diagnostics
    sapi_probe.cpp          # verification probe
  installer/
    DectalkDtc01SAPI.iss
```

## 12. Decisions and residual risks

Resolved with the project owner (2026-09-11):

1. **ROM bundling** (§8.4) — bundle ROMs, **not** marked private. ✔
2. **Output rate** — return **native 10 kHz**; SAPI resamples. If a host ever rejects
   it, add an optional in-engine upsample to 22050 (deferred until observed). ✔
3. **Word-boundary events** — **approximate** from text length + elapsed audio;
   bookmarks and start/end exact (§4.4). ✔
4. **Commit** — feature branch, committed as work proceeds. ✔

Residual risks:

- **Single-instance serialization** (§3/§4.3) — the process-wide mutex means two SAPI
  voices in one host speak strictly one at a time. Matches the hardware (one box) and
  normal screen-reader use (one utterance at a time).
- **Rate-boost placement** — port `ratebooster.py` to C++ (not call into Python) so the
  DLL stays dependency-free.
