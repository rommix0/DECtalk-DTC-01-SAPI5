# DECtalk DTC-01 for NVDA

An NVDA synthesizer driver that runs the **original 1984 DEC DECtalk DTC-01
firmware** in an emulator. The voice is the real hardware's, produced by the
same 68000 and TMS32010 code that shipped in the box — not a recreation or a
sample set.

> **No firmware is included.** The DTC-01 ROMs are Digital Equipment
> Corporation / Fonix property. You must supply your own dump. See
> [Providing the ROMs](#providing-the-roms).

## Status

**0.6.0 is released** — download the `.nvda-addon` from
[Releases](https://github.com/borris84/dectalk-dtc01/releases/latest). The
add-on checks for later releases itself and offers to install them.

Working and in daily use. Speech, all ten built-in voices, rate, volume,
and the firmware's Design Voice parameters are functional, along with
say-all, index reporting, and rate boost beyond the hardware's own ceiling.

New in 0.6.0: the **v1.8 firmware** can be selected alongside v2.0, and
**Doctor Dennis** and **Whispery Wendy** — two v2.0 voices that earlier
releases missed — are now offered.

| | |
|---|---|
| Latest release | 0.6.0 |
| Emulation speed | ~19.7x realtime (native C core, PGO build) |
| Startup | ~0.5s to first speech |
| Latency | ~50ms typical from request to audio |
| NVDA | 2026.1 (x64); built and tested against 2026.1.1 |

Installed from the published package and verified on NVDA 2026.1.1 AMD64.
The 32-bit core ships in the same package but has not been run on a 32-bit
NVDA.

## Requirements

* NVDA 2025.1 or later (64-bit or 32-bit -- both emulator cores ship in the package)
* Your own DTC-01 v2.0 ROM dump (16 main-CPU chips + the `204`/`205` DSP pair).
  The v1.8 firmware is optional — see [Firmware version](#firmware-version).

## Providing the ROMs

Put the ROM files in `dectalkDtc01\roms\` inside **your NVDA user
configuration directory**:

```
%APPDATA%\nvda\dectalkDtc01\roms\        installed copy of NVDA
<portable folder>\userConfig\dectalkDtc01\roms\    portable copy
```

If you are not sure which applies, open **NVDA menu → Preferences →
Settings → DECtalk DTC-01**. That panel names the exact folder for your
installation, reports which firmware versions were found there, and lets you
point the driver at a different folder instead. It is available even when the
synthesizer is not — which is the normal state before any ROMs are present.

Filenames don't matter — chips are identified by content hash, so whatever
naming your dump uses will work. The driver validates the full set before
starting and refuses to run on an incomplete or altered one. `DTC01_ROM_DIR`
overrides everything, for development.

The expected set is the v2.0 firmware (first half tagged 23 Jul 84, second
half 02 Jul 84) with the `23-204f4` / `23-205f4` DSP pair. Exact SHA1s are
listed in [DESIGN.md](DESIGN.md) §2.

### Firmware version

The DTC-01 shipped with two firmware revisions, and both can be run:

| | v2.0 (default) | v1.8 |
|---|---|---|
| Dated | Jul 1984 | Oct/Dec 1983 |
| Main CPU | `23-095`…`106`, `23-119`…`126` | `23-031`…`038`, `23-059`…`066` |
| DSP pair | `23-204` / `23-205` | `23-165` / `23-166` |
| Voices | 10 | 8 (no Doctor Dennis or Whispery Wendy) |

Select it in the synthesizer settings under **Firmware version**. Only
versions you actually have a complete ROM set for are offered, so nothing
appears unless your dump holds both — one directory can hold both sets at
once, since chips are matched by content hash.

The two halves are not mixable: each main-CPU set requires its own DSP pair.
Running v1.8 against v2.0's DSP clips badly, and v2.0 against v1.8's DSP
comes out very quiet. The driver keeps them paired so the wrong combination
cannot be selected.

v1.8 is offered because it is the firmware the hardware originally shipped
with, and it sounds noticeably different. v2.0 remains the default and is
the better-tested path — most notably, the timing constants above the
emulator were measured against it.

Selecting v1.8 while using Doctor Dennis or Whispery Wendy switches you to
Perfect Paul, since that firmware has no such voices.

## Settings

Beyond the usual rate, volume and voice:

* **Rate boost** — time-compresses the audio so speech can exceed the
  firmware's 350 wpm ceiling, up to ~3x, with pitch unchanged.
* **Pitch, Inflection, Head size, Breathiness, Richness, Smoothness,
  Formant gain** — the firmware's real Design Voice parameters. Each slider
  is *relative to the selected voice*: 50 means that voice's own factory
  default, and the values come from the ROM itself rather than from
  documentation.
* **Formant gain** reduces level inside the synthesis chain rather than on
  the finished audio, so it can clear the clipping that extreme head-size
  settings provoke — something a volume control cannot do.
* **Variable Val** keeps whatever you set on it when you switch away and
  back, matching the slot's purpose on real hardware. The fixed voices
  reset to their own defaults.
* **Firmware version** — v2.0 or v1.8, see
  [Firmware version](#firmware-version) above. Changing it restarts the
  emulator, so speech stops for a moment.

The ROM folder itself lives in NVDA's own settings rather than the
synthesizer's, under **DECtalk DTC-01** — the synthesizer has no settings
until firmware is found, which is exactly when you need to be told where it
goes.

### Voices

v2.0 provides all ten the firmware implements: Perfect Paul, Beautiful
Betty, Huge Harry, Frail Frank, **Doctor Dennis**, Kit the Kid, Rough Rita,
Uppity Ursula, **Whispery Wendy**, and Variable Val.

Dennis and Wendy were absent before 0.6.0. The reference used for the voice
table was the Owner's Manual, 2nd ed. May 1984 — which predates the v2.0
firmware (July 1984) and documents the v1.8 set. Both voices were in the ROM
all along; their factory parameters are read from the firmware itself.

Line joining during say-all is automatic: wrapped lines are reassembled and
split on real sentence boundaries, so a hard wrap mid-sentence doesn't
produce a pause.

## Building

Needs MSVC (Build Tools are enough) and Python 3.

```
tools\build_native.bat          # builds build\dtc01_x64.dll (and x86)
python tools\make_addon.py      # -> build\dectalkDtc01-<ver>.nvda-addon
```

Packaging refuses to run if any NVDA symbol the add-on imports is missing
from your installed NVDA (`tools/check_nvda_api.py`), and the add-on is
verified to contain no ROM data.

The same import check runs in CI against both the current stable NVDA and
the **alpha** channel -- the NVDA 2027.1 development branch -- on every push
and once a day (`.github/workflows/nvda-api-check.yml`). The daily run is the
early-warning for the next API-breaking release: if NVDA removes a symbol the
add-on uses, the build goes red within a day. `tools/ci_fetch_nvda.py`
resolves the newest installer for a channel; the workflow has it build a
portable copy and points the checker at that.

### Private builds with firmware bundled

`python tools\make_addon.py --with-roms` bundles a ROM set into the package
at `<addon>/roms`, so it can be installed on a test machine without setting
up firmware there first. It exists to move **your own** dump between **your
own** machines.

> The result must never be uploaded, released or shared — the firmware is
> Digital Equipment Corporation / Fonix property. The output is named
> `...-PRIVATE-WITH-ROMS-DO-NOT-DISTRIBUTE.nvda-addon` and marks itself in
> the manifest, but the filename is not a safety mechanism. The default
> build is the only one fit to distribute.

Installing a newer release over a private build replaces the add-on
directory, which would take the bundled ROMs with it. **Since 0.5.57 the
updater prevents that**: before installing it copies any bundled firmware to
`%APPDATA%\nvda\dectalkDtc01\roms\`, which lives outside the add-on,
survives every update, and already outranks the bundled copy in the driver's
search order. If that copy cannot be made it asks before continuing, rather
than removing the firmware silently.

Updating a private build is therefore safe. The one case it cannot help is
updating *from* a version older than 0.5.57, whose updater has no such
guard — install 0.5.57 by hand on those machines first.

**A second gap, closed in 0.6.0 but only for updates *after* it.** Until
0.6.0 the guard skipped copying entirely when the config ROM folder already
held anything at all — reasonable when a package could only ever bundle one
firmware set, wrong once 0.6.0 let it bundle two. A machine whose own dump
covered only v2.0 would lose the bundled v1.8 chips on update. Since 0.6.0
the guard copies whatever the config folder is *missing*, compared by
content hash rather than filename.

The catch is structural, and worth understanding rather than working around:
**the updater that performs an update is the old one, so a fix to it cannot
protect the very update that replaces it.** Updating a private build older
than 0.6.0 therefore hits this once, whatever the new version does. If that
build bundled firmware your config folder does not already have, copy
`<addon>\synthDrivers\dectalkDtc01\roms\` into
`<NVDA config>\dectalkDtc01\roms\` **before** updating. From 0.6.0 onward it
is handled for you.

### Release builds use PGO

`tools\build_pgo.bat instrument` → `python tools\pgo_train.py` →
`tools\build_pgo.bat optimize` produces a profile-guided build in
`build\pgo\`, worth **+7–13%** with bit-identical output.
`make_addon.py` prefers it automatically, but ignores it if it is older than
anything in `native\` — a PGO build goes stale as soon as the emulator is
edited, and shipping one built from code that no longer exists would look
entirely normal. `--require-pgo` fails packaging rather than quietly
producing a slower release. The 32-bit core is always an ordinary build:
training has to run the instrumented DLL in a matching-architecture process.

Musashi's opcode tables are generated rather than checked in; the build
script produces them on first run and verifies them.

⚠ That step compiles `m68kmake` with `/Od` **deliberately**. At `/O2` MSVC
miscompiles it: every generated opcode mask loses bit 14, so none carries
the `0xff00` mask Musashi's own table builder scans for as a terminator, the
scan runs off the end of the array, and `m68k_init()` dies with an access
violation before a single instruction executes. It presents as "the DLL
loads but creating a machine crashes", nowhere near the real cause. The
build script checks the generated tables for that mask group and fails
loudly rather than handing you a DLL that crashes at runtime.
`native/musashi/VENDORING.md` has the full detail.

## Development

```
python tools\test_driver_offline.py   # driver logic against the real emulator
python tools\sayall_sim.py            # say-all, with NVDA's real handshake
python tools\compare_native.py        # C core vs the Python reference
python tools\test_scheduler_exact.py  # DSP/DAC timing vs exact rational maths
python tools\bench_native.py A.dll B.dll   # A/B two builds, with an output-identity check
python tools\check_no_roms.py         # refuse to commit firmware
```

The emulator services the DSP and DUART every `M68K_BATCH_CYCLES` (32) 68000
cycles rather than every instruction, which is worth ~+82%; the DAC stays
exactly periodic regardless. Setting it to 1 restores the historical
one-instruction schedule. See [DESIGN.md](DESIGN.md) §16.

### Diagnosing missing or truncated speech

Create an empty file at:

```
%APPDATA%\nvda\dectalkDtc01\trace.flag
```

and restart NVDA. The driver then logs one line per utterance: the text NVDA
handed over, anything a cancel discarded before it was spoken, the bytes sent
to the firmware, and how much audio actually reached the output device. That
distinguishes "NVDA never sent it" from "we dropped it" from "the firmware
produced nothing". Delete the file and restart to turn it off.

It is off by default, and worth turning off again afterwards — it records
everything the screen reader speaks. The `discarded=` counter in the periodic
`DTC-01 stats:` log line is always on and needs no flag.

The pure-Python emulator in `addon/synthDrivers/dectalkDtc01/emu/` is kept as
the readable reference implementation and correctness oracle; the C core in
`native/` is the one fast enough to drive a screen reader.

[DESIGN.md](DESIGN.md) is the working reference — hardware architecture,
verified ROM layout, the DECtalk command language as this firmware actually
implements it, and a log of what was measured against the real ROM (including
several places where the OCR'd manual turned out to be wrong).

## Licence

MIT — see [LICENSE](LICENSE), which also records the provenance of the
vendored Musashi core and the MAME-derived device emulation.
