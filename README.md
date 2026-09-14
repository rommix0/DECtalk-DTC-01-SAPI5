# DECtalk DTC-01 for SAPI5

A SAPI5 application that runs the **original 1984 DEC DECtalk DTC-01
firmware** in an emulator using Musashi. The voice is the real hardware's, produced by the same 68000 and TMS32010 code that shipped in the box — not a recreation or a
sample set.

Both versions v1.8 and v2.0 work flawlessly with their correct DSP versions. Based on a finding while creating this project, @rommix0 found that mismatching DSP versions created crackling in the voices. Claude was able to address that directly with no problem.

Everything has been tested by both Claude (automatically) and @rommix0 (manually). If anybody finds bugs I (@rommix0) may have missed, please submit an issue.

Some info in the markdown files contain info regarding the NVDA version, which is not in this repo. However, they contain useful information and some will be left here.

### Firmware versions

The DTC-01 shipped with two firmware revisions, and both can be run:

|          | v2.0 (default)                               | v1.8                                   |
| -------- | -------------------------------------------- | -------------------------------------- |
| Dated    | Jul 1984                                     | Oct/Dec 1983                           |
| Main CPU | `23-095`…`106`, `23-119`…`126`               | `23-031`…`038`, `23-059`…`066`         |
| DSP pair | `23-409` / `23-410` (or `23-204` / `23-205`) | `23-165` / `23-166`                    |
| Voices   | 10                                           | 8 (no Doctor Dennis or Whispery Wendy) |

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

### Configuration utility

**DECtalk configuration** (Start menu, or the optional desktop icon) holds
the SAPI5 settings. Voice settings belong to the voice chosen in the Voice
box. Tick **Apply settings to all voices** to give every voice, on both
firmware versions, the chosen voice's settings; while it stays ticked, each
change applies to all voices and the reset button becomes **Reset all
voices**. Untick it to go back to adjusting one voice at a time — the voices
keep what they have.

**Allow SAPI5 apps to control rate, pitch and volume** is ticked by default:
your screen reader or other SAPI5 program sets the rate, pitch and volume —
including NVDA's higher pitch for capital letters — and the utility's Rate,
Volume and Pitch sliders are unavailable. With NVDA, its pitch setting works
like the utility's Pitch slider: 50 is the voice's own pitch. Untick the box
to set those three in the utility instead; the program's rate, pitch and
volume are then ignored. Rate boost works either way.

**Diagnostic log for bug reports** is **Off** unless you choose **On**, which
saves the text of everything DECtalk speaks to a log file; see
[Diagnostic log](#diagnostic-log).

### Responsiveness

Speech starts as soon as the firmware makes its first sound. The silence the
emulated unit produces before and after an utterance is not sent to the
screen reader, an interrupted utterance is abandoned by rewinding the
emulator to its booted state rather than rebooting it, and a voice change
reuses that booted state. Moving quickly down a list, the delay from a key
press to hearing the next item fell from about 680 ms to about 40 ms, on both
firmware versions; see [DESIGN.md §23](DESIGN.md) for the measurements. Long
text is fed to the firmware in pieces it accepts, so long sentences are
spoken rather than dropped.

### Diagnostic log

The voices can keep a diagnostic log for bug reports. It saves the text
DECtalk speaks, so it is off unless you turn it on: in the configuration
utility, set **Diagnostic log for bug reports** to **On**. Logging starts with
the next thing DECtalk says, without restarting your screen reader, and stops
the same way when you set it back to **Off**. **Reset all settings** turns it
off too.

The log is written to `%LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log`. The
setting is the registry value `HKCU\Software\DECtalkDTC01\Logging` — 1 is on,
0 or no value is off — so a Command Prompt can turn it on:

```
reg add HKCU\Software\DECtalkDTC01 /v Logging /t REG_DWORD /d 1 /f
```

and off again:

```
reg delete HKCU\Software\DECtalkDTC01 /v Logging /f
```

Versions before 1.2.1 kept the log on by default, and those logs are not
deleted automatically. To remove one, delete `dectalk-sapi.log` from that
folder, and `dectalk-sapi.1.log` if it is there.

## Building

Needs Microsoft's Visual Studio 2022 (Build Tools are enough) for building the SAPI wrapper and the DTC-01 engine for Windows, and Inno Setup 6 for compiling the installation program.

```
sapi/build_all.bat
```

## Licence

MIT — see [LICENSE](LICENSE), which also records the provenance of the
vendored Musashi core and the MAME-derived device emulation.
