"""TMS32010 DSP core.

A direct, instruction-for-instruction port of MAME's BSD-3-Clause
"tms320c1x" device (src/devices/cpu/tms320c1x/tms320c1x.cpp, Tony La
Porta), configured as a TMS320C10 (12-bit program address space). Unicorn
Engine has no TMS32010 backend, so this hand-written core exists to run the
DTC-01's real DSP firmware, which performs the actual Klatt speech
synthesis math -- see DESIGN.md section 1.

Every opcode function below mirrors its MAME counterpart line-for-line,
including quirks that look like bugs (e.g. TBLR/TBLW's STACK[0]=STACK[1]
shuffle, SST never updating ARP, interrupts that can be set but never
externally cleared). These are preserved intentionally: the real DSP
firmware was written and tested against this exact behavior, and "fixing"
any of it would silently diverge from what the ROM expects.
"""

from __future__ import annotations

from typing import Callable

MASK16 = 0xFFFF
MASK32 = 0xFFFFFFFF

OV_FLAG = 0x8000
OVM_FLAG = 0x4000
INTM_FLAG = 0x2000
ARP_REG = 0x0100
DP_REG = 0x0001
STR_RESERVED = 0x1EFE  # reserved status bits that always read back as 1


def s16(v: int) -> int:
	v &= MASK16
	return v - 0x10000 if v & 0x8000 else v


def s32(v: int) -> int:
	v &= MASK32
	return v - 0x100000000 if v & 0x80000000 else v


class TMS32010:
	ADDR_MASK = 0xFFF  # 12-bit program address space (TMS320C10)
	PROGRAM_SIZE = 0x1000
	DATA_SIZE = 0x100

	__slots__ = (
		"program", "data", "_io_read", "_io_write", "_bio_read",
		"PC", "PREVPC", "STR", "ACC", "ALU", "Preg", "Treg", "AR", "STACK",
		"opcode", "oldacc", "memaccess", "int_pending", "in_reset",
		"_branch_taken", "_dispatch_main", "_dispatch_7f",
	)

	def __init__(
		self,
		program_words: list[int],
		io_read: Callable[[int], int],
		io_write: Callable[[int, int], None],
		bio_read: Callable[[], int],
	):
		self.program = [0] * self.PROGRAM_SIZE
		for i, w in enumerate(program_words[: self.PROGRAM_SIZE]):
			self.program[i] = w & MASK16
		self.data = [0] * self.DATA_SIZE

		self._io_read = io_read
		self._io_write = io_write
		self._bio_read = bio_read

		self.PC = 0
		self.PREVPC = 0
		self.STR = 0
		self.ACC = 0
		self.ALU = 0
		self.Preg = 0
		self.Treg = 0
		self.AR = [0, 0]
		self.STACK = [0, 0, 0, 0]
		self.opcode = 0
		self.oldacc = 0
		self.memaccess = 0
		self.int_pending = False
		self.in_reset = True
		self._branch_taken = False

		self._dispatch_main = self._build_main_table()
		self._dispatch_7f = self._build_7f_table()

		self.reset()

	# -- status register helpers, exact port of CLR/SET_FLAG -------------
	def _clr(self, flag: int) -> None:
		self.STR = ((self.STR & ~flag) | STR_RESERVED) & MASK16

	def _set_flag(self, flag: int) -> None:
		self.STR = ((self.STR | flag) | STR_RESERVED) & MASK16

	@property
	def OV(self) -> int:
		return self.STR & OV_FLAG

	@property
	def OVM(self) -> int:
		return self.STR & OVM_FLAG

	@property
	def INTM(self) -> int:
		return self.STR & INTM_FLAG

	@property
	def ARP(self) -> int:
		return (self.STR & ARP_REG) >> 8

	@property
	def DP(self) -> int:
		return (self.STR & DP_REG) << 7

	# -- lifecycle ---------------------------------------------------------
	def reset(self) -> None:
		self.PC = 0
		self.ACC = 0
		self.int_pending = False
		self._clr(OV_FLAG | ARP_REG | DP_REG)
		self._set_flag(OVM_FLAG | INTM_FLAG)  # net result: STR == 0x7efe

	def set_reset_line(self, asserted: bool) -> None:
		"""Mirrors the DTC-01 glue logic driving the DSP's CLR line from the
		68000 SPC-flags register (see DESIGN.md section 3/4): held in reset
		clamps PC/ACC and blocks execution; release re-arms from PC=0."""
		if asserted:
			self.in_reset = True
			self.reset()
		else:
			self.in_reset = False

	def set_int_line(self, asserted: bool) -> None:
		"""Pending interrupts cannot be cleared externally -- only Ext_IRQ,
		by actually servicing one, clears int_pending. This matches MAME's
		execute_set_input, which only ever ORs the pending flag in."""
		if asserted:
			self.int_pending = True

	# -- memory helpers, exact port of getdata/putdata/putdata_* ---------
	def _dma_dp(self) -> int:
		return self.DP | (self.opcode & 0x7F)

	def _dma_dp1(self) -> int:
		return 0x80 | (self.opcode & 0xFF)

	def _ind(self) -> int:
		return self.AR[self.ARP] & 0xFF

	def _update_ar(self) -> None:
		low = self.opcode & 0xFF
		if low & 0x30:
			tmp = self.AR[self.ARP]
			if low & 0x20:
				tmp = (tmp + 1) & MASK16
			if low & 0x10:
				tmp = (tmp - 1) & MASK16
			self.AR[self.ARP] = (self.AR[self.ARP] & 0xFE00) | (tmp & 0x01FF)

	def _update_arp(self) -> None:
		low = self.opcode & 0xFF
		if (~low) & 0x08:
			if low & 0x01:
				self._set_flag(ARP_REG)
			else:
				self._clr(ARP_REG)

	# _getdata/_putdata*/below are hand-inlined versions of
	# _ind()/_dma_dp()/_dma_dp1()/_update_ar()/_update_arp() -- these four
	# are by far the hottest methods in the core (called on nearly every
	# instruction), so the indirection is collapsed directly into each for
	# CPython call-overhead reasons. Semantics and evaluation order
	# (address computed from the pre-update AR, AR/ARP updated after,
	# regnum reads happening after the AR update in _putdata_sar) are
	# preserved exactly -- see the still-present _ind/_dma_dp*/_update_ar*
	# methods (kept for _op_larp_mar) for the reference, non-inlined form.
	def _getdata(self, shift: int, signext: bool) -> None:
		opcode = self.opcode
		low = opcode & 0xFF
		if low & 0x80:
			arp = (self.STR & ARP_REG) >> 8
			ar = self.AR
			addr = ar[arp] & 0xFF
			self.memaccess = addr
			val = self.data[addr] & MASK16
			if low & 0x30:
				tmp = ar[arp]
				if low & 0x20:
					tmp = (tmp + 1) & MASK16
				if low & 0x10:
					tmp = (tmp - 1) & MASK16
				ar[arp] = (ar[arp] & 0xFE00) | (tmp & 0x01FF)
			if (~low) & 0x08:
				if low & 0x01:
					self._set_flag(ARP_REG)
				else:
					self._clr(ARP_REG)
		else:
			addr = ((self.STR & DP_REG) << 7) | (opcode & 0x7F)
			self.memaccess = addr
			val = self.data[addr] & MASK16
		alu = ((val - 0x10000) if (val & 0x8000) else val) if signext else val
		self.ALU = (alu << shift) & MASK32

	def _putdata(self, value: int) -> None:
		opcode = self.opcode
		low = opcode & 0xFF
		if low & 0x80:
			arp = (self.STR & ARP_REG) >> 8
			ar = self.AR
			addr = ar[arp] & 0xFF
			self.memaccess = addr
			if low & 0x30:
				tmp = ar[arp]
				if low & 0x20:
					tmp = (tmp + 1) & MASK16
				if low & 0x10:
					tmp = (tmp - 1) & MASK16
				ar[arp] = (ar[arp] & 0xFE00) | (tmp & 0x01FF)
			if (~low) & 0x08:
				if low & 0x01:
					self._set_flag(ARP_REG)
				else:
					self._clr(ARP_REG)
		else:
			addr = ((self.STR & DP_REG) << 7) | (opcode & 0x7F)
			self.memaccess = addr
		self.data[addr] = value & MASK16

	def _putdata_sar(self, regnum: int) -> None:
		opcode = self.opcode
		low = opcode & 0xFF
		if low & 0x80:
			arp = (self.STR & ARP_REG) >> 8
			ar = self.AR
			addr = ar[arp] & 0xFF
			self.memaccess = addr
			if low & 0x30:
				tmp = ar[arp]
				if low & 0x20:
					tmp = (tmp + 1) & MASK16
				if low & 0x10:
					tmp = (tmp - 1) & MASK16
				ar[arp] = (ar[arp] & 0xFE00) | (tmp & 0x01FF)
			if (~low) & 0x08:
				if low & 0x01:
					self._set_flag(ARP_REG)
				else:
					self._clr(ARP_REG)
		else:
			addr = ((self.STR & DP_REG) << 7) | (opcode & 0x7F)
			self.memaccess = addr
		self.data[addr] = self.AR[regnum] & MASK16

	def _putdata_sst(self, value: int) -> None:
		opcode = self.opcode
		low = opcode & 0xFF
		if low & 0x80:
			arp = (self.STR & ARP_REG) >> 8
			ar = self.AR
			addr = ar[arp] & 0xFF
			self.memaccess = addr
			if low & 0x30:
				tmp = ar[arp]
				if low & 0x20:
					tmp = (tmp + 1) & MASK16
				if low & 0x10:
					tmp = (tmp - 1) & MASK16
				ar[arp] = (ar[arp] & 0xFE00) | (tmp & 0x01FF)
			# note: no ARP-flag update here, matches original putdata_sst
		else:
			addr = 0x80 | (opcode & 0xFF)
			self.memaccess = addr
		self.data[addr] = value & MASK16

	# -- overflow helpers --------------------------------------------------
	def _calc_add_overflow(self, addval: int) -> None:
		oldacc = self.oldacc
		v = (~(oldacc ^ addval) & (oldacc ^ self.ACC)) & MASK32
		if v & 0x80000000:
			self._set_flag(OV_FLAG)
			if self.OVM:
				self.ACC = 0x80000000 if s32(oldacc) < 0 else 0x7FFFFFFF

	def _calc_sub_overflow(self, subval: int) -> None:
		oldacc = self.oldacc
		v = ((oldacc ^ subval) & (oldacc ^ self.ACC)) & MASK32
		if v & 0x80000000:
			self._set_flag(OV_FLAG)
			if self.OVM:
				self.ACC = 0x80000000 if s32(oldacc) < 0 else 0x7FFFFFFF

	# -- stack ---------------------------------------------------------
	def _pop_stack(self) -> int:
		data = self.STACK[3]
		self.STACK[3] = self.STACK[2]
		self.STACK[2] = self.STACK[1]
		self.STACK[1] = self.STACK[0]
		return data & self.ADDR_MASK

	def _push_stack(self, data: int) -> None:
		self.STACK[0] = self.STACK[1]
		self.STACK[1] = self.STACK[2]
		self.STACK[2] = self.STACK[3]
		self.STACK[3] = data & self.ADDR_MASK

	# -- opcode field accessors ------------------------------------------
	@property
	def _op_h(self) -> int:
		return (self.opcode >> 8) & 0xFF

	@property
	def _op_l(self) -> int:
		return self.opcode & 0xFF

	# -- ROM read helper (also used for opcode fetch/branch targets) -----
	def _rdop(self, addr: int) -> int:
		return self.program[addr & self.ADDR_MASK] & MASK16

	# =====================================================================
	# Instructions -- one method per opcode, ported 1:1 from tms320c1x.cpp
	# =====================================================================

	def _op_illegal(self) -> None:
		pass  # logged as illegal in MAME; silently ignore here

	def _op_abst(self) -> None:
		if s32(self.ACC) < 0:
			self.ACC = (-self.ACC) & MASK32
			if self.OVM and self.ACC == 0x80000000:
				self.ACC = (self.ACC - 1) & MASK32

	def _op_add_sh(self) -> None:
		self.oldacc = self.ACC
		self._getdata(((self.opcode >> 8) & 0xFF) & 0xF, True)
		self.ACC = (self.ACC + self.ALU) & MASK32
		self._calc_add_overflow(self.ALU)

	def _op_addh(self) -> None:
		self.oldacc = self.ACC
		self._getdata(0, False)
		oldacc_h = (self.oldacc >> 16) & MASK16
		alu_l = self.ALU & MASK16
		alu_h = (self.ALU >> 16) & MASK16  # always 0 after getdata(0, False); kept
		# explicit to match MAME's overflow check operand exactly (source
		# compares against ALU's high word here, not the low word used in
		# the addition itself -- see tms320c1x.cpp addh()).
		new_h = (oldacc_h + alu_l) & MASK16
		self.ACC = (new_h << 16) | (self.ACC & MASK16)
		if s16(~(oldacc_h ^ alu_h) & (oldacc_h ^ new_h)) < 0:
			self._set_flag(OV_FLAG)
			if self.OVM:
				new_h = 0x8000 if s16(oldacc_h) < 0 else 0x7FFF
				self.ACC = (new_h << 16) | (self.ACC & MASK16)

	def _op_adds(self) -> None:
		self.oldacc = self.ACC
		self._getdata(0, False)
		self.ACC = (self.ACC + self.ALU) & MASK32
		self._calc_add_overflow(self.ALU)

	def _op_and(self) -> None:
		self._getdata(0, False)
		self.ACC &= self.ALU

	def _op_apac(self) -> None:
		self.oldacc = self.ACC
		self.ACC = (self.ACC + self.Preg) & MASK32
		self._calc_add_overflow(self.Preg)

	def _op_br(self) -> None:
		self.PC = self._rdop(self.PC) & self.ADDR_MASK

	def _op_banz(self) -> None:
		if self.AR[self.ARP] & 0x01FF:
			self.PC = self._rdop(self.PC) & self.ADDR_MASK
			self._branch_taken = True
		else:
			self.PC = (self.PC + 1) & self.ADDR_MASK
		tmp = (self.AR[self.ARP] - 1) & MASK16
		self.AR[self.ARP] = (self.AR[self.ARP] & 0xFE00) | (tmp & 0x01FF)

	def _cond_branch(self, taken: bool) -> None:
		if taken:
			self.PC = self._rdop(self.PC) & self.ADDR_MASK
			self._branch_taken = True
		else:
			self.PC = (self.PC + 1) & self.ADDR_MASK

	def _op_bgez(self) -> None:
		self._cond_branch(s32(self.ACC) >= 0)

	def _op_bgz(self) -> None:
		self._cond_branch(s32(self.ACC) > 0)

	def _op_bioz(self) -> None:
		self._cond_branch(self._bio_read() != 0)

	def _op_blez(self) -> None:
		self._cond_branch(s32(self.ACC) <= 0)

	def _op_blz(self) -> None:
		self._cond_branch(s32(self.ACC) < 0)

	def _op_bnz(self) -> None:
		self._cond_branch(self.ACC != 0)

	def _op_bv(self) -> None:
		taken = bool(self.OV)
		if taken:
			self._clr(OV_FLAG)
		self._cond_branch(taken)

	def _op_bz(self) -> None:
		self._cond_branch(self.ACC == 0)

	def _op_cala(self) -> None:
		self._push_stack(self.PC)
		self.PC = self.ACC & MASK16 & self.ADDR_MASK

	def _op_call(self) -> None:
		self.PC = (self.PC + 1) & self.ADDR_MASK
		self._push_stack(self.PC)
		self.PC = self._rdop(self.PC - 1) & self.ADDR_MASK

	def _op_dint(self) -> None:
		self._set_flag(INTM_FLAG)

	def _op_dmov(self) -> None:
		self._getdata(0, False)
		self.data[(self.memaccess + 1) & 0xFF] = self.ALU & MASK16

	def _op_eint(self) -> None:
		self._clr(INTM_FLAG)

	def _op_in(self) -> None:
		val = self._io_read((((self.opcode >> 8) & 0xFF)) & 7) & MASK16
		self.ALU = val
		self._putdata(val)

	def _op_lac_sh(self) -> None:
		self._getdata(((self.opcode >> 8) & 0xFF) & 0x0F, True)
		self.ACC = self.ALU & MASK32

	def _op_lack(self) -> None:
		self.ACC = self._op_l & MASK32

	def _op_lar_ar0(self) -> None:
		self._getdata(0, False)
		self.AR[0] = self.ALU & MASK16

	def _op_lar_ar1(self) -> None:
		self._getdata(0, False)
		self.AR[1] = self.ALU & MASK16

	def _op_lark_ar0(self) -> None:
		self.AR[0] = self._op_l & MASK16

	def _op_lark_ar1(self) -> None:
		self.AR[1] = self._op_l & MASK16

	def _op_larp_mar(self) -> None:
		if self._op_l & 0x80:
			self._update_ar()
			self._update_arp()

	def _op_ldp(self) -> None:
		self._getdata(0, False)
		if self.ALU & 1:
			self._set_flag(DP_REG)
		else:
			self._clr(DP_REG)

	def _op_ldpk(self) -> None:
		if self._op_l & 1:
			self._set_flag(DP_REG)
		else:
			self._clr(DP_REG)

	def _op_lst(self) -> None:
		saved_opcode = self.opcode
		if self._op_l & 0x80:
			self.opcode = self.opcode | 0x08  # suppress ARP update for LST specifically
		self._getdata(0, False)
		self.opcode = saved_opcode
		alu = self.ALU & (~INTM_FLAG) & MASK16
		self.STR = self.STR & INTM_FLAG
		self.STR |= alu
		self.STR |= STR_RESERVED
		self.STR &= MASK16

	def _op_lt(self) -> None:
		self._getdata(0, False)
		self.Treg = self.ALU & MASK16

	def _op_lta(self) -> None:
		self.oldacc = self.ACC
		self._getdata(0, False)
		self.Treg = self.ALU & MASK16
		self.ACC = (self.ACC + self.Preg) & MASK32
		self._calc_add_overflow(self.Preg)

	def _op_ltd(self) -> None:
		self.oldacc = self.ACC
		self._getdata(0, False)
		self.Treg = self.ALU & MASK16
		self.data[(self.memaccess + 1) & 0xFF] = self.ALU & MASK16
		self.ACC = (self.ACC + self.Preg) & MASK32
		self._calc_add_overflow(self.Preg)

	def _op_mpy(self) -> None:
		self._getdata(0, False)
		alu = self.ALU & MASK16
		alu = alu - 0x10000 if alu & 0x8000 else alu
		treg = self.Treg & MASK16
		treg = treg - 0x10000 if treg & 0x8000 else treg
		p = alu * treg
		self.Preg = p & MASK32
		if self.Preg == 0x40000000:
			self.Preg = 0xC0000000

	def _op_mpyk(self) -> None:
		# 13-bit immediate, sign-extended: (opcode.w.l << 3) as int16, >> 3
		val = ((self.opcode & MASK16) << 3) & MASK16
		val = s16(val) >> 3
		self.Preg = (s16(self.Treg) * val) & MASK32

	def _op_nop(self) -> None:
		pass

	def _op_or(self) -> None:
		self._getdata(0, False)
		self.ACC = (self.ACC & 0xFFFF0000) | (((self.ACC & MASK16) | (self.ALU & MASK16)) & MASK16)

	def _op_out(self) -> None:
		self._getdata(0, False)
		self._io_write((((self.opcode >> 8) & 0xFF)) & 7, self.ALU & MASK16)

	def _op_pac(self) -> None:
		self.ACC = self.Preg & MASK32

	def _op_pop(self) -> None:
		self.ACC = self._pop_stack() & MASK16

	def _op_push(self) -> None:
		self._push_stack(self.ACC & MASK16)

	def _op_ret(self) -> None:
		self.PC = self._pop_stack()

	def _op_rovm(self) -> None:
		self._clr(OVM_FLAG)

	def _op_sach_sh(self) -> None:
		shift = ((self.opcode >> 8) & 0xFF) & 7
		val = (self.ACC << shift) & MASK32  # C truncates the shift to 32 bits
		self._putdata((val >> 16) & MASK16)

	def _op_sacl(self) -> None:
		self._putdata(self.ACC & MASK16)

	def _op_sar_ar0(self) -> None:
		self._putdata_sar(0)

	def _op_sar_ar1(self) -> None:
		self._putdata_sar(1)

	def _op_sovm(self) -> None:
		self._set_flag(OVM_FLAG)

	def _op_spac(self) -> None:
		self.oldacc = self.ACC
		self.ACC = (self.ACC - self.Preg) & MASK32
		self._calc_sub_overflow(self.Preg)

	def _op_sst(self) -> None:
		self._putdata_sst(self.STR)

	def _op_sub_sh(self) -> None:
		self.oldacc = self.ACC
		self._getdata(((self.opcode >> 8) & 0xFF) & 0x0F, True)
		self.ACC = (self.ACC - self.ALU) & MASK32
		self._calc_sub_overflow(self.ALU)

	def _op_subc(self) -> None:
		self.oldacc = self.ACC
		self._getdata(15, False)
		alu = (s32(self.ACC) - s32(self.ALU)) & MASK32
		if s32((self.oldacc ^ alu) & (self.oldacc ^ self.ACC)) < 0:
			self._set_flag(OV_FLAG)
		if s32(alu) >= 0:
			self.ACC = ((alu << 1) + 1) & MASK32
		else:
			self.ACC = (self.ACC << 1) & MASK32

	def _op_subh(self) -> None:
		self.oldacc = self.ACC
		self._getdata(16, False)
		self.ACC = (self.ACC - self.ALU) & MASK32
		self._calc_sub_overflow(self.ALU)

	def _op_subs(self) -> None:
		self.oldacc = self.ACC
		self._getdata(0, False)
		self.ACC = (self.ACC - self.ALU) & MASK32
		self._calc_sub_overflow(self.ALU)

	def _op_tblr(self) -> None:
		val = self._rdop(self.ACC & MASK16)
		self._putdata(val)
		self.STACK[0] = self.STACK[1]  # documented hardware quirk, preserved verbatim

	def _op_tblw(self) -> None:
		self._getdata(0, False)
		addr = (self.ACC & MASK16) & self.ADDR_MASK
		self.program[addr] = self.ALU & MASK16
		self.STACK[0] = self.STACK[1]  # documented hardware quirk, preserved verbatim

	def _op_xor(self) -> None:
		self._getdata(0, False)
		self.ACC = (self.ACC & 0xFFFF0000) | (((self.ACC & MASK16) ^ (self.ALU & MASK16)) & MASK16)

	def _op_zac(self) -> None:
		self.ACC = 0

	def _op_zalh(self) -> None:
		self._getdata(0, False)
		self.ACC = (self.ALU & MASK16) << 16

	def _op_zals(self) -> None:
		self._getdata(0, False)
		self.ACC = self.ALU & MASK16

	# -- dispatch tables: (cycles, function) per major opcode byte --------
	def _build_main_table(self):
		t = [(0, self._op_illegal)] * 256
		for i in range(0x00, 0x10):
			t[i] = (1, self._op_add_sh)
		for i in range(0x10, 0x20):
			t[i] = (1, self._op_sub_sh)
		for i in range(0x20, 0x30):
			t[i] = (1, self._op_lac_sh)
		t[0x30] = (1, self._op_sar_ar0)
		t[0x31] = (1, self._op_sar_ar1)
		t[0x38] = (1, self._op_lar_ar0)
		t[0x39] = (1, self._op_lar_ar1)
		for i in range(0x40, 0x48):
			t[i] = (2, self._op_in)
		for i in range(0x48, 0x50):
			t[i] = (2, self._op_out)
		t[0x50] = (1, self._op_sacl)
		for i in range(0x58, 0x60):
			t[i] = (1, self._op_sach_sh)
		t[0x60] = (1, self._op_addh)
		t[0x61] = (1, self._op_adds)
		t[0x62] = (1, self._op_subh)
		t[0x63] = (1, self._op_subs)
		t[0x64] = (1, self._op_subc)
		t[0x65] = (1, self._op_zalh)
		t[0x66] = (1, self._op_zals)
		t[0x67] = (3, self._op_tblr)
		t[0x68] = (1, self._op_larp_mar)
		t[0x69] = (1, self._op_dmov)
		t[0x6A] = (1, self._op_lt)
		t[0x6B] = (1, self._op_ltd)
		t[0x6C] = (1, self._op_lta)
		t[0x6D] = (1, self._op_mpy)
		t[0x6E] = (1, self._op_ldpk)
		t[0x6F] = (1, self._op_ldp)
		t[0x70] = (1, self._op_lark_ar0)
		t[0x71] = (1, self._op_lark_ar1)
		t[0x78] = (1, self._op_xor)
		t[0x79] = (1, self._op_and)
		t[0x7A] = (1, self._op_or)
		t[0x7B] = (1, self._op_lst)
		t[0x7C] = (1, self._op_sst)
		t[0x7D] = (3, self._op_tblw)
		t[0x7E] = (1, self._op_lack)
		# 0x7F handled separately via _dispatch_7f
		for i in range(0x80, 0xA0):
			t[i] = (1, self._op_mpyk)
		t[0xF4] = (1, self._op_banz)
		t[0xF5] = (1, self._op_bv)
		t[0xF6] = (1, self._op_bioz)
		t[0xF8] = (2, self._op_call)
		t[0xF9] = (2, self._op_br)
		t[0xFA] = (1, self._op_blz)
		t[0xFB] = (1, self._op_blez)
		t[0xFC] = (1, self._op_bgz)
		t[0xFD] = (1, self._op_bgez)
		t[0xFE] = (1, self._op_bnz)
		t[0xFF] = (1, self._op_bz)
		return t

	def _build_7f_table(self):
		t = [(0, self._op_illegal)] * 32
		t[0x00] = (1, self._op_nop)
		t[0x01] = (1, self._op_dint)
		t[0x02] = (1, self._op_eint)
		t[0x08] = (1, self._op_abst)
		t[0x09] = (1, self._op_zac)
		t[0x0A] = (1, self._op_rovm)
		t[0x0B] = (1, self._op_sovm)
		t[0x0C] = (2, self._op_cala)
		t[0x0D] = (2, self._op_ret)
		t[0x0E] = (1, self._op_pac)
		t[0x0F] = (1, self._op_apac)
		t[0x10] = (1, self._op_spac)
		t[0x1C] = (2, self._op_push)
		t[0x1D] = (2, self._op_pop)
		return t

	# =====================================================================
	# Execution
	# =====================================================================

	def _service_interrupt(self) -> int:
		self.int_pending = False
		self._set_flag(INTM_FLAG)
		self._push_stack(self.PC)
		self.PC = 0x0002
		return 3  # PUSH (2) + DINT (1) cycles, per MAME's Ext_IRQ comment

	def step(self) -> int:
		"""Execute exactly one instruction (servicing a pending interrupt
		first if one is pending and allowed). Returns cycles consumed."""
		cycles = 0
		if self.int_pending:
			prev = self.opcode
			prev_h = (prev >> 8) & 0xFF
			# don't interrupt right after MPY, MPYK, or EINT (matches MAME)
			if (self.STR & INTM_FLAG) == 0 and prev_h != 0x6D and (prev_h & 0xE0) != 0x80 and prev != 0x7F82:
				cycles += self._service_interrupt()

		pc = self.PC
		self.PREVPC = pc
		opcode = self.program[pc & self.ADDR_MASK] & MASK16
		self.opcode = opcode
		self.PC = (pc + 1) & self.ADDR_MASK

		op_h = (opcode >> 8) & 0xFF
		if op_h != 0x7F:
			c, fn = self._dispatch_main[op_h]
		else:
			c, fn = self._dispatch_7f[opcode & 0x1F]
		cycles += c
		self._branch_taken = False
		fn()
		if self._branch_taken:
			# MAME's add_branch_cycle(): taken conditional branches (and
			# BANZ) cost their own base cycle count a second time.
			cycles += c
		return cycles

	def run(self, cycles: int) -> int:
		"""Run until at least `cycles` worth of instructions have executed.
		Returns the (possibly negative) overshoot, mirroring MAME's
		do-while(icount>0) scheduling so callers can carry the remainder
		into the next quantum.

		This is step()'s body inlined into the loop (with program/dispatch
		table/ADDR_MASK hoisted to locals) rather than calling self.step()
		per iteration -- this is machine.py's hot path (millions of calls
		per second of virtual audio), where the per-call overhead of a
		separate step() invocation is significant. step() itself is kept
		as the single source of truth for instruction semantics (still
		used directly by tools/smoke_test_dsp.py) -- if instruction
		dispatch/interrupt logic changes there, mirror the change here."""
		if self.in_reset:
			return 0
		budget = cycles
		program = self.program
		dispatch_main = self._dispatch_main
		dispatch_7f = self._dispatch_7f
		addr_mask = self.ADDR_MASK
		while budget > 0:
			step_cycles = 0
			if self.int_pending:
				prev = self.opcode
				prev_h = (prev >> 8) & 0xFF
				if (self.STR & INTM_FLAG) == 0 and prev_h != 0x6D and (prev_h & 0xE0) != 0x80 and prev != 0x7F82:
					step_cycles += self._service_interrupt()

			pc = self.PC
			self.PREVPC = pc
			opcode = program[pc & addr_mask] & MASK16
			self.opcode = opcode
			self.PC = (pc + 1) & addr_mask

			op_h = (opcode >> 8) & 0xFF
			if op_h != 0x7F:
				c, fn = dispatch_main[op_h]
			else:
				c, fn = dispatch_7f[opcode & 0x1F]
			step_cycles += c
			self._branch_taken = False
			fn()
			if self._branch_taken:
				step_cycles += c
			budget -= step_cycles
		return budget
