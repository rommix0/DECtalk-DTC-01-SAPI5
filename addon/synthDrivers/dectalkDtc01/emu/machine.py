"""Top-level DTC-01 machine glue: wires our hand-written M68000 core, our
TMS32010 core, our SCN2681 DUART, and the FIFO/latch logic between the two
CPUs into one runnable system, replicating src/mame/dec/dectalk.cpp's
dectalk_state exactly (see DESIGN.md sections 1, 3, 4, 5).

The M68000 core is our own (emu/m68000.py + m68000_ops.py) rather than a
third-party emulator -- see DESIGN.md for why: Unicorn's M68K backend can't
auto-vector internally-generated CPU exceptions the DTC-01's mandatory boot
self-test relies on (documented gap, unicorn-engine/unicorn#1502), and
machine68k (a direct Musashi binding) has a broken Windows build script.
Our core's exception handling (see m68000.py's _vector/_raise_bus_error) is
exactly what was missing, so external IRQ delivery below is now just a
matter of calling cpu.service_interrupt(level) -- no more hand-rolled stack
frame construction at this layer.

Timing note (v1): the 68000 and TMS32010 are interleaved by a simple
virtual-time scheduler (run each up to its allotted share of a shared
"elapsed seconds" clock, gated by the 10kHz DAC tick) rather than being
cycle-exact against real silicon. The firmware is interrupt/poll driven
and not expected to be sensitive to sub-instruction timing, but this is a
deliberate simplification worth revisiting if audio artifacts show up.
"""

from __future__ import annotations

from typing import Callable

from . import rom_loader
from .tms32010 import TMS32010
from .duart2681 import SCN2681
from .m68000_ops import M68000
from .m68000 import BusError

ROM_BASE = 0x000000
ROM_SIZE = 0x040000
RAM_BASE = 0x080000
RAM_SIZE = 0x014000
LED_NVRAM_BASE = 0x094000
LED_NVRAM_REGION_SIZE = 0x400
DUART_BASE = 0x098000
DUART_REGION_SIZE = 0x020
SPC_TLC_BASE = 0x09C000
SPC_TLC_REGION_SIZE = 0x008

# Factory-default X2212 NVRAM image, transcribed from the ROM_FILL
# directives in dectalk.cpp's ROM_START(dectalk) (v2.0-specific -- see
# DESIGN.md). A blank/zeroed NVRAM appears to route the firmware into an
# "NVR FAULT" setup-mode dead-end rather than normal operation, so we boot
# from this factory image every time (persistence across runs isn't
# implemented -- DESIGN.md section 8).
_DEFAULT_NVRAM = bytearray(0x100)
for _off, _val in (
	(0x00, 0x05), (0x04, 0x00), (0x08, 0x06), (0x0C, 0x01),
	(0x10, 0x06), (0x14, 0x0B), (0x18, 0x02), (0x1C, 0x02),
	(0x20, 0x01), (0x24, 0x01), (0x28, 0x00), (0x2C, 0x01),
	(0xFC, 0x0D), (0xFD, 0x02), (0xFE, 0x05), (0xFF, 0x0B),
):
	_DEFAULT_NVRAM[_off] = _val

IRQ_TLC = 4
IRQ_SPC = 5
IRQ_DUART = 6

INFIFO_DEPTH = 32
OUTFIFO_DEPTH = 16

DAC_SAMPLE_HZ = 10000
M68K_HZ = 10_000_000
DSP_CYCLE_HZ = 20_000_000 // 4  # matches MAME's execute_clocks_to_cycles (clocks+3)//4

# Both ratios are exact integers, so the DSP and DAC schedules are counted in
# 68000 cycles rather than accumulated as seconds in a float. Accumulating
# seconds put roughly 10% of DAC samples one instruction early or late,
# because a boundary that is exact in rational arithmetic is not exact in
# binary floating point (tools/test_scheduler_exact.py demonstrates this).
M68K_PER_DSP = M68K_HZ // DSP_CYCLE_HZ    # 2
M68K_PER_DAC = M68K_HZ // DAC_SAMPLE_HZ   # 1000


class SystemBus:
	"""Implements the m68000.Bus protocol against the DTC-01 memory map
	(DESIGN.md section 3). ROM/RAM are flat bytearrays; the MMIO regions
	dispatch into DectalkMachine's peripheral handlers."""

	def __init__(self, machine: "DectalkMachine", rom: bytes):
		self.machine = machine
		self.rom = bytearray(rom)
		self.ram = bytearray(RAM_SIZE)

	def _region(self, addr: int):
		if ROM_BASE <= addr < ROM_BASE + ROM_SIZE:
			return "rom", addr - ROM_BASE
		if RAM_BASE <= addr < RAM_BASE + RAM_SIZE:
			return "ram", addr - RAM_BASE
		if LED_NVRAM_BASE <= addr < LED_NVRAM_BASE + LED_NVRAM_REGION_SIZE:
			return "led_nvram", addr - LED_NVRAM_BASE
		if DUART_BASE <= addr < DUART_BASE + DUART_REGION_SIZE:
			return "duart", addr - DUART_BASE
		if SPC_TLC_BASE <= addr < SPC_TLC_BASE + SPC_TLC_REGION_SIZE:
			return "spc_tlc", addr - SPC_TLC_BASE
		return None, None

	# read8/write8/read16/write16 fast-path ROM and RAM directly (by far
	# the two hottest regions -- every instruction fetch and most data
	# access) before falling back to _region()'s full dispatch for the
	# less-hot MMIO regions. Semantics/fallback behavior (including
	# BusError for undefined addresses) are unchanged from routing
	# everything through _region().
	def read8(self, addr: int) -> int:
		if addr < ROM_SIZE:
			return self.rom[addr]
		off = addr - RAM_BASE
		if 0 <= off < RAM_SIZE:
			return self.ram[off]
		region, off = self._region(addr)
		if region == "led_nvram":
			return self.machine.led_nvram_read8(off, addr)
		if region == "duart":
			return self.machine.duart.read(off)
		if region == "spc_tlc":
			# byte access into a word-only register block: return the byte
			# half of the 16-bit value the real bus cycle would carry.
			word = self.machine.spc_tlc_read16(off & ~1)
			return (word >> 8) & 0xFF if (off & 1) == 0 else word & 0xFF
		raise BusError(addr, write=False)

	def write8(self, addr: int, value: int) -> None:
		if addr < ROM_SIZE:
			return  # real ROM is read-only; ignore stray writes rather than corrupt the image
		off = addr - RAM_BASE
		if 0 <= off < RAM_SIZE:
			self.ram[off] = value & 0xFF
			return
		region, off = self._region(addr)
		if region == "led_nvram":
			self.machine.led_nvram_write8(off, addr, value)
			return
		if region == "duart":
			self.machine.duart.write(off, value)
			return
		if region == "spc_tlc":
			return  # SPC/TLC regs are word-only on real hardware; ignore stray byte writes
		raise BusError(addr, write=True)

	def read16(self, addr: int) -> int:
		if addr < ROM_SIZE:
			rom = self.rom
			return (rom[addr] << 8) | rom[addr + 1]
		off = addr - RAM_BASE
		if 0 <= off < RAM_SIZE:
			ram = self.ram
			return (ram[off] << 8) | ram[off + 1]
		region, off = self._region(addr)
		if region == "spc_tlc":
			return self.machine.spc_tlc_read16(off)
		return (self.read8(addr) << 8) | self.read8(addr + 1)

	def write16(self, addr: int, value: int) -> None:
		if addr < ROM_SIZE:
			return
		off = addr - RAM_BASE
		if 0 <= off < RAM_SIZE:
			self.ram[off] = (value >> 8) & 0xFF
			self.ram[off + 1] = value & 0xFF
			return
		region, off = self._region(addr)
		if region == "spc_tlc":
			self.machine.spc_tlc_write16(off, value)
			return
		self.write8(addr, (value >> 8) & 0xFF)
		self.write8(addr + 1, value & 0xFF)

	def read32(self, addr: int) -> int:
		return (self.read16(addr) << 16) | self.read16(addr + 2)

	def write32(self, addr: int, value: int) -> None:
		self.write16(addr, (value >> 16) & 0xFFFF)
		self.write16(addr + 2, value & 0xFFFF)


class DectalkMachine:
	def __init__(self, rom_dir: str, on_audio_sample: Callable[[int], None], on_host_tx: Callable[[int], None],
	             rom_version: str | None = None):
		"""on_audio_sample(sample) is called once per 10kHz DAC tick with the
		12-bit-ish DAC word (already through the '((data&0xfff0)^0x8000)'
		transform -- see DESIGN.md section 5). on_host_tx(byte) is called
		for every byte the firmware transmits back on the host serial link
		(channel B) -- present for completeness/future protocol needs, even
		though this firmware has no [:index] to report (DESIGN.md section 6)."""
		self.rom_version = rom_loader.resolve_version(rom_version)
		main_image, dsp_words = rom_loader.build_images(rom_dir, self.rom_version)

		self._on_audio_sample = on_audio_sample
		self._on_host_tx = on_host_tx

		# -- FIFOs and latches, mirroring dectalk_state's members exactly --
		self.infifo = [0] * INFIFO_DEPTH
		self.infifo_count = 0
		self.infifo_head = 0
		self.infifo_tail = 0
		self.outfifo = [0] * OUTFIFO_DEPTH
		self.outfifo_count = 0
		self.outfifo_head = 0
		self.outfifo_tail = 0
		self.infifo_semaphore = False
		self.spc_error_latch = False
		self.spc_flags_latch = 0  # bit0: speech-init, bit6: spc-irq-enable
		self.tlc_flags_latch = 0
		# NB: _DEFAULT_NVRAM was decoded from the v2.0 ROM's own embedded copy.
		# MAME's driver notes say v1.8 expects a different image; v1.8 boots
		# and speaks correctly with this one anyway, and the NVRAM was ruled
		# out as the cause of v1.8's old audio bug (DESIGN.md §21). Whether
		# the config it decodes to is *right* for v1.8 is still unverified.
		self.nvram = bytearray(_DEFAULT_NVRAM)
		self.led_state = 0
		self._simulate_outfifo_error = False

		self._irq_lines = {IRQ_TLC: False, IRQ_SPC: False, IRQ_DUART: False}
		self._pending_irq = 0

		# -- DSP core --------------------------------------------------------
		self.dsp = TMS32010(dsp_words, self._dsp_io_read, self._dsp_io_write, self._dsp_bio_read)

		# -- DUART -------------------------------------------------------------
		self.duart = SCN2681(on_tx_b=self._duart_tx_b, irq_cb=lambda active: self._set_irq(IRQ_DUART, active))
		# Always-connected virtual link: assert CTS/DSR so the firmware's
		# modem-style connect state machine passes straight through to its
		# "moving data" state instead of waiting for a real modem handshake
		# (see DESIGN.md; LED state-machine bits documented in the driver
		# source's comment block, states 0/3/5 wait on these lines).
		self.duart.set_input_bit(0, True)  # CTS
		self.duart.set_input_bit(2, True)  # DSR

		# -- M68000 ------------------------------------------------------------
		self.bus = SystemBus(self, main_image)
		self.cpu = M68000(self.bus)
		self.cpu.reset()

		self._time_seconds = 0.0
		self._dsp_half_debt = 0   # unconverted 68000 cycles owed to the DSP
		self._dac_debt = 0        # 68000 cycles since the last DAC sample

	# =====================================================================
	# Reset
	# =====================================================================
	def reset(self) -> None:
		self.cpu.reset()
		self.spc_flags_latch = 1  # speech reset active, spc int disabled
		self.tlc_flags_latch = 0
		self.duart.reset()
		self.duart.set_input_bit(0, True)
		self.duart.set_input_bit(2, True)
		self._clear_all_fifos()
		self._dsp_semaphore_w(False)
		self.spc_error_latch = False
		self.dsp.set_reset_line(True)

	# =====================================================================
	# FIFO / SPC glue -- ported 1:1 from dectalk_state (DESIGN.md section 4)
	# =====================================================================
	def _outfifo_check(self) -> None:
		# Matches the real driver's outfifo_check(): only called from the
		# 10kHz sample-pop path (dsp_outfifo_r equivalent), NOT from every
		# DSP fifo write -- see the driver's own comment "outfifo check
		# should only be done in the audio 10khz polling function". Because
		# our TMS32010.set_int_line only does anything on assert (pending
		# interrupts can't be externally cleared -- matches MAME's generic
		# execute_set_input), calling it with False here is an intentional
		# no-op kept for readability/fidelity.
		self.dsp.set_int_line(self.outfifo_count < OUTFIFO_DEPTH)

	def _clear_all_fifos(self) -> None:
		self.outfifo = [0] * OUTFIFO_DEPTH
		self.outfifo_count = 0
		self.outfifo_head = self.outfifo_tail = 0
		self.infifo = [0] * INFIFO_DEPTH
		self.infifo_count = 0
		self.infifo_head = self.infifo_tail = 0
		self._outfifo_check()

	def _dsp_semaphore_w(self, state: bool) -> None:
		self.infifo_semaphore = state
		fire = state and bool(self.spc_flags_latch & 0x40)
		self._set_irq(IRQ_SPC, fire)

	# -- 68000 side -----------------------------------------------------
	def m68k_infifo_w(self, data: int) -> None:
		if self.infifo_count == INFIFO_DEPTH:
			return
		self.infifo[self.infifo_head] = data & 0xFFFF
		self.infifo_head = (self.infifo_head + 1) & (INFIFO_DEPTH - 1)
		self.infifo_count += 1

	def m68k_spcflags_r(self) -> int:
		data = self.spc_flags_latch
		data |= 0x20 if self.spc_error_latch else 0
		data |= 0x80 if self.infifo_semaphore else 0
		return data

	def m68k_spcflags_w(self, data: int) -> None:
		self.spc_flags_latch = data & 0x41
		if data & 0x01:
			self._clear_all_fifos()
			self.dsp.set_reset_line(True)
			self.spc_error_latch = False
			self._dsp_semaphore_w(False)
		else:
			self.dsp.set_reset_line(False)
		if data & 0x02:
			self.spc_error_latch = False
			self._dsp_semaphore_w(False)
		if data & 0x40:
			if self.infifo_semaphore:
				self._set_irq(IRQ_SPC, True)
		else:
			self._set_irq(IRQ_SPC, False)

	def m68k_tlcflags_r(self) -> int:
		return self.tlc_flags_latch  # tone/ring detect unimplemented -- always idle

	def m68k_tlcflags_w(self, data: int) -> None:
		self.tlc_flags_latch = data & 0x4140
		self._set_irq(IRQ_TLC, False)  # telephone hardware not modeled -- never fires

	# -- DSP side ---------------------------------------------------------
	def _dsp_io_write(self, port: int, value: int) -> None:
		if port == 0:
			self._dsp_semaphore_w(not self._simulate_outfifo_error)
			self.spc_error_latch = bool(value & 1)
		elif port == 1:
			self.dsp.set_int_line(False)  # inert no-op, kept for fidelity -- see _outfifo_check
			if self.outfifo_count != OUTFIFO_DEPTH:
				self.outfifo[self.outfifo_head] = value & 0xFFFF
				self.outfifo_head = (self.outfifo_head + 1) & (OUTFIFO_DEPTH - 1)
				self.outfifo_count += 1

	def _dsp_io_read(self, port: int) -> int:
		if port == 1:
			data = self.infifo[self.infifo_tail]
			if self.infifo_count > 0:
				self.infifo_tail = (self.infifo_tail + 1) & (INFIFO_DEPTH - 1)
				self.infifo_count -= 1
			return data
		return 0xFFFF

	def _dsp_bio_read(self) -> int:
		return 1 if self.infifo_semaphore else 0

	def _dsp_pop_outfifo(self) -> int:
		data = self.outfifo[self.outfifo_tail]
		if self.outfifo_count > 0:
			self.outfifo_tail = (self.outfifo_tail + 1) & (OUTFIFO_DEPTH - 1)
			self.outfifo_count -= 1
		self._outfifo_check()
		return ((data & 0xFFF0) ^ 0x8000) & 0xFFFF

	# =====================================================================
	# MMIO region handlers (called from SystemBus)
	# =====================================================================
	def led_nvram_read8(self, off: int, addr: int) -> int:
		if off < 0x200 and not (addr & 1):
			return self.nvram[(off // 2) & 0xFF]
		return 0xFF

	def led_nvram_write8(self, off: int, addr: int, value: int) -> None:
		if off < 0x200:
			if addr & 1:
				self.led_state = value & 0xFF
			else:
				self.nvram[(off // 2) & 0xFF] = value & 0xFF
		# else: NVRAM recall/store trigger region -- persistence not
		# modeled (DESIGN.md section 8), buffer is already current.

	def spc_tlc_read16(self, off: int) -> int:
		local = off & 0x7
		if local == 0:
			return self.m68k_spcflags_r()
		if local == 4:
			return self.m68k_tlcflags_r()
		if local == 6:
			return 0
		return 0xFFFF

	def spc_tlc_write16(self, off: int, value: int) -> None:
		local = off & 0x7
		if local == 0:
			self.m68k_spcflags_w(value)
		elif local == 2:
			self.m68k_infifo_w(value)
		elif local == 4:
			self.m68k_tlcflags_w(value)

	def _duart_tx_b(self, byte: int) -> None:
		self._on_host_tx(byte)

	# =====================================================================
	# Interrupts
	# =====================================================================
	def _set_irq(self, level: int, asserted: bool) -> None:
		# _pending_irq_level() is called once per 68000 instruction (the
		# hot path), so the active-level is maintained incrementally here
		# rather than rescanning/max()-ing all IRQ lines every single call;
		# IRQ lines change far less often than instructions execute.
		self._irq_lines[level] = asserted
		if asserted:
			if level > self._pending_irq:
				self._pending_irq = level
		elif level == self._pending_irq:
			active = [lvl for lvl, on in self._irq_lines.items() if on]
			self._pending_irq = max(active) if active else 0

	def _pending_irq_level(self) -> int:
		return self._pending_irq

	def _maybe_service_interrupt(self) -> None:
		level = self._pending_irq  # inlined _pending_irq_level(): hot path, called every 68000 instruction
		if level == 0:
			return
		mask = (self.cpu.SR >> 8) & 0x7
		if level <= mask:
			return
		self.cpu.stopped = False  # an asserted interrupt wakes a STOPped CPU
		self.cpu.service_interrupt(level)

	# =====================================================================
	# Execution
	# =====================================================================
	def run_seconds(self, seconds: float) -> None:
		"""Advance the whole machine by `seconds` of wall-clock-equivalent
		time: interleave the 68000, the DSP, and the 10kHz DAC tick. See
		module docstring for the timing-model caveat."""
		end_time = self._time_seconds + seconds
		while self._time_seconds < end_time:
			self._maybe_service_interrupt()
			cycles = self.cpu.step()
			self._time_seconds += cycles / M68K_HZ

			self.duart.step(cycles / M68K_HZ)

			# Split the 68000 instruction's cycles at DAC-tick boundaries so
			# the DSP never runs *past* a tick before the tick is delivered.
			# The tick is what asserts the DSP's outfifo interrupt, and v1.8's
			# DSP program times out if that interrupt is late: it waits for it
			# in a 20-iteration loop (~180 cycles) entered ~354 cycles into a
			# 500-cycle sample period, so barely ~146 cycles of slack. Running
			# the DSP for a whole 68000 instruction first could overshoot that
			# and make the DSP declare a soft error, which puts the firmware
			# on its resend path and shreds the audio (DESIGN.md §21). MAME
			# avoids this with `config.m_perfect_cpu_quantum = subtag("dsp")`;
			# this is the same guarantee expressed in our cycle arithmetic.
			# v2.0's DSP has a laxer wait and is bit-identical either way.
			remaining = cycles
			while remaining > 0:
				chunk = remaining
				until_tick = M68K_PER_DAC - self._dac_debt
				if until_tick < chunk:
					chunk = until_tick

				self._dsp_half_debt += chunk
				if not self.dsp.in_reset:
					spent = self._dsp_half_debt // M68K_PER_DSP
					if spent > 0:
						overshoot = self.dsp.run(spent)  # <= 0
						self._dsp_half_debt = overshoot * M68K_PER_DSP

				self._dac_debt += chunk
				remaining -= chunk
				if self._dac_debt >= M68K_PER_DAC:
					self._dac_debt -= M68K_PER_DAC
					sample = self._dsp_pop_outfifo()
					self._on_audio_sample(sample)
