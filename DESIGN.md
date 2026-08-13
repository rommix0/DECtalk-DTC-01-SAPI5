# DECtalk DTC-01 → NVDA Synth Driver: Design Reference

Ground truth for a multi-session build. Do not re-derive these facts from
memory — they were pulled from the actual MAME driver source and the 1984
DTC-01 Owner's Manual (OCR). Sources are cited inline.

## 0. Legal / asset handling

- The DTC-01 main-CPU ROMs and DSP ROMs are Digital Equipment Corp / Fonix
  copyrighted firmware. They are **never** committed to this repo and
  **never** shipped inside the `.nvda-addon` package.
- `roms_extracted/` at the project root holds the user's own dumped ROM set
  for local development/testing only. Verified against MAME's known-good
  SHA1 hashes (see §2) — all 16 main-CPU v2.0 ROMs and the DSP `204/205`
  pair matched exactly, as did the full v1.8 set (§21).
- The shipped addon ships zero ROM bytes. On first run it points the user
  at a config folder and validates checksums via `tools/rom_loader.py`
  logic before allowing the synth to start.
- Two files in the user's zip are *not* part of the MAME `dectalk` ROM_START
  and are unused/unexplained: `chargen-15ie.bin` (2048B) and `dump1.bin`/
  `dump5.bin` (4096B each). Leave them alone — not needed for this driver.

## 1. Hardware architecture

Source: `src/mame/dec/dectalk.cpp` (MAME, BSD-3-Clause, Jonathan Gevaryahu),
fetched from `github.com/mamedev/mame/blob/master/src/mame/dec/dectalk.cpp`.
Local copy: `research/mame_dectalk.cpp`.

- **Main CPU**: Motorola 68000, `XTAL(20MHz)/2` = 10MHz. Runs the closed
  firmware: sentence parsing, letter-to-sound, prosody, DUART/host I/O,
  NVRAM, LEDs. Emulate with **Unicorn Engine's M68K backend**.
- **DSP**: TI TMS32010, `XTAL(20MHz)` = 20MHz. Runs the Klatt synthesis
  math and produces 10kHz 12-bit samples. **Unicorn has no TMS32010
  backend** — this needs a standalone core (small ISA, ~40 instructions,
  single accumulator + T/P registers, 144 words on-chip data RAM, 2K words
  program space). Port logic from MAME's BSD-3 `cpu/tms320c1x/` device or
  write from the public TI datasheet.
- **DUART**: Signetics/Philips SCN2681 (a.k.a. MC68681-compatible),
  `XTAL(3.6864MHz)`. Channel B is the host RS-232 port (this is the one we
  care about — the driver wires `b_tx_cb` to the `rs232` slot device, i.e.
  outbound bytes from channel B are what the host sees). Channel A
  (`duart_txa`) is an unused secondary passthrough in the MAME driver —
  ignore it. DUART IRQ → 68K IRQ level 6.
- **NVRAM**: Xicor X2212, holds saved voice/setup state across power
  cycles. Not critical for v1 — can start from the ROM-embedded default
  NVRAM image and skip persistence.
- **DAC**: AD7541, 12-bit, driven by a **fixed 10kHz** sample-output timer
  (`outfifo_read_cb`, `attotime::from_hz(10000)`). This is a hard ceiling
  on output audio bandwidth/fidelity to reproduce, and it's also why
  end-to-end latency can be very low — samples are produced and consumed
  in small 100µs steps, not big blocks.
- Interrupt map: TLC (telephone/DTMF) = IRQ4, SPC (speech/DSP) = IRQ5,
  DUART = IRQ6. We don't need the telephone/DTMF (TLC) hardware for a
  screen-reader use case — stub it to always-idle/no tone/no ring.

## 2. ROM layout (verified SHA1, v2.0 firmware — first-half tag 23Jul84,
   second-half tag 02Jul84 — plus the "clean" DSP `204/205` pair which the
   driver's own comments say doesn't clip, unlike `165/166` or `409/410`
   — but see §21: `165/166` does not clip with v2.0, it is just quieter,
   and it is the pair v1.8 requires)

Main CPU ROM region is `ROM_REGION16_BE(0x40000, "maincpu")`: 16 chips of
0x4000 bytes each, **byte-interleaved** (`ROM_SKIP(1)`) into a linear
0x40000-byte big-endian word image. Load order/offsets (offset → chip →
verified SHA1):

```
0x00000 e8   e586de03e113683c2534fca1f3f40ba391193044
0x00001 e22  7954bb56b7591f8954403a22d34de31c7d5441ac
0x08000 e7   7724babf4ae5d77c0b4200f608d599058d04b25c
0x08001 e21  af5e4ea0b3631f7d6f16c22e86a33fa2cb520ee0
0x10000 e6   1b60cd71dfa83408b17e13f683b6bf3198c905cc
0x10001 e20  4ad0b00628a90085cd7c78a354256c39fd14db6c
0x18000 e5   e2b2415eec838ddd46094f2fea93fd289dd0caa2
0x18001 e19  92ab22a24484ad0d0f5c8a07347105509999f3ee
0x20000 e4   b5aec0bf37a176ff4d66d6a10357715957662ebd
0x20001 e18  891f3a3b4ce75ef14001257bc8f1f60463a9a7cb
0x28000 e3   4d6808f67cbdd316df23adc8ddf701df57aa854a
0x28001 e17  496c69e52cfa013173f7b9c500ce544a03ad01f7
0x30000 e2   de0c25687bab3ff0c88c98622092e0b58331aa16
0x30001 e16  c450abae0ccf372d7eb87370b8a8c97a45e164d3
0x38000 e1   355348bfc96a04193136cdde3418366e6476c3ca
0x38001 e15  01921e77b46c2d4845023605239c45ffa4a35872
```

Each pair loads at `offset` and `offset+1` with `ROM_SKIP(1)`, i.e. chip A
supplies even bytes, chip B supplies odd bytes, of each 0x8000-byte block.

DSP ROM region `ROM_REGION(0x2000, "dsp")`, two 0x800-byte chips,
byte-interleaved into a 0x800-word (2048 word) TMS32010 program image:

```
word offset 0x000 (even bytes) e70  3136bae243ef48721e21c66fde70dab5fc3c21d0  (23-205f4)
word offset 0x001 (odd bytes)  e69  9409f90f7a397b041e4440341f2d7934cb479285  (23-204f4)
```

All of the above were confirmed byte-for-byte against the user's dump on
2026-07-28 (`sha1sum` on `roms_extracted/*`).

## 3. 68000 memory map

Verbatim from the driver's own comment block (address lines a23..a1,
UDS/LDS via a0). Key regions:

| Range (before mirror) | Access | Function |
|---|---|---|
| `0x000000–0x03ffff` | R | ROM (mirrored across `0x740000`) |
| `0x080000–0x093fff` | RW | RAM (mirrored across `0x760000`) |
| `0x094000–0x0943ff` (umask 0x00ff) | W | Status LED byte |
| `0x094000–0x0941ff` (umask 0xff00) | RW | X2212 NVRAM direct read/write |
| `0x094200–0x0943ff` (umask 0xff00) | RW | NVRAM recall (R) / store (W) trigger |
| `0x098000–0x09801f` (umask 0x00ff) | RW | SCN2681 DUART (a0 not connected) |
| `0x09c000–0x09c001` | RW | SPC flags reg: d7 infifo-semaphore(R), d6 spc-irq-enable(RW), d5 fifo-error(R), d1 clear-error/semaphore(W), d0 speech-init/reset(RW) |
| `0x09c002–0x09c003` | W | SPC infifo write (clocks the 32-word input FIFO to the DSP) |
| `0x09c004–0x09c005` | RW | TLC flags (telephone/DTMF — stub, not needed) |
| `0x09c006–0x09c007` | R | TLC DTMF read (stub, not needed) |

## 4. TMS32010 side

- Program map: `0x000–0x7ff` = ROM (2048 words, matches the interleaved
  DSP image above).
- IO map: port 0 write = `spc_latch_outfifo_error_stats` (sets the
  infifo-semaphore true, latches a soft-error bit from data&1). Port 1
  read = pull next word from the 32-word infifo (`spc_infifo_data_r`);
  port 1 write = push a sample word into the 16-word outfifo
  (`spc_outfifo_data_w`, also clears DSP INT momentarily). `BIO` pin reads
  the infifo-semaphore (`spc_semaphore_r`) — DSP polls this to know if
  input is available.
- DSP `INT` (input line 0) is asserted whenever outfifo has room
  (`count < 16`) so the DSP knows it can push more samples; cleared when
  full or immediately after a port-1 write.
- 68K→DSP reset: SPC flags bit 0 (speech-init) drives the DSP's
  `INPUT_LINE_RESET` directly — set it to hold DSP in reset & clear both
  FIFOs; clear it to run.

## 5. Output audio pipeline

`outfifo_read_cb` fires at a fixed 10kHz, every tick pops one word off the
16-word outfifo (repeats last value if empty — this is what real hardware
does, not a bug to "fix"), transforms it `((data & 0xfff0) ^ 0x8000)`
(top bit inverted, bottom 4 bits dropped — 12-bit signed-ish DAC word),
and writes it to the AD7541. **Our emulator harness owns this timer loop**
and is the single source of truth for "what has been spoken so far" —
this is what makes sample-accurate synthetic indexing possible (§7).

## 6. DECtalk v2.0 in-line command language (verified from the actual 1984
   DTC-01 Owner's Manual, `EK-DTC01-OM-002`, 2nd ed. May 1984 — NOT the
   later "DECtalk Software" SDK docs at dectalk.github.io, which describe
   a different, later product generation with a superset command set that
   does not fully apply to this ROM)

- **Rate**: `[:ra n]`, n = 120–350 words/minute, default 180. (Later
  DECtalk Software calls this `[:rate]` — this hardware uses `[:ra]`.)
- **Pause**: `[:cp n]` comma pause, `[:pp n]` period pause (both affect
  effective WPM).
- **Punctuation / voice select / etc.**: built-in canned voices are
  selected with short mnemonic commands (`:np` Paul, `:nb` Betty, `:nh`
  Harry, `:nf` Frank, `:nr` Rita, `:nu` Ursula, `:nw` Wendy, `:nk` Kit,
  `:nv` the user-modifiable "Val" slot) — confirm exact set against
  Table 5-2 "New Voice Commands" before finalizing the NVDA voice list
  (not yet fully extracted — **follow-up needed**).
- **NO `[:index mark]` command exists in this firmware.** Confirmed by
  full-text search of the OCR'd manual — zero hits for "index mark" /
  "Index Mark" as a command. This is a later DECtalk Software addition.
  See §7 for how NVDA indexing is achieved anyway.
- **`[:dv ...]` Design Voice parameters** — Table 5-3, verified verbatim
  from the manual (OCR'd table was column-scrambled; reconstructed by
  matching the alphabetical abbreviation list against the matching
  alphabetical description list, both of which independently line up
  1:1):

| Abbr | Meaning | Min | Max | Unit |
|---|---|---|---|---|
| `ap` | Average pitch | 50 | 300+ | Hz |
| `as` | Assertiveness | 0 | 100 | % |
| `b4` | 4th formant bandwidth | 100 | 2048 | Hz |
| `b5` | 5th formant bandwidth | 100 | 2048 | Hz |
| `bf` | Beginning pitch baseline fall | 50 | 200 | Hz |
| `br` | Breathiness | 0 | 60 | dB |
| `ef` | End pitch baseline fall | 50 | 200 | Hz (OCR showed "dB"; almost certainly Hz to match `bf` — verify against ROM disassembly or real unit before shipping) |
| `f4` | 4th formant frequency | 2500 | `f5`-250 | Hz |
| `f5` | 5th formant frequency | coupled to `f4` (`f4`+250 .. ~4900) | | Hz — OCR-ambiguous, treat as coupled range, re-derive precisely later |
| `fo` | Forte voice | 0 | 100 | % |
| `ft` | F0-dependent spectral tilt | 0 | 100 | % |
| `g1`–`g5` | Synthesizer gain 1–5 | 0 | 80 | dB |
| `gf` | Gain of frication source | 0 | 80 | dB |
| `gh` | Gain of aspiration source | 0 | 80 | dB |
| `gn` | Gain of nasal resonator | 0 | 80 | dB |
| `gv` | Gain of voicing source | 0 | 80 | dB |
| `hs` | Head size | 75 | 150 | % |
| `la` | Laryngealization | 0 | 100 | % |
| `nf` | Samples in glottal pulse open phase | 0 | 60 | (int) |
| `p4` | Parallel formant 4 frequency | — | — | tied to `f4` |
| `p5` | Parallel formant 5 frequency | — | — | tied to `f5` |
| `pr` | Pitch range | 0 | 250 | % |
| `ri` | Richness | 0 | 100 | % |
| `sex` | 0=female / 1=male (also accepts `f`/`m`) | | | |
| `sm` | Smoothness (high-freq attenuation) | 0 | 24 | dB |

  Plus `list`, `listall`, `save` (actions, not values).

- **No native "hat rise" / "stress rise" / "quickness" / "lax
  breathiness" parameters exist on this firmware.** These are terms from
  later DECtalk Software (the user's original request used that later
  vocabulary). Map user-facing NVDA settings honestly:
  - "Head Size" → `hs` (real, direct match)
  - "Breathiness" → `br` (real, direct match)
  - "Laryngealization" → `la` (real, direct match) — do **not** invent a
    separate "lax breathiness" setting; it doesn't exist here.
  - "Baseline pitch contour" (bf/ef) is the real analog closest to what
    later docs call "hat rise" — expose as two settings (`bf`, `ef`) with
    honest labels, not a fabricated "hat rise" name.
  - "Assertiveness" (`as`) is the closest real analog to "stress rise" —
    expose it under its real name.
  - Do not expose settings the firmware doesn't have.

## 7. Synthetic indexing design (user-approved 2026-07-28)

Because the firmware has no index-mark concept, NVDA index tracking is
built entirely in our own driver/emulator layer, not derived from
anything the DTC-01 sends back:

1. Split each NVDA speech sequence into chunks at every `IndexCommand`.
2. Feed chunk N's text into the emulated infifo as normal.
3. Because our harness runs the *exact* cycle-stepped simulation (§5),
   it has ground truth for "has the firmware fully drained everything
   related to chunk N" — poll internal state (infifo empty AND outfifo
   empty AND held stable for a short debounce window) rather than
   inspecting the audio stream.
4. Only once that idle state is confirmed, fire the NVDA index callback
   for chunk N's IndexCommand and release chunk N+1 into the infifo.
5. Trade-off accepted by the user: a small, tunable silence gap at each
   index boundary (target: imperceptible, likely masked by DECtalk's own
   natural clause-boundary pausing) in exchange for guaranteed-correct
   index positions instead of timing-estimated ones.

## 6b. Built-in voice table

**CORRECTED 2026-08-13.** The table below came from the Owner's Manual
`EK-DTC01-OM-002`, **2nd ed. May 1984** — which predates the v2.0 firmware
(ROM halves tagged 02Jul84 and 23Jul84). It is the *v1.8* voice set. v2.0
has **ten** voices: it adds **Doctor Dennis** (`[:nd]`) and **Whispery
Wendy** (`[:nw]`). Note the ROM's spelling — "Whispery", not the
"Whispering" some secondary sources give.

Evidence, since a manual predating the firmware is exactly how this was
missed the first time:

- v2.0's ROM holds all ten names as a contiguous table at main-CPU
  `0x179AA`: `Perfect Paul\0Beautiful Betty\0…Doctor Dennis\0…Whispery
  Wendy\0Variable Val\0`. **The v1.8 image contains none of them.**
- Functionally, on v2.0 `[:nd]` and `[:nw]` produce clearly distinct audio
  (Dennis peak 27616 / rms 2052; Wendy peak 9776 / rms 804 / zcr 0.338 — the
  low amplitude and high zero-crossing rate of a breathy whisper, against
  Paul's 13536 / 1157 / 0.188). On v1.8 both are indistinguishable from
  Paul, while `[:nb]` stays distinct — so v1.8 has voices, just not these
  two, and an unrecognised `[:n_]` leaves the current voice in place.
- Their Design Voice defaults were queried from the ROM by
  `tools/dump_voice_defaults.py` (`[:dv listall]`), not transcribed: Dennis
  `ap=100 br=62 sm=100 g5=74`, Wendy `ap=264 br=58 ri=0 sm=100 g5=80`.

Consequences in the driver: `commands.voices_for(firmware)` filters the
table, `availableVoices` follows the selected firmware, and switching to
v1.8 while on Dennis or Wendy falls back to Paul with a logged warning —
otherwise the panel would name a voice the ROM silently ignores.

Original (v1.8) set — seven built-in voices plus one user-definable slot,
selected with `[:n_]`:

| Command | Name | Characteristics |
|---|---|---|
| `:np` | Perfect Paul | standard male |
| `:nb` | Beautiful Betty | standard female |
| `:nh` | Huge Harry | deep male |
| `:nf` | Frail Frank | older male |
| `:nk` | Kit the Kid | child's voice (10yo) |
| `:nr` | Rough Rita | deep female |
| `:nu` | Uppity Ursula | light female |
| `:nv` | Variable Val | user-definable (holds whatever `[:dv ... save]` last stored) |

A voice change needs a brief silence around it (the manual recommends
putting a clause boundary/comma before or after a mid-utterance `[:n_]`).

## 9. Session 1 status (2026-07-28/29) — read this first when resuming

**Architecture pivot:** Unicorn's M68K backend was abandoned (documented gap,
unicorn-engine/unicorn#1502, can't auto-vector internally-generated CPU
exceptions like privilege violation/address error, which the DTC-01's
mandatory boot self-test relies on) and `machine68k` (direct Musashi
binding) was abandoned too (broken Windows build script, no prebuilt
wheels). The M68000 core is now fully hand-written:
`emu/m68000.py` (registers/memory/exceptions) + `emu/m68000_ops.py`
(instruction set). **This is validated against Musashi's authoritative
opcode table (research/musashi_m68k_in.c, MIT-licensed) via
`tools/verify_opcodes.py`: 0 missing opcodes, 0 extraneous opcodes, across
all 65536 possible values.** Every bug that check caught (CLR/NEG swapped,
EXT size-bit position, CHK missing Dn-direct/immediate forms, MOVEM
predecrement/postincrement backwards, ABCD wrong opmode, several
byte-size-to-An-register and control-addressing-mode over-permissions) is
fixed. Re-run this checker after touching any instruction-table code.

**Current working state:** boots the real ROM cleanly with **no patches or
bypasses** (the old `SELF_TEST_SKIP_PC` hack was needed only for Unicorn
and is gone). Confirmed working: full self-test pass, DUART host-channel
handshake (CTS/DSR spoof + the documented IP4-second-read hack), default
NVRAM image loaded from ROM_FILL data, text sent over the DUART host
channel (`machine.duart.feed_rx_b(...)`) is correctly received byte-by-byte
via RHRB (confirmed by watching `duart.b.rx_queue` drain to empty).

**Where it's stuck:** received host text never reaches the SPC/DSP input
FIFO (`machine.infifo_count` stays 0, `m68k_infifo_w` is never called), so
no synthesis happens. `tools/disasm68k.py` (a minimal disassembler built
this session, mnemonic ID cross-checked against the same Musashi table)
shows the firmware settles into `0x10D8: BRA $0010D8` with interrupts fully
enabled (SR=0) after boot -- this looks like a legitimate idle loop (the
firmware appears to implement lightweight cooperative multitasking using
RTE as a generic "restore saved SR/PC and jump" context-switch primitive,
not a crash), waiting for an interrupt to hand control to a runnable task.
DUART RX-ready interrupts clearly *are* being serviced (that's how bytes
get drained from `rx_queue`), so a receive handler runs -- it just isn't
handing characters on to speech processing. Two rejected/inconclusive
hypotheses this session: (1) a fake phone-ring stimulus *did* move execution
into new code (0x213E-0x21B8, a TLC/telephone state machine keyed off a
struct at `$80552`) but led back into the same idle loop -- likely an
unrelated parallel subsystem, not the real blocker; (2) faking
tone-detect made no observable difference at all.

**Next steps to try when resuming:**
- Use `tools/disasm68k.py` to read the DUART RX-ready interrupt handler
  itself (find its vector -- IRQ6 handler address at ROM offset 0x78 --
  and disassemble forward from there) to see what it does with each
  received character and what condition it's waiting for before handing
  off to the SPC (likely: waiting for a specific terminator, a line-buffer
  full condition, or a "we're in on-line/host-speak mode" flag this
  session never confirmed is actually set).
- Check whether our default NVRAM image (section 2's factory-default
  bytes) actually encodes "HOST SPEAK: on" per the manual's stated
  default, or whether it's misconfigured -- the driver author himself
  never fully validated this image ("calculated some weird way which I
  haven't figured out yet"). Correlate NVRAM byte offsets against the
  manual's Table 3-2 SET command parameters (host format/speed/speak,
  local host/speak/edited, etc.) if a mapping can be derived.
- Consider dumping/disassembling the dispatch table at ROM `$2666` (3
  entries, functions at 0x1184/0x15D8/0x22AA) more fully -- entry 0
  (0x1184) is the self-test-adjacent code already explored; 0x15D8 and
  0x22AA (the TLC/ring code from hypothesis 1) haven't been read in detail.

**Tooling added this session** (all in `tools/`, dev-only, not shipped):
`build_rom_images.py`, `smoke_test_dsp.py`, `boot_test.py`, `diag_step.py`,
`speak_test.py`, `wav_writer.py`, `verify_opcodes.py`, `disasm68k.py`.
`research/` holds the downloaded MAME driver source, Musashi opcode table,
and the OCR'd DTC-01 Owner's Manual -- all reference material, not shipped.

## 10. Session 2 status (2026-07-30) — read this first when resuming

Picked up exactly where session 1 left off (§9's "next steps"): disassembled
the level-6 (DUART) interrupt handler and traced the full path a received
host-channel character takes, using `tools/disasm68k.py` for static reading
and two new live-instrumentation scripts (`tools/trace_spc_queue.py`,
`tools/trace_task_wait.py`) that monkeypatch `SystemBus.write8/16/32` to log
writes to specific regions with the executing PC, run against the real ROMs
via `DectalkMachine` directly (not through `emu.uc`/Unicorn — those are gone
per §9's architecture pivot).

> **Correction (2026-07-31):** this section previously said *both*
> `tools/boot_test.py` and `tools/diag_step.py` were stale pre-pivot tools
> referencing a `m.uc` attribute. Only `diag_step.py` was — it imported
> `unicorn` directly and has been deleted. `boot_test.py` never touched
> Unicorn and still runs correctly against the Python core; it was checked
> before removal and reports the LED self-test progression
> (`0x00` → `0xff` → `0xda`) as expected. Use `tools/speak_test.py` as the
> reference for driving `DectalkMachine`.

**Confirmed call chain, RX side:**
- Level-6 vector = `0x1706` → saves regs, calls a RAM function-pointer at
  `$080532` (set up at boot, presumably normally = the dispatcher below),
  then `JMP $1132` (continuation).
- The DUART ISR-bit dispatcher lives at `0x1718`: reads ISR (`$09800B`),
  loops handling each set bit, re-reading ISR each pass, until 0. Bit
  layout confirmed against source: bit0/4 = TxRDY A/B → `0x1800`; bit1/5 =
  RxRDY A/B → `0x1878`; bit3 = counter/timer tick → `0x0B7A`; bit2/6 =
  delta-break A/B → sets a status byte; final unconditional tail (`0x17C2`)
  recomputes and writes the LED byte (`$094001`) from IMR + a state var at
  `$080536`, optionally calling a hook at `$08053A` if `$080538` is set.
- Channel B (host, our channel) RX → `0x1878` with `A1 = $080328` (the
  channel-B "line" struct). This routine does XON/XOFF handling (checks
  D0==0x11/0x13, sets a pause flag at offset 65) then, for a normal data
  byte, falls to `0x1954` → `JSR $000CFE` with `D0`=the received byte,
  `A2`=a queue-descriptor pointer (not literally `$080328`; comes from
  `70(A1)` deref, i.e. indirection through the line struct — see below).
- **`0x0CFE` is a generic OS primitive** (receive-a-char-into-a-queue /
  wake-a-waiter), reused by other drivers too — not host-channel-specific.
  Logic: if `16(A2)` (offset 0x10, "task waiting on this queue" pointer)
  is nonzero, it hands the char directly to that task and wakes it by
  inserting it into the OS ready-queue at `$080000` via the generic
  sorted-list-insert helper at `0x2334`. If `16(A2)` is zero, it instead
  calls a buffering path at `0x0FFE` (not yet disassembled) via
  `12(A2)` — i.e. just stores the byte, no task involved.
- **Live-traced with `tools/trace_task_wait.py`**: `$080328+0x10` (the
  waiting-task pointer for channel B's queue) reads **`0x00000000` both
  before and after** feeding `"[:np] Hello world.\r"` and running 2
  virtual seconds. **No task is ever registered as waiting on host-channel
  input.** Every received byte therefore takes the silent-buffer branch
  (`0x0FFE`), and nothing ever drains that buffer.

**Confirmed call chain, SPC (speech) send side** (the other end of the
pipe, independently traced from the MMIO addresses in DESIGN.md §3):
- Static search of every absolute reference to `$09C000`/`$09C002`
  (spc-flags / infifo-write MMIO) in the ROM found them **all** clustered
  in one module, `0x13C00`–`0x13F98`. This is the actual "feed the SPC"
  code: `0x13CC8` bursts 24 words at a time into `$09C002` (infifo write);
  `0x13D1E` does the DSP reset/handshake sequence (write speech-init,
  clear it, poll bit 0x80 = infifo-semaphore for DSP-ready).
- Level-5 (SPC) interrupt vector = `0x231C` (matches `IRQ_SPC` in
  `machine.py`) → its entire body is: push a queue-descriptor pointer
  (`$08273C`), `JSR $13C7C`, `RTE`. So **every** SPC interrupt just asks
  "is there a queued utterance buffer ready to send?".
- `0x13C7C`/`0x13D48` implement a classic linked-list send-queue: head
  `$08272A`, tail `$08272E`, descriptor base `$082726` (offsets +4/+8).
  Dequeues one node (`A3`), sends its buffer via `0x13CC8`, on empty
  clears `$09C000` and returns. **Static search found zero references to
  `$08272A`/`$08272E`/`$082726` anywhere in the ROM outside this one
  module** — meaning the *producer* (whatever is supposed to enqueue a
  synthesized utterance here) either doesn't exist in this firmware image
  in the form expected, or reaches this queue through a pointer passed in
  a register rather than a literal address (not yet located).
- **Live-traced with `tools/trace_spc_queue.py`**: watched all writes to
  `$082700`–`$082780` (the whole queue-descriptor region) and to
  `$09C000`–`$09C007` (SPC MMIO) for the entire run. Only 84 writes total,
  **all during the 0.3s boot settle** (a zero-init loop at `PC=0x001046`
  and a ROM→RAM init-table copy at `PC=0x001076`) — literally **zero
  writes of any kind after the text is fed**, confirming synthesis is
  never even attempted, consistent with (and now more precisely located
  than) §9's original "`m68k_infifo_w` is never called" observation.

**Conclusion — the real blocker is upstream of both ends**: nothing ever
creates/starts the host-channel-input-processing task (the piece that
would (a) register itself as the waiter on `$080328`'s queue so `0x0CFE`
wakes it per-character instead of silently buffering, and (b) turn
buffered text into phonemes and enqueue a buffer at `$08272A`/`$08272E`
for the SPC ISR to send). This is consistent with §9's `BRA $0010D8` idle
loop finding — there's simply no runnable "host speak" task in this
firmware image as currently booted.

**NVRAM investigated and ruled out as a simple "wrong default bit" bug**
(the hypothesis this session set out to test first): MAME's driver source
comments (`research/mame_dectalk.cpp` lines 129-136, 960-977) document that
the real factory-default NVRAM image is embedded in the ROM at main-CPU
address `0x1A7AE` (0x80 bytes), decoded by taking, for each of the 64
16-bit ROM words there, all four nibbles in turn as four successive NVRAM
bytes (`word 0x0005` → nvram bytes `05 00 00 00`). **Independently decoded
this directly from our own dumped ROM** (not just trusting MAME's comment)
and it reproduces `machine.py`'s `_DEFAULT_NVRAM` array byte-for-byte at
every populated offset — so our default image is a faithful copy of the
real one, not a guess. Also found and disassembled the actual NVRAM-recall
+ checksum-validate routine (`0x10F52`, matches MAME's noted "`$10f52`
entry point for nvram check routine"): it reads all 256 NVRAM MMIO nibble
bytes (`$094000`-`$0941FF`), repacks them into a 64-word block, runs an
XOR/rotate checksum over words 0-62 against the stored checksum in word 63
(byte offset 0xFC, matches §2/§9's already-known checksum bytes), and
separately requires word 0 == 5 (a validity magic number — also matches:
our default's offset-0 nibble is 5). All consistent and passing, which
also matches Session 1's observation of a clean self-test with no NVR
FAULT LED code. **Conclusion: the recalled NVRAM block is valid and
correctly loaded — this is not a checksum/corruption bug.** What remains
unknown is the *semantic* mapping of each of the 12 populated nibble
fields (offsets 0,4,8,0xC,...,0x2C) to named Table 3-2 SET parameters
(HOST FORMAT/SPEED/SPEAK, LOCAL HOST/SPEAK/EDITED/HARDCOPY/SPOKENSETUP,
LOCAL SPEED/FORMAT, MODE flags, LOG flags) — the manual doesn't document
byte layout, so this can only come from disassembling how the recalled
64-word block (copied further into a per-field structure around
`0x8279C`-`0x827B8`+ during boot, seen starting at `0x1108C`) is consumed.

**New, stronger finding this session — likely the real root cause
location**: reused `tools/trace_task_wait.py`'s existing log of every
write to the OS ready-queue anchor (`$080000`-`$08000C`) during the 0.3s
boot settle. Only **one single task TCB address, `$008297C`, is ever
inserted** into the ready queue — 4 total writes, all cycling the same
address (queue-anchor self-init at `0x10A8-0x10B4`, then that one TCB
inserted/manipulated at `0x11C6`/`0x2370`/`0x1166`/`0x239A`). **No second
task is ever created or scheduled during boot.** This is consistent with
(and narrows down) §9's `BRA $0010D8` idle-loop finding: there is
apparently exactly one runnable task in the whole running system — almost
certainly the idle task itself — and no "host speak" input-processing task
ever exists to begin with. This reframes the problem: it may not be that
an existing host-task fails to register as a waiter (as previously
theorized) so much as that **the host-task is never spawned in the first
place**, in which case the fix isn't a config-bit flip but finding why
task creation for it doesn't happen (or doesn't happen in our emulated
environment specifically — e.g. gated on a hardware signal we're not
asserting, not just an NVRAM value).

**Next steps to try when resuming:**
- Find the task-*creation* OS primitive (allocates a TCB, sets its entry
  PC — distinct from `0x2334`'s ready-queue insert, which only reschedules
  an *existing* TCB) and enumerate every call to it during the boot
  sequence (same live-trace-with-PC-logging technique as
  `trace_spc_queue.py`/`trace_task_wait.py`) to get a definitive list of
  which tasks this firmware actually spawns in our emulated environment.
  If a host-task creation call exists but is skipped, find its guard
  condition next (may still turn out to be an NVRAM/config check, just at
  creation time rather than at wakeup time as first assumed).
- If no host-task creation call exists in the boot path at all, work
  backward from TCB `$8297C` (the one task that *is* created — dump its
  entry point / stack setup to confirm it's really the idle task) to find
  the boot-time task-spawn table/loop it came from, and see what other
  entries that table has that we're not reaching (could be gated by a
  hardware strap/DIP-switch condition our emulation isn't asserting,
  distinct from the NVRAM values already ruled out above).
- Still-open from earlier: disassemble `0x0FFE` (the silent-buffer path in
  the generic char-receive primitive) and `0x1132` (continuation after the
  level-6 handler's `$080532` function-pointer call) — unread, could
  contain relevant logic. Also still unconfirmed: what `$080532` actually
  points to at runtime (assumed `0x1718` from context, never live-checked).

**UPDATE, same session, root cause found and confirmed**: traced the boot
sequence exhaustively rather than guessing at NVRAM semantics further.
`0x102C` is the actual post-self-test boot continuation (documented by
MAME's driver comment as the self-test-skip target): it sets up a heap
allocator at `$082978`, statically creates exactly one task (the idle
task, entry point `0x000010D8` — now 100% confirmed, not just inferred)
at fixed address `$080018`, then walks a **3-entry ROM table at
`0x2666`-`0x2696`** calling each entry's init function. All 3 entries are
now identified: `0x1184` (self-test-adjacent, per §9), `0x22AA` (TLC/
telephone subsystem, per this session's earlier TLC init trace), and
`0x15D8` (newly read this session) — **there is no 4th subsystem-init
call anywhere in the boot path**, so nothing else is "supposed" to run at
boot beyond these three.

`0x15D8` programs the SCN2681 DUART hardware directly (baud/mode/command
registers for both channels), sets up the channel-A/B line-discipline
structs (confirms `$080532 = 0x1718`, matching what we already verified
live), and calls `JSR $1B8E`. **`0x1B8E` is the connection state
machine's boot-time init**: it puts the system into "state 5, first
500ms" (LED byte written once: `0xDA` — matches the LED value observed
unchanged for the *entire* run, confirming this is a one-time write, not
a live/current status as originally assumed) and explicitly **clears**
`$080538`/`$08053A` (the hook-enable flag and hook pointer). The function
that *would* advance past this state — `0x1BF4` (reads CTS/DSR/DCD via
the DUART's IP register, transitions state, and **does** arm
`$080538=1` + a real `$08053A` handler address) — is never called by
anything in the boot path. It's designed to be invoked via a **software
delayed-callback timer list** (walked by `0x0B7A`, dispatched from DUART
ISR bit 3 "counter ready", head pointer at `$080118`) — i.e. the
intended mechanism is "arm a ~500ms hardware timer, and when it fires,
call `0x1BF4` to check the modem lines and progress the state machine."

**Confirmed empirically**: `$080118` (the timer list head) is `0x00000000`
(no timer ever armed) and, more tellingly, `$08010E` (a global tick
counter incremented once per DUART counter-interrupt service in `0x0B7A`)
stays **exactly `0x00000000` after 7+ virtual seconds** of run time
across two text feeds. **The DUART's counter/timer interrupt has never
fired once.** This fully explains every symptom traced this session: no
second task, empty SPC queue, null `$08053A` hook, frozen LED — the
firmware is stuck at the very first checkpoint of its boot sequence,
waiting on a timer interrupt our emulation never generates.

**Root cause, confirmed by reading the emulation code directly**:
`emu/duart2681.py`'s `SCN2681` class has **no counter/timer emulation at
all**. `_isr()` (lines 206-216) computes only TXRDYA/RXRDYA/TXRDYB/RXRDYB
— there's no "counter ready" bit (real SCN2681 ISR bit 3, `0x08`). The
CTU/CTLR count-value registers (offsets `0x0C`/`0x0E`) aren't stored
anywhere (`write()` has no case for them — silently dropped). Offsets
`0x1C`/`0x1E` are implemented only for their *write*-side meaning
(Output-Port set/reset, per the file's own docstring) — the real SCN2681
also defines a *read*-side meaning for those same addresses
(start-counter / stop-counter commands), which `read()` doesn't
implement at all (falls through to `return 0`). There is no periodic
countdown/tick mechanism anywhere in the class.

**This is the actual, scoped bug**, not a config/NVRAM issue as
originally suspected (NVRAM was independently verified correct earlier
this session — see above). Fix sketch, not yet implemented (pending
user go-ahead): store CTU/CTLR on write; implement start/stop-counter
semantics on read of `0x1C`/`0x1E` (arm/disarm an internal countdown);
add a step function called from `machine.py`'s existing virtual-time
loop (same place DSP cycles and DAC sampling are already stepped) that
decrements the countdown using the DUART crystal's documented rate
(`3.6864MHz`, DESIGN.md §1 — real SCN2681 counter mode typically clocks
at crystal÷16; verify against ACR clock-source-select bits the firmware
actually programs rather than assuming); on expiry, set ISR bit `0x08`
and run it through the existing `_update_irq()`/IMR gating, which is
already correctly wired to `IRQ_DUART`. **This one fix is expected to
unblock the entire pipeline** traced this session (state machine
progression → `$08053A` hook → whatever ultimately creates the
host-speak task/enqueues SPC buffers), though that downstream chain
past `0x1BF4` hasn't been traced yet and should be re-verified once the
timer fix lands (things could still be stuck one step further in — this
should be treated as "removes the currently-known blocker," not
"guaranteed to produce audio" until re-tested).

## 11. Session 2 continued — DUART counter/timer implemented, SPEECH WORKS

**Implemented the fix described above** in `emu/duart2681.py`: added
`ctur`/`ctlr` register storage (writes to offsets `0x0C`/`0x0E`), a
`start_counter()`/`stop_counter()` pair wired to reads of offsets
`0x1C`/`0x1E` (previously unhandled), a `step(dt_seconds)` method that
converts elapsed wall-clock-equivalent time to clock ticks using
`ACR`'s mode/clock-source-select bits (crystal ÷16 or ÷1 for the
crystal-derived modes this firmware actually uses; IP2/TxC-derived modes
are left at 0 Hz since nothing models those external clock sources), and
an `ISR_COUNTER_READY` (`0x08`) bit wired into the existing `_isr()`/
`_update_irq()`/IMR gating. `machine.py`'s `run_seconds()` now calls
`self.duart.step(elapsed)` once per 68000 instruction, alongside the
existing DSP-cycle and DAC-sample stepping.

**One correction made during implementation, found empirically**: the
first version (stop-counter unconditionally halts counting, matching a
literal one-shot "Counter mode" reading of the datasheet) produced
exactly **one** tick ever, then silence — confirmed by live-tracing every
DUART register access with the executing PC: the firmware's own boot
init (`0x15D8`, tail instruction `TST.B 29(A0)` at ROM offset `0x1700`,
executing with `A0` still `= $098000`) issues exactly one Start-Counter
command, and its ISR bit-3 handler (`0x1750`, `TST.B $0009801F`) issues a
Stop-Counter read **unconditionally on every tick as its interrupt-ack**,
with no re-arm call anywhere else in the ROM (confirmed by scanning ALL
DUART register reads over a 5-virtual-second run — exactly one start,
one stop, nothing else). Since real firmware obviously wouldn't design a
system tick that kills itself on first use, this only makes sense if
**Stop-Counter in Timer mode doesn't actually halt counting** (only
Counter/one-shot mode truly stops; Timer mode's stop just acks/clears
the interrupt and disables the OP3 output pin, per the standard SCN2681
command semantics) — implemented that way in `stop_counter()`, and it
immediately produced a steady ~100-190 tick/sec heartbeat matching the
programmed `CTUR:CTLR=0x0480`/crystal-÷16 math.

**Wrong turn, corrected**: with the heartbeat now running continuously,
`$08010E` (global tick count) climbed steadily but `$080538`/`$08053A`
(the FSM hook) stayed null for 10+ virtual seconds regardless — so the
heartbeat alone wasn't sufficient, and chasing this further revealed a
**misreading from earlier in this session**: `0x1BF4` (the CTS/DSR-check/
state-advance function) has exactly one caller in the whole ROM, at
`0x1B66`, inside a small per-channel dispatcher (`0x1B56`-`0x1B76`) that
explicitly special-cases channel B: `CMPA.L #$00080328,A0; BEQ
$001B74` — **if the channel is B (the host link), it branches straight
to "return 0," skipping the FSM-advance call entirely.** The whole
`0x1BF4`/`0x1B8E`/LED-state-machine/CTS-DSR/"moving data" apparatus is
**channel-A-only** (the local/modem port) — consistent with DESIGN.md
§1's original guidance that channel A is "an unused secondary passthrough
... ignore it." This session's earlier framing of the LED byte as
somehow gating host-channel readiness was a mistaken inference; it does
not apply to channel B at all. (This dispatcher itself has no static
callers found either — it's reached via a stored callback pointer, not
worth chasing further now that it's understood to be irrelevant to the
host path.)

**Re-ran the earlier task/queue diagnostics after the fix, on channel B
specifically, and the whole pipeline unblocked**: feeding text now
causes `$080328+0x10` (channel B's "waiting task" pointer, stuck at
`0x00000000` for the entire previous session) to become
`0x0008297C` — a task is now genuinely blocking on host-channel input
and getting woken per-character, exactly as the `0x0CFE` primitive was
always designed to do. End-to-end (`tools/speak_test.py`, ROM booted,
settled 0.5s, fed `"[:np] Hello world.\r"`, run 6s):
- **Host TX now produces real bytes** (was always exactly 0 before):
  `b'>[:np] Hello world.\r\n>'` — a clean local-echo-with-prompt,
  exactly matching expected DEC terminal-driver behavior.
- **Audio is no longer frozen**: samples were literally `-32768`
  (silence baseline) for 100% of every prior run, forever. Now: silence
  until t≈0.5s, then ~2.5s of densely-varying audio (t≈0.6-3.1s,
  hundreds of unique sample values per 100ms block — consistent with
  real formant synthesis, and a very plausible duration for "Hello
  world." at the ~180 WPM default), a brief gap, a second ~0.9s burst
  (t≈3.6-4.5s, plausibly the terminal echo line itself being spoken),
  then silence again. This is a speech-shaped envelope, not noise or an
  artifact.

**Bottom line: this was the actual root-cause fix.** The DUART
counter/timer was genuinely unimplemented (confirmed by reading the
emulation code directly), the firmware's boot sequence and text-reception
path were otherwise already completely correct (every piece traced this
session — self-test, ROM/NVRAM, DUART wiring, RX line discipline, task
wake primitive, SPC send-queue mechanics — turned out to be fine once
the missing system tick was supplied). Not yet done: this hasn't been
verified by ear (no audio playback available in this environment) or
cross-checked against a spectrogram/known-good reference; the WAV at
`build/speak_test.wav`/`speak_test2.wav` should be listened to before
declaring this fully proven, and voice-command edge cases (other `[:n_]`
voice selectors, `[:dv]` parameters, multi-utterance queuing, the
still-undocumented Table 5-2 mnemonic set) remain open per §8 below.

## 12. Session 2 continued — user listening test, audio DC-offset bug found and fixed

**User confirmed by ear**: "definitely speech ... understandable," validating
§11's fix end-to-end for the first time. But: "a click, some silence, then
speech ... extremely, extremely loud ... about 30dB higher than it should
be."

**Root cause, found and fixed**: `tools/wav_writer.py`'s `on_sample()`
stored the `on_audio_sample` callback's value as-is, on the mistaken
assumption (stated in its own old docstring) that
`machine.py`'s `_dsp_pop_outfifo` transform (`(data & 0xfff0) ^ 0x8000`,
which is correct and should **not** change — it's the exact real-hardware
AD7541 DAC input code, matching MAME) was already standard signed PCM.
It isn't: that XOR converts the DSP's natural signed sample into
*offset-binary* (what a real DAC chip needs — silence maps to mid-scale
code `0x8000`, which is the DAC's 0V point). Storing `0x8000` directly
and packing it as a 16-bit sample means it reads back as signed **-32768**
— the extreme negative rail, not center. Voiced audio swinging up from a
baseline pinned at the negative rail can wrap past `+32767` back around
to strongly negative mid-utterance, producing exactly the harsh,
clicky, over-loud distortion reported.

**Fix**: XOR by `0x8000` a *second* time in `on_sample()` before storing,
undoing the offset-binary conversion and recovering the DSP's natural
signed sample (silence at `0`, correctly centered). Verified: stats went
from `{min: -32768, max: -32768}` (frozen, pre-§11-fix) →
`{min: -32768, max: 32752}` (post-§11-fix, but DC-shifted/wrapping,
matching the "very loud" report) → `{min: -11792, max: 14608}` (after
this fix — silence sample-blocks read exactly `0`, voiced blocks land at
a sane ~±5000-12500 out of the full ±32768 range, no rail-pinning or
wraparound). New reference file: `build/speak_test3.wav` (not yet
re-confirmed by ear as of this writing — do that before considering this
closed).

**Important for future work**: this bug lives in the dev-only WAV
exporter, not in `machine.py` (which stays hardware-accurate on purpose).
**The eventual real NVDA-driver audio-output code will need this same
"XOR 0x8000 again" correction wherever it turns `on_audio_sample` values
into actual playback samples** — don't copy the old (buggy) assumption
that the raw callback value is ready-to-play PCM.

## 13. Session 3 (2026-07-30) — real-time performance investigation

Before wiring the emulator into an actual NVDA `synthDrivers` package (the
existing `addon/synthDrivers/dectalkDtc01/__init__.py` is still an empty
stub), measured whether the pure-Python emulator can keep up with live
audio playback. **It can't, by a wide margin**: baseline was
**~0.138x real time** (speaking phase) / **~0.120x** (boot settle) --
roughly an 8x slowdown, dominated by the TMS32010 DSP core (`cProfile`:
~60-65% of total time in `tms32010.step()`/`run()`, vs. ~20% in the
68000 core).

User chose "try pure-Python optimization first" over going straight to a
compiled extension. Applied, in order (correctness re-verified after
each step via identical `speak_test.py` host-TX bytes + audio stats, and
`tools/verify_opcodes.py` re-run clean at the end -- 0 missing/extraneous
68000 opcodes):
- `__slots__` on `TMS32010`, `Channel`, and `SCN2681` (removes
  per-instance `__dict__` overhead on the hottest classes; skipped on
  `M68000`/`M68000Core` -- it's built via multiple inheritance from
  `M68000OpsMixin` + `M68000Core`, so `__slots__` there risks layout
  conflicts for a smaller relative payoff, since the 68K core is a much
  smaller share of total time than the DSP).
- Hand-inlined `_ind()`/`_dma_dp()`/`_dma_dp1()`/`_update_ar()`/
  `_update_arp()` directly into `_getdata`/`_putdata`/`_putdata_sar`/
  `_putdata_sst` (by far the hottest TMS32010 methods) -- exact same
  evaluation order preserved (address computed from pre-update AR,
  AR/ARP updated after, `_putdata_sar`'s register read happens after the
  AR update, matching the original). The non-inlined helpers are kept
  for `_op_larp_mar`'s use.
- Merged `TMS32010.run()` and `.step()`: `run()` (the actual hot path
  called from `machine.py`) now has `step()`'s full body inlined into
  its loop (with `program`/dispatch tables/`ADDR_MASK` hoisted to
  locals) instead of calling `self.step()` per iteration.
  `step()` itself is kept unchanged as the single source of truth
  (still called directly by `tools/smoke_test_dsp.py`) -- **if
  instruction dispatch/interrupt logic in `step()` ever changes, mirror
  the change into `run()`'s inlined copy, and vice versa.**
- Cached the DUART's `_counter_clock_hz()` result (depends only on
  `acr`, which is essentially write-once at boot) instead of
  recomputing it on every `duart.step()` call (called once per 68000
  instruction).
- `machine.py`'s `_pending_irq_level()`: was rebuilding a list and
  calling `max()` from scratch on every single 68000 instruction;
  replaced with an incrementally-maintained `self._pending_irq`,
  updated only when `_set_irq()` actually changes a line (IRQ lines
  change far less often than instructions execute).
- `SystemBus.read8/write8/read16/write16`: fast-pathed the two
  dominant regions (ROM, RAM) with a direct bounds check before falling
  back to `_region()`'s full region-by-region dispatch for the less-hot
  MMIO regions.
- Inlined the trivial `_op_h` property (`(opcode>>8)&0xFF`, extremely
  hot -- read by nearly every arithmetic/shift opcode) and the `s16()`
  sign-extension helper at its two hottest call sites (`_getdata`'s
  signext branch, `_op_mpy`).
- `M68000.fetch16()`: inlined the trivial `_check_align` call.

**Result: ~1.37x** (speaking phase: 0.138x → **0.189x** real time;
boot settle: 0.120x → **0.163x**). Matches the "recommended" option's own
stated expectation almost exactly ("realistic best case is maybe 2-4x...
likely not enough alone") -- landed at the low end of that, and it is
**not enough**: 0.189x is still roughly a 5.3x slowdown from real time,
nowhere close to usable for a live NVDA synth driver. Diminishing
returns had clearly set in by the last couple of changes (negligible
measured delta). Going further with pure-Python micro-optimization
is not expected to close a 5x+ gap.

**Conclusion for next steps**: a compiled extension (Cython or a small C
module) for the TMS32010 hot path -- and possibly the 68000 core too,
though it's the smaller contributor -- is very likely necessary to reach
real-time. This was flagged as the fallback option in the original
choice between approaches; the pure-Python pass wasn't wasted work (it
meaningfully shrinks what the compiled path needs to cover, and all the
changes are behavior-preserving optimizations worth keeping regardless
of what comes next), but it doesn't get to a usable place on its own.

## 14. Session 3 continued — native C core: 0.189x → ~10x realtime

Pure-Python optimisation (§13) topped out at 0.189x, still ~5x short of
realtime, so the emulator was rebuilt as a native DLL. **Result: ~10x
realtime, a 53x speedup over the optimised Python**, verified correct
against the Python reference.

### Architecture

Everything runs in C; Python only pulls finished audio.

- **68000: vendored Musashi 4.60** (`native/musashi/`, MIT). Not
  hand-ported — it's the same lineage MAME's own dectalk driver uses, and
  crucially it *does* auto-vector internally-generated exceptions
  (address/bus error), the exact gap that killed Unicorn back in §9. Its
  `m68k_in.c` is byte-identical (SHA1 `f8edf509…`) to the
  `research/musashi_m68k_in.c` our Python core was validated against, so
  both cores derive from the same opcode ground truth. See
  `native/musashi/VENDORING.md` for config and a **critical build
  gotcha** (m68kmake must be built `/Od`; `/O2` miscompiles it).
- **TMS32010** (`native/tms32010.c`), **SCN2681** (`native/duart2681.c`),
  **machine glue** (`native/dtc01.c`) — ported from the Python, quirks
  intact, including the Timer-mode stop semantics from §11.
- **Binding: ctypes to a flat C DLL**, not a CPython extension. NVDA
  ships its own Python whose version moves independently of the dev
  environment; a CPython-ABI extension would need rebuilding per version,
  a plain DLL does not. The boundary is crossed once per audio chunk, so
  ctypes' per-call overhead is irrelevant. Wrapper:
  `addon/synthDrivers/dectalkDtc01/emu/native.py`.

Why the *whole* machine and not just the DSP: profiling put the DSP at
~65% of runtime, so by Amdahl even an infinitely fast DSP caps the gain
at ~2.9x (→0.54x realtime). Porting only the hot part could not reach
realtime; and a per-instruction Python↔C boundary would have cost more
than it saved.

### Two real bugs found during bring-up (both worth remembering)

1. **`m68k_init()` was never called** — Musashi builds its opcode jump
   table there; without it the first instruction jumps through a null
   handler. Symptom: access violation on first execute.
2. **Musashi auto-clears its own interrupt level.** With
   `M68K_EMULATE_INT_ACK` off it does `CPU_INT_LEVEL = 0` the moment it
   takes an interrupt (m68kcpu.h ~line 2157), expecting the board to
   re-assert. Our IRQ lines are level-triggered, and `set_irq()`
   originally notified Musashi only when the computed level *changed* —
   so exactly one interrupt was ever delivered and the firmware's system
   tick froze at 1. This reproduced §10's symptoms exactly (LED stuck at
   `0xDA`, no speech), which is a good reminder that *identical symptoms
   can have unrelated causes*. Fix: the run loop re-asserts
   `pending_irq` every instruction.

### Correctness: `tools/compare_native.py`

Runs both cores on identical input and checks logical behaviour. Latest:

```
host TX  python: b'>[:np] Hello world.\r\n>'
host TX  native: b'>[:np] Hello world.\r\n>'     -> MATCH (exact)
LED      python: 0xda   native: 0xda             -> MATCH
audio    exact-equal samples: 27663 (42.56%)
  python  min=-11792 max=14608 rms=1793.5
  native  min=-12336 max=14608 rms=1790.0
  speech blocks (100ms): python=36  native=36
  speech envelope MATCHES; rms ratio 0.998
unmapped memory accesses: 0
RESULT: PASS
```

**Bit-identical audio is not expected and not required.** The Python core
carries its own *approximate* 68000 cycle counts; Musashi's are accurate.
That shifts the 68000↔DSP interleaving slightly, so individual samples
differ (~43% happen to match) while everything that matters is the same:
identical host-TX protocol bytes, identical speech duration (36 blocks
each), identical peak amplitude, RMS within 0.2%. The check asserts those
invariants rather than sample equality.

**Confirmed by ear (2026-07-31)**: the user listened to
`build/compare_native.wav` against the known-good `build/speak_test3.wav`
(the Python output validated in §12) and reports them **indistinguishable**.
So the ~57% of samples that differ numerically are perceptually
irrelevant, as the envelope/RMS analysis predicted. The native core is
now the validated implementation; the Python one remains the readable
reference and the oracle for `tools/compare_native.py`.

### Performance

| workload | realtime factor |
|---|---|
| cold boot + 0.5s settle | 0.12s wall (total) |
| short utterance (6s) | **10.1x** |
| typical sentence (10s) | **10.3x** |
| long paragraph (30s) | **10.6x** |

~10x with comfortable headroom for a screen reader, and the 0.12s cold
boot means synth startup isn't a noticeable stall. `dtc01_is_idle()` was
verified to do what §7's synthetic-indexing design needs (True before
feed, False while speaking, True again once both FIFOs drain).

### Build

`tools/build_native.bat [x64|x86]` → `build/dtc01_{x64,x86}.dll`.
Both build clean, and both import **only `KERNEL32.dll`** (built `/MT`,
static CRT) — so no VC++ redistributable is needed and they load in any
process.

**CORRECTION (2026-07-31): NVDA is x64, not x86.** Earlier notes in this
section claimed "x86 is what NVDA ships" — that was an out-of-date
assumption, checked and disproved against the actual installation:

```
C:\Program Files\NVDA\nvda.exe   -> PE machine = x64
C:\Program Files\NVDA\python313.dll -> x64   (NVDA 2026.1.1, Python 3.13)
C:\Program Files (x86)\NVDA\     -> leftover dir, contains no nvda.exe
```

So **`dtc01_x64.dll` is the binary NVDA needs**, and it is the one already
exercised end-to-end. The `_synthDrivers32` folder inside NVDA is only the
legacy out-of-process bridge for 32-bit SAPI4/SAPI5 COM synths; a Python
synth driver using ctypes runs in NVDA's main x64 process, so it needs an
x64 DLL. The x86 build is retained only as optional legacy support (old
NVDA / 32-bit Windows) and is still unexercised — nothing depends on it.

### Verified inside real NVDA (2026-07-31)

`tools/make_test_addon.py` packages `addon/globalPlugins/dtc01NativeTest.py`
into an installable dev-only addon that boots the emulator in NVDA's own
process a few seconds after startup and announces the result through the
active synth. Installed into NVDA 2026.1.1 (x64, Python 3.13), it reported:

> **"DTC-01 native test passed. 9 times realtime."**

That closes the last real deployment unknown: the DLL loads and runs
in-process under NVDA's Python 3.13 (not just the dev interpreter), the
`synthDrivers.dectalkDtc01.emu.native` import path resolves from inside a
packaged addon, the firmware boots to LED `0xDA`, synthesis produces
audio, host-TX bytes are correct, and unmapped accesses are 0. ~9x
realtime *while NVDA is actively running* (vs ~10x on an idle dev
interpreter) — the small drop is expected contention, and the headroom is
still large.

Both DLLs import **only `KERNEL32.dll`** (static CRT via `/MT`), so no
VC++ redistributable is a deployment prerequisite.

The test addon is throwaway: it announces on *every* NVDA start, ships no
ROMs, and reads them from a hardcoded dev path (`DTC01_ROM_DIR` overrides).
Remove it once the real driver exists.

**Host caveat:** this machine is ARM64 hardware running an x64-emulated
Python, and its MSVC 14.44 has *no arm64 target* installed (only x64/x86
under Hostx64/Hostarm64). A native-ARM64 NVDA would need the ARM64
toolchain added; `native.py`'s arch detection already anticipates an
`arm64` tag.

## 15. Design Voice parameter ranges — corrected from the ROM (2026-07-31)

§6's `[:dv]` table came from an OCR of the manual and is **wrong for
several parameters**. The firmware will report its own values: sending
`[:dv listall]` makes it print each parameter's current value and legal
range over the host link. `tools/dump_voice_defaults.py` queries every
built-in voice this way; the result is baked into
`protocol/commands.py` as `VOICE_PARAM_DEFAULTS`.

| param | §6 (OCR) | ROM says | note |
|---|---|---|---|
| `hs` head size | 75–150 % | **40–200 %** | |
| `ap` average pitch | 50–300 Hz | **30–300 Hz** | |
| `br` breathiness | 0–60 dB | **0–72 dB** | |
| `sm` smoothness | 0–24 dB | **0–100 %** | unit was wrong too |
| `pr` pitch range | 0–250 % | 0–250 % | matches |
| `ri` richness | 0–100 % | 0–100 % | matches |

The ROM also reports `g5` "Loudness (gain of resonator 5)", 0–80 dB — a
real hardware volume control. The driver still does volume as software
gain on the DAC output, which is smoother and doesn't interact with the
voice's timbre, but `g5` is the authentic alternative if that's ever
wanted.

**Per-voice defaults matter more than the ranges.** A voice's default is
often nowhere near the middle of its band:

| voice | `ap` | `pr` | `hs` | `br` | `ri` | `sm` |
|---|---|---|---|---|---|---|
| Perfect Paul | 120 | 100 | 100 | 0 | 80 | 54 |
| Beautiful Betty | 180 | 160 | 100 | 46 | 0 | 44 |
| Huge Harry | **78** | 50 | 120 | 0 | 86 | 34 |
| Frail Frank | 153 | 90 | 90 | 50 | 80 | 36 |
| Kit the Kid | **306** | 180 | 80 | 40 | 40 | 44 |
| Rough Rita | 106 | 80 | 95 | 0* | 49* | 34 |
| Uppity Ursula | 264 | 135 | 95 | 0 | 100 | 64 |

(*Rita: `ri` 0, `br` 49 — the columns above follow the query output.)

This caused a real user-visible bug. The driver originally mapped each
NVDA slider linearly onto the *absolute* range and sent nothing at 50,
so 50 meant "the voice's own default" while 51 meant a value from the
middle of the absolute band. On Huge Harry that is 78 Hz versus ~183 Hz —
a slider step of one produced a huge jump, and moving 45→50→55 went
"high, very low, high again".

Sliders are now **relative to the selected voice**: 50 = that voice's
default, 0 → the parameter's minimum, 100 → its maximum, piecewise linear
through the default, so the midpoint is continuous (Harry: 49→77, 50→78,
51→82). Changing voice resets the sliders to 50, because the same slider
position means different absolute values on different voices.

Two consequences worth knowing:
- Kit's default `ap` (306) is **above** the maximum the ROM reports (300),
  so there is no headroom above 50 on her pitch slider — it stays at 306.
  `voice_param()` widens the bounds to include the default rather than
  clamping the voice's own value.
- Where a default sits at a range end (Paul's breathiness is 0), that half
  of the slider is flat. That is the firmware's own doing, not a mapping
  bug.

## 16. Session 4 (2026-08-01) — optimisation: what worked and what didn't

Question asked: can the core be made to run on slower CPUs? Baseline on this
machine (x64 under Windows-on-ARM emulation, so all figures are through
binary translation) was **~9.6x realtime**.

### PGO: +7–11%, kept

MSVC profile-guided optimisation, via `tools/build_pgo.bat` (instrument →
`tools/pgo_train.py` → optimize). Measured +10.6% on the first build and
+7.2% after the scheduler change, both **bit-identical** output. It compiles
all but 14 of 2305 functions for size, shrinking the DLL 465→368 KB; the win
is almost certainly instruction-cache behaviour in the two dispatch loops.

`tools/make_addon.py` now prefers `build/pgo/dtc01_<arch>.dll` — but **only
when it is newer than every file in `native/`**, because a PGO DLL goes stale
the moment the emulator is edited and the ordinary build is re-run. Shipping
one built from code that no longer exists would look completely normal.
`--require-pgo` enforces PGO for the release architecture. x86 cannot be
PGO'd here: training must run the instrumented DLL in a matching-architecture
process and there is no 32-bit Python on this machine.

### The floating-point scheduler theory: DISPROVED

`dtc01_run_samples` performed a `double` division per emulated 68000
instruction (~1M/sec of audio) plus several double add/multiplies. Estimated
at 10–25%. **Measured at ~1%, inside the noise floor.** Converting the DSP
and DAC to integer counters gained +0.2%; additionally replacing the
remaining division with a reciprocal multiply reached only +1.1%, against a
1.2–1.3% run-to-run spread.

Cost is dominated by `m68k_execute` and `tms_step` dispatch, not by the
scheduler arithmetic — which is consistent with PGO (a code-layout
optimisation) being the thing that does help. **Do not re-attempt this as a
performance measure.** The planned follow-up converting `duart_step` to
fixed-point was abandoned on this evidence: it would have disturbed the
counter/timer that makes speech work at all (§11) for no measurable gain.

### The integer scheduler was kept anyway — as a correctness fix

Both clock ratios are exact integers (DSP = 68000/2, DAC = one sample per
1000 68000 cycles), but the old code accumulated *seconds* in a double. A
boundary exact in rational arithmetic is not exact in binary floating point,
so `tools/test_scheduler_exact.py` (Fraction-based, no emulator needed)
shows the float schedule put **772 of 7803 DAC samples (9.9%) on the wrong
instruction**. The DAC is hardware-clocked at a fixed 10kHz, so
exactly-periodic sampling is the faithful model and the jitter was an
emulation artifact.

Consequence: **18% of audio samples differ** from builds up to 0.5.2.
Underruns (31 vs 32) and DAC tick count (126000) were unchanged, and
`compare_native`, `test_driver_offline` and `sayall_sim` all pass.
`emu/machine.py` mirrors the change so the reference oracle stays faithful.

### Batching `m68k_execute(n)`: +82%, far more than estimated

Estimated at 15–30%. **Measured at +82%** for a 32-cycle batch, and it keeps
paying beyond that. Musashi saves and restores its entire CPU state around
every `m68k_execute()` call, so one instruction per call was spending most of
its time on call overhead rather than emulation.

The batch is capped at `M68K_PER_DAC - dac_debt`, i.e. the cycles remaining
before the next DAC sample. That keeps the 10kHz DAC exactly periodic:
`m68k_execute` meets the budget and then finishes the instruction in
progress, which is the same instruction that would have crossed the boundary
one-at-a-time. What batching actually coarsens is how often the DSP and DUART
are serviced relative to the 68000.

Measured across batch sizes (ordinary builds, interleaved trials, 5.6% noise
floor), against the one-instruction schedule:

| batch | speed | vs base | underruns | rms | peak | envelope |
|---|---|---|---|---|---|---|
| 1 | 9.73x | — | 32 | 1470 | 14304 | 71 |
| 8 | 9.99x | +2.7% | 32 | 1470 | 14304 | 71 |
| 32 | 17.72x | **+82%** | 28 | 1468 | 14304 | 70 |
| 128 | 22.85x | +135% | 29 | 1470 | 14304 | 71 |
| 1000 | 26.40x | +171% | 23 | 1464 | 14064 | 69 |

Batch 8 is bit-identical to batch 1 — most 68000 instructions already cost
≥8 cycles, so the budget rarely covers two. Underruns *improve* with
batching (the DSP runs in larger chunks and keeps the FIFO fuller), and LED,
unmapped accesses and host TX are identical throughout.

`M68K_BATCH_CYCLES` defaults to **32**: the conservative end of the useful
range, leaving the DSP no more than ~3.2µs (3% of a DAC period) behind the
68000. `#define` it at build time to explore. Setting it to 1 reproduces the
historical schedule exactly, which is how the refactor itself was verified —
batch 1 is bit-identical to the pre-batch build.

`compare_native.py` passes, and the exact-match rate against the Python
reference is 42.56% versus 42.57% before batching: the batched core is no
further from the reference than the unbatched one was.

### Still untried
- Native ARM64 build (this host is ARM64; the toolchain target is not
  installed). Irrelevant to low-end x86, which was the actual question.
- `EMULATOR_INSTANCES = 3` costs ~0.9s of startup and 3x memory but buys no
  throughput — Musashi's globals serialise all three behind one lock.

## 17. Session 5 (2026-08-01) — "phrases go missing": instrumented, not yet fixed

Report: whole phrases occasionally never speak, "particularly lines that don't
end with a terminator" — a list line read as just "Borris".

**Not a regression from the §16 batching work.** Measured across the released
0.5.3 DLL and batch 1/32/128, all identical: audio duration and internal gaps
for the reported text, the block at which the driver ends an utterance,
`is_idle` frequency (114/400 blocks) and quick-drain success rate (53%).
`sanitize_text` passes the string through unchanged and `_terminate` guards
empty input. Rolling back would not have helped. Do not re-blame the emulator
without new evidence.

### Two silent-drop paths in the driver

Both in `cancel()`, both previously uncounted and unlogged:

1. **The job queue is drained** — utterances NVDA queued that were never
   spoken are thrown away. Usually correct (the user moved on), but if NVDA
   sends one line as more than one `speak()` call and a cancel lands between
   them, the remainder vanishes. Now counted as `discarded=` in the periodic
   stats line, which is the number to look at first next time.
2. **`_pendingText` is dropped** — the fragment smooth mode holds while
   waiting for a sentence ender. Fits the "no terminator" description, but
   smooth mode is gated on say-all, so it only applies there.

### The trace

`<NVDA config>/dectalkDtc01/trace.flag` (presence only, read once at driver
construction) turns on one INFO line per utterance at four points: what NVDA
handed us, what a cancel discarded, what bytes reached the firmware, and how
much audio actually reached the device. Those four distinguish "NVDA never
sent it" from "we discarded it" from "the firmware produced nothing" from
"audio never got delivered" — which is the fork the report cannot be resolved
without.

A flag file rather than a settings checkbox (the panel is deliberately short)
and rather than NVDA's global debug level, which is noisy enough to perturb
speech timing. **Remove or demote this once the bug is found**: it records
everything the screen reader says, which is a privacy consideration and not
something to leave enabled by default.

### `QUICK_DRAIN_BLOCKS` 40 → 80

Unrelated to the report, but the §16 speedup made it free. The budget is in
emulated blocks (host-independent), but its *wall* cost falls as the core gets
faster: 40 was chosen as "~100ms at ~10x" and at ~19.7x spends only ~51ms.
Measured swap rate on echo-length text cancelled after 4 blocks:

| `QUICK_DRAIN_BLOCKS` | wall | swap rate |
|---|---|---|
| 40 | ~51ms | 53% |
| **80** | ~102ms | **20%** |
| 120 | ~152ms | 0% |

80 restores the wall cost the constant was tuned for. 120 reaches 0% but
exceeds the cancelled-keystroke latency budget already accepted.

**This is not presented as the fix for the missing phrases** — a swap moves to
a *clean* instance and `fallbacks=0` in every stats line, so the all-dirty
path never ran. It reduces churn; it is not known to reduce phrase loss.

## 18. Auto-update never applied (2026-08-01) — install without restart

Symptom on a second machine updating 0.5.2 → 0.5.55: the offer dialog
appeared, the package downloaded, and then nothing. The log ended at
`DTC-01 updater: offering v0.5.55 from ...` with no error.

Cause: `gui.addonGui.installAddon()` installs the bundle but does **not**
offer to restart — that is a separate `promptUserForRestart()`, which NVDA's
own Add-on Store calls after installing (`addonStoreGui/controls/storeDialog`
references both, plus "Add-ons pending install, restart required"). Without
it the add-on sits staged in `<name>.pendingInstall` and nothing applies it or
says so. **Restarting NVDA applies a staged install**, which is the recovery
if this is seen again on an old build.

Two things made it hard to see:

- The success log was written *before* the work: it logged "offering" right
  after `wx.CallAfter` scheduled the call, so it reported success for merely
  scheduling. Outcome logging now happens after the attempt, and distinguishes
  a failure from the user declining NVDA's own confirmation.
- `tools/check_nvda_api.py` did not and **could not** catch this. The gate
  checks that imported symbols exist; `installAddon` exists. This was a wrong
  assumption about what a function *does*. Do not expect that gate to cover
  behaviour — only end-to-end exercise of the update path does, and that path
  had never been run.

Related trap, hit at the same time: that machine was running a private
`--with-roms` build, so updating to a public release removed its bundled
firmware (README documents this). Either keep a dump in
`<NVDA config>/dectalkDtc01/roms/`, which survives updates and outranks the
bundled copy, or re-install a fresh `--with-roms` package.

## 19. THE missing-phrase bug (2026-08-01) — firmware input line limit

**The firmware silently discards an input line past ~134 bytes.** Output does
not truncate proportionally: it collapses to a fixed ~1.74s stub regardless of
how much longer the text is. Measured on a clean machine per case:

| payload bytes | audio |
|---|---|
| 121 | 8.23s |
| 133 | 8.89s |
| **136** | **1.74s** |
| 203 | 1.74s |
| 500 | 1.74s |

Below the limit, duration grows linearly with length exactly as expected. The
cliff sits between 133 and 136 bytes, counting the command prefix and the
terminating `,\r`. `FIRMWARE_LINE_BYTES = 120` leaves margin.

This had been present from the beginning and explains every "phrase went
missing" report: list items and chat messages routinely run 150–250
characters, so most of the content was lost and the stub often fell below the
silence threshold, logging as `0.00s audio fed`.

### The fix

`_speakChunk` splits over-long text with `_splitForFirmware` and speaks the
pieces **one at a time**, each pumped before the next is fed. Splitting alone
is not enough: the firmware's buffer holds only about two lines, so writing
all the pieces in one go merely moves the loss further out (299 and 500
characters both capped at the same 14.4s). Pacing is what say all has always
done between lines, so the cadence was already known-good.

Splits prefer sentence enders, then clause marks, then spaces
(`_SPLIT_PREFERENCE`). A say-all chunk is usually several complete sentences,
so it splits at real full stops and costs nothing prosodically. Only a single
sentence longer than the limit gets a break that was not already there — and
today that sentence produces almost nothing at all.

Verified through the real driver, not a simulation: seconds-per-character is
constant (0.053–0.056) from 47 to 899 characters, versus flat 1.74s past 131
before. `sayall_sim` is unchanged at 5.30s vs 5.35s ideal.

### Method note — three wrong answers before this one

Each was killed by measurement, and each had come from reasoning:

1. **Punctuation** (`"` looked guilty, 5.13s → 3.40s). Artifact: one emulator
   instance reused across cases, so each measurement absorbed the previous
   utterance's tail. With a fresh machine per case, quotes, apostrophes and
   colons make no difference whatever.
2. **Feeding faster than the firmware consumes.** Throttling made it *worse*
   (1.24–2.76s vs 3.44s), which rules out rate.
3. **Missing XON/XOFF.** The firmware sends no XOFF. It does maintain a DUART
   output port that nothing reads (`duart2681.c:279`), which is a real gap but
   not this bug.

A fourth near-miss: simulating the fix with an ad-hoc "finished" test of 0.5s
silence gave nonsense (167 characters scoring lower than 131), because an
utterance can legitimately go quiet for 0.54s mid-way. **Drive the real
SynthDriver.** Its `_pump` already knows what finished means; hand-rolled
equivalents in a test script do not.

## 20. The other half of it — the firmware pauses mid-utterance

Fixing the line-length limit (§19) exposed a second, older bug: speech from
one item surfacing attached to the next one, "moving up the list the previous
line begins with the end of the last line".

**The firmware goes quiet inside an utterance while it works out how to say
something** -- measured at **450ms** before a long number in
`Meddeau: oh that works; ... 20366 of 20366`. Our own buffers are empty during
that pause too, so it is indistinguishable from the end by the test `_pump`
was using: `SILENCE_BLOCKS = 6`, i.e. **150ms**. The utterance was declared
finished mid-way, `_speakLine` returned, and the rest stayed in the firmware
until the next utterance flushed it out.

Measured internal gaps:

| text | gap |
|---|---|
| `Meddeau: oh that works; ... 20366 of 20366` | **450ms** |
| `Meddeau: weird? ... 20364 of 20366` | 150ms (exactly at the threshold) |
| `Noof: I think for some stupid reason ...` | 75ms |
| `Desktop list`, `Paperback 5 of 12` | 0ms |

§19 made it far more visible: over-limit text used to be discarded by the
firmware, so little was left to leak. Once the full text is accepted, an
utterance cut short at 150ms strands seconds of speech.

### Fix, and why it is conditional

`LONG_SILENCE_BLOCKS = 28` (700ms) is used instead of 6 when the text is over
`LONG_TEXT_CHARS` (40) **and say all is not running**, plus on every drain
path -- draining is exactly where leftovers with gaps live.

Both conditions matter:

- Short text has no gaps at all (0ms measured), so key echo keeps the 150ms
  threshold and stays responsive.
- During say all the next chunk continues the same text, so speech left over
  from an early "done" plays before it in the correct order and nothing sounds
  wrong. Applying the long threshold there cost **+325ms at every line break**
  (`sayall_sim` went from ±25ms to +325ms), which is precisely the pause say
  all was tuned to remove. Restricting it to navigation restored ±25ms.

Verified: gappy line 9.97s (was cut at the 450ms gap), a short line following
it 0.85s against 0.85s spoken alone -- no leak; `sayall_sim` back to 5.30s vs
5.35s ideal; length remains linear to 899 characters.

### Signals that turned out not to work

- **`>` prompt.** The firmware does emit one, but at 0.40s for an utterance
  whose audio ran to 15.12s -- it means "line accepted", not "finished".
- **DSP reset.** `dtc01_dsp_active` was added on the theory that the firmware
  parks the DSP between utterances. It does not: the DSP stayed active
  throughout and was still active after the audio ended and `is_idle` was
  true. The accessor is kept because it is cheap and correct about what it
  reports, but it is not an end-of-utterance signal.

## 21. Firmware v1.8 (2026-08-12) — MAME's other BIOS, implemented

MAME's "switch" between firmware versions is not a runtime DIP switch: it is
`ROM_SYSTEM_BIOS` (`mame dectalk -bios v18`), chosen when the machine is
built. Selecting it swaps **both** the 16 main-CPU EPROMs and the DSP PROM
pair. `emu/rom_loader.py` now mirrors that with a `RomSet` record per
version, so the two halves cannot be selected independently.

**No new ROM dumps were needed.** The user's `dectalk.zip` already contained
every v1.8 chip, unused: main CPU `23-031…038` / `23-059…066`, DSP
`23-165/166`. All 18 SHA-1s match MAME's `ROM_BIOS(1)` entries.

### The DSP pairing is real, and now measured

MAME's driver comments say the wrong DSP pair clips with v1.8. Confirmed
here by speaking one sentence through all four combinations on our own core:

| main CPU | DSP pair | peak | rms | clipped samples |
|---|---|---|---|---|
| v1.8 | 165/166 | 12544 | 474 | 0 |
| **v1.8** | **204/205** | **32768** | **10390** | **61** |
| v2.0 | 204/205 | 13536 | 1157 | 0 |
| v2.0 | 165/166 | 9232 | 193 | 0 |

The v1.8 + 204/205 row rails against the 16-bit limit — that is the
"clips/screeches like hell" the MAME driver header describes. The last row
reproduces its other note, that 165/166 works with v2.0 but comes out
quieter (rms 193 vs 1157). Note this corrects §2's parenthetical, which
lumped 165/166 in with 409/410 as "clipping": 165/166 does not clip with
v2.0, it is merely quiet, and it is the *correct* pair for v1.8.

### v1.8's audio was broken — what it was NOT (kept: the ruled-out list)

**This section is the investigation record; the cause is in "RESOLVED" below.**


Listening test, 2026-08-12: every v1.8 clip is broken. It is recognisably
speech — fragments are intelligible, so the synthesis is not producing
noise — but most of the waveform is missing. Measurements agree: **69% of
the samples the v1.8 DSP writes are exactly `0x0000`**, against 20-30% for
v2.0. The output is bursts of 10-60 real samples separated by ~90-sample
(9 ms) runs of pure silence.

This is worth reading before investigating further, because MAME's own TODO
records the same symptom class as an upstream bug they already fixed:

> `<DONE>` Figure out why the older -165/-166 and newer -409/-410 TMS32010
> DSP firmwares don't produce any sound, while the middle -204/-205 one
> does (**fifo implementations were busted**)

So "the 165/166 DSP program is silent while 204/205 works" is a known way
for a *host emulation* to be wrong, not evidence that the ROM pair is bad.
Our port reproduces the shape of that bug. What has been ruled out:

| hypothesis | how it was ruled out |
|---|---|
| DSP stalling / FIFO underrun | DSP writes 31935 samples per 40000 DAC ticks — near full rate, no stall. `outfifo_underruns` is 0. |
| DAC holding a stale sample | gaps are exact `0x0000`; bursts end mid-swing (−1856), so a held value would be audible, not silent |
| 68000 starving the DSP | v1.8 writes **more** to the infifo than v2.0 (9811 vs 5890 words per 2 s) |
| wrong NVRAM image | blank NVRAM vs the v2.0 default moves the silent fraction 68.6% → 69.4%. Not the cause. |
| CPU interleave granularity | the Python core interleaves one 68000 instruction at a time (MAME's exact historical schedule) and shows the same 69% |
| missing/incorrect DSP opcodes | the five opcodes only v1.8 executes (`0x24/25/2a/2b/2e`, `LAC` with shift) are implemented, and `_getdata`'s sign-extend-then-shift matches `tms320c1x.cpp` exactly |
| the DSP idling in a wait loop | during the silent runs it executes **955 distinct PCs** — the full synthesis loop. It is computing silence, not waiting for anything. |
| a native-only bug | the pure-Python oracle and the C core agree to within a percentage point |
| INT-line clear on outfifo write | our "inert no-op" is faithful: MAME's `execute_set_input` ignores `CLEAR_LINE` ("Pending Interrupts cannot be cleared!") |

The infifo/outfifo/semaphore handlers were diffed against
`mame_dectalk.cpp` line by line and match.

### Differential trace against MAME (2026-08-13) — the 68000 half is exonerated

MAME 0.289 was installed locally, which turns this from inference into
measurement. `tools/mame_tap.lua` taps the same two buses inside MAME that
we instrument in `machine.py`; run it as documented at the top of that file
(**with an empty `-nvram_directory`** — a saved nvram changes the boot path
and cost one confusing pair of runs before it was spotted).

**MAME plays v1.8 correctly.** Duty cycle within the speech span, from
`-wavwrite` captures: MAME v2.0 48.6%, MAME **v1.8 51.5%** — no degradation.
Ours: v2.0 82.1%, v1.8 **8.9%**. (The absolute numbers differ between
emulators because MAME resamples to 48 kHz; only the within-emulator
comparison is meaningful.) So this is our defect, not a bad ROM pair and not
an upstream limitation.

**Our 68000 → DSP parameter stream is identical to MAME's**, word for word,
on both firmware versions:

```
v20  ours  6000 0CE7 0104 0E45 014A 0CE4 0FD2 04B0 0000 0047 003C 0032
v20  MAME  6000 0CE7 0104 0E45 014A 0CE4 0FD2 04B0 0000 0047 003C 0032
v18  ours  6000 0CE7 00A0 0F3F 0082 0CE4 0FD2 04B0 0000 0047 003D 0032
v18  MAME  6000 0CE7 00A0 0F3F 0082 0CE4 0FD2 04B0 0000 0047 003D 0032
```

That exonerates the entire 68000 half for both versions — firmware
execution, memory map, DUART, NVRAM, interrupts, and the letter-to-sound and
prosody code that generates these words. **The defect is inside the DSP
subsystem.**

**Where the DSP diverges.** MAME's DSP writes *exactly* 10000 samples/s
while speaking — one per DAC tick, paced by the outfifo INT:

| | write rate while speaking | exact-zero writes | first nonzero write |
|---|---|---|---|
| MAME v2.0 | 10000/s | — | t=0.5181s |
| MAME v1.8 | **10000/s** | **3.2%** | t=0.7340s |
| ours v2.0 | 9993/s | 20–30% | t=0.6755s |
| ours v1.8 | **8898/s** | **69%** | t=0.4949s |

Our v2.0 hits the pacing (9993 ≈ 10000) which is why it sounds right. Our
v1.8 runs 11% short *and* fills 69% of its writes with silence, where MAME
fills 3.2%. Counting only real waveform samples over 12 s: MAME ~34600,
ours ~15900 — MAME produces **2.2× more actual audio**. Our v1.8 also starts
speaking 0.24 s *early* off an identical input stream.

**Verified faithful, so not the cause** (in addition to the table above):
the DSP opcode cycle table is a **0-mismatch** diff against MAME's
`s_opcode_main[256]`; `add_branch_cycle()` returns the instruction's base
cycles (what we do); the interrupt costs 3 cycles (what we do); `BIOZ`
polarity matches; `execute_set_input` ignoring `CLEAR_LINE` matches.

### RESOLVED (2026-08-13) — the DAC tick was delivered late

**The scheduler let the DSP run past the DAC tick before the tick was
delivered.** The tick is what asserts the DSP's outfifo interrupt, and v1.8's
DSP program cannot tolerate a late one.

Found by bus bisection rather than the planned instruction lockstep — the
taps in `tools/mame_tap.lua` narrowed it in four steps:

1. `dspread` — the words our DSP pulls out of the infifo diverge from MAME's
   at read 44 (v2.0: identical for all 16307 reads, which validated the
   harness).
2. `in` — the **68000's own write stream** diverges at word 62. This
   overturned the previous session's "68000 exonerated" claim, which had
   compared only the first 12 words. Ours re-sends frame 1 (`6000 0CE7…`)
   where MAME sends a new frame (`4000 015C…`).
3. `spcflags` — ours reads `0x00E0` 104 times; MAME never does. Bit 5 is the
   **DSP soft-error latch**. The firmware's error path is what re-sends
   frames.
4. `dsp0` — MAME's v1.8 DSP raises that error bit **once**; ours **104
   times**, all from DSP PC `0x68E`.

`0x68E` sits at the end of a timeout loop. The DSP arms a counter of 20,
then spins `EINT / LAC 7D / DINT / BZ` — a one-instruction interrupt window
per iteration. The ISR (vector `002: br 694`) writes the sample, zeroes
`mem[7D]`, and critically executes `ZAC`, so **ACC=0 on return is the signal
that the tick arrived** and the `BZ` at `0x67F` is the normal exit. Twenty
missed windows falls through to `OUT mem[10]` → error bit.

The margin is thin: the DSP reaches that loop ~354 cycles into a 500-cycle
sample period, leaving ~146 cycles of slack. Our loop ran the DSP for a
whole 68000 instruction *before* processing the DAC tick, so the interrupt
could land late enough to blow the window. In the native core, capping
`budget` at the tick boundary was not enough — `m68k_execute()` finishes the
instruction in progress, so `cycles` overshoots anyway.

**Fix:** hand the executed cycles to the DSP in chunks that stop at each DAC
boundary (`emu/machine.py` `run_seconds`, `native/dtc01.c` `dtc01_run_samples`).
This is our cycle arithmetic expressing what MAME gets from
`config.m_perfect_cpu_quantum = subtag("dsp")`.

| v1.8 | before | after | MAME |
|---|---|---|---|
| soft errors (2 s) | 104 | **0** | 1 |
| DSP writes while speaking | 8898/s | **10000/s** | 10000/s |
| exact-zero samples | 69% | **4.9%** | 3.2% |
| nonzero audio samples | 9% | **98%** | — |

**v2.0 is bit-identical either way** — its DSP uses a laxer wait — so this
carries no regression risk for the shipping path; realtime factor 15.6x vs
15.9x before, inside noise.

Also fixed alongside: the EINT guard constant in both TMS32010 cores was
`0x7F02`; the real `EINT` encoding is **`0x7F82`** (MAME:
`m_opcode.w.l != 0x7f82`), so "don't service an interrupt right after EINT"
never fired. It made no difference to this bug — the interrupt is serviced
one instruction later either way, before the `DINT` — but it was wrong.

**Note for future MAME work:** `-debugger none` initialises the debugger but
never pumps its command queue, so `-debugscript` silently does nothing —
`trace` is not available headless. And reading `cpu.state[...]` from inside a
Lua memory tap segfaults MAME. The tap-based approach in `tools/mame_tap.lua`
is the one that works.

### The NVRAM worry did not materialise

MAME's ROM_START carries `// NOTE: this nvram image is ONLY VALID for v2.0;
v1.8 expects a different image`, and our `DEFAULT_NVRAM_*` tables in
`native/dtc01.c` were decoded from v2.0's own embedded copy at `0x1A7AE`.
That copy is **not** at `0x1A7AE` in the v1.8 image, and a scan of the v1.8
image for a structurally similar sparse 0x80-byte nibble block found no
candidate — so the expectation was an `NVR FAULT` dead-end.

It did not happen. With the v2.0 default image, v1.8 reaches the same LED
state (`0xda`), emits the same `>` host prompt, and drives the synthesiser.
Booting it with a **blank** NVRAM instead changes the silent-sample
fraction from 68.6% to 69.4% — so the foreign NVRAM image is not what
breaks v1.8's audio either. Finding v1.8's own copy remains open as a
correctness question, but it is not the bug.

### Gate

`$DTC01_ROM_VERSION` (`v20` default; accepts `v18`, `1.8`, `18`, …).
Resolved inside `rom_loader`, so every tool — `build_rom_images.py`,
`disasm68k.py`, `smoke_test_dsp.py` — picks it up with no code change; that
is what makes the v1.8 image disassemblable for the NVRAM hunt above.
Deliberately not a settings-panel entry: everything above the emulator was
calibrated against v2.0 — the NVRAM defaults, `BOOT_ANNOUNCE_WAIT_BLOCKS`,
and `FIRMWARE_LINE_BYTES` (§19's ~134-byte input cliff). Those need
re-measuring on v1.8 before it is a choice a user should be offered.
`findRomDir()` validates only the selected version, so a dump holding just
one firmware is still a valid dump.

## 22. "It stopped after 0.5." (2026-08-13) — a bad split and a stuck DAC

Reported from real use on v1.8: NVDA's add-on dialog was spoken as
*"Add-on Installation dialog You are about to install version 0.5."* and
then nothing. **Two independent bugs**, and only their combination is
audible as a truncation.

### Bug 1 — the line splitter cut inside the number

`_splitForFirmware` picked the rightmost `.` in its window, and in
`version 0.5.59` that is the period between `5` and `59`. The first piece
came out as `...install version 0.5.` — byte-for-byte what the user heard.
Firmware-independent: v2.0 mangles it too (`"zero point five"` /
`"fifty-nine of..."`), it just does not stall afterwards, so it reads as
awkward rather than broken. Same trap for `1,234`, `U.S.A.`, `3.14`.

**Fix:** a mark only ends a piece when whitespace (or end of text) follows.

### Bug 2 — a held DAC sample read as speech forever

The real failure. With that piece, v1.8's pump ran to its 12000-block
ceiling: **298.93s of audio for a 66-byte line** (v2.0: 4.53s). The probe:

```
v18 pump#1: status=exhausted emitted=11957 speech=11954 idle(T/F)=11849/151
```

Speech on 11954 of 11957 blocks *while reporting idle*. Sampling the output
shows what it actually was:

```
peak=128  distinct values=1   idle=True     (still true at 40s)
```

A **constant DC level of 128**. `dsp_pop_outfifo` holds the last sample when
the fifo drains — correct hardware behaviour — and v1.8 parked on 128, which
is just over `SILENCE_THRESHOLD` (120). So `isSpeech` was true forever,
`quiet` never accumulated, and the loop only ended at `maxBlocks`. Had the
held value been 119 nothing would have happened. v2.0 parks at 0.

The trigger is narrow and **rate-dependent**, which is why the first
isolation attempts missed it:

| payload | outcome |
|---|---|
| `…version 0.5.` at `[:ra 235]` (the driver's rate at speed 50) | never goes quiet |
| `…version 0.5.` at `[:ra 350]` | normal, 4.7s |
| same text, trailing dot removed, `[:ra 235]` | normal, 1.9s |
| `The version is 0.5.` / `Pi is 3.14.` at `[:ra 235]` | normal — short text does not reach it |

**Fix:** a block whose samples are all one value is not speech, whatever its
level (`NativeMachine.is_flat`, applied at both detection sites in `_pump`).
A constant level carries no audio and is inaudible through a speaker; only a
peak detector was fooled. With the guard, the same forced-bad-split payload
ends in 271 blocks (6.8s) instead of 12000 (300s).

Both fixes are kept: Bug 1 removes the trigger, Bug 2 removes the failure
mode. Either alone would have hidden this particular report.

**Why it surfaced now:** nothing here is new — the held-sample behaviour and
the splitter predate v1.8 support. Making v1.8 selectable in the settings
panel is what made it reachable, and it is the same lesson as §21: the
constants above the emulator (`SILENCE_THRESHOLD`, `FIRMWARE_LINE_BYTES`,
the boot windows) are all v2.0 measurements.

## 8. Open follow-ups (not yet resolved — do not assume)

- **v1.8's factory NVRAM image** — location unknown (§21). Not at `0x1A7AE`
  as in v2.0. Needs the same treatment v2.0 got: disassemble the v1.8
  NVRAM-recall routine and follow it to its table.
- **v1.8 timing constants** — the §19 input-line limit and the boot
  announcement window are v2.0 measurements, unverified on v1.8.
- ~~Built-in voice table~~ **RESOLVED 2026-07-28**: see section 6b above.
- ~~DSP ROM byte-interleave order~~ **RESOLVED 2026-07-28**: assembled
  image's first two words are `F900 00E1`, which is exactly TMS32010's
  unconditional `B` (branch) opcode with target `0x0E1` — a textbook-sane
  reset handler. Confirms `e70`=high byte, `e69`=low byte, same convention
  as the main CPU ROMs. Main CPU image also validated: reset vector
  SSP=`0x0008c000` (in RAM), PC=`0x000001fe` (in ROM) — both sane.
  See `tools/build_rom_images.py`.
- Exact built-in voice mnemonic table (Table 5-2) — need to fetch/OCR.
- `ef`/`f4`/`f5` unit and coupling ambiguity from OCR noise (§6) — verify
  against ROM disassembly behavior once the emulator can run, or find a
  cleaner scan of the manual.
- SCN2681 DUART: only channel B (host RS-232) is required functionally;
  channel A and the self-test-heavy DUART IP/OP pins can be minimally
  stubbed as long as POST self-tests are bypassed (dipswitch "Skip Self
  Test (IP4)" — driver already documents this path).
- NVRAM persistence across NVDA restarts — nice-to-have, not required for
  v1; can always cold-boot from the ROM-embedded default NVRAM image
  (`ROM_REGION(0x100,"nvram")`, decoded in the driver source).
