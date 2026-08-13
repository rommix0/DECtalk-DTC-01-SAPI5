/* TMS32010 DSP core -- see tms32010.h for provenance/licensing notes.
 * Ported from emu/tms32010.py (itself a 1:1 port of MAME's tms320c1x),
 * preserving all documented hardware quirks verbatim.
 */
#include "tms32010.h"

#define MASK16 0xFFFFu
#define MASK32 0xFFFFFFFFu

#define OV_FLAG      0x8000
#define OVM_FLAG     0x4000
#define INTM_FLAG    0x2000
#define ARP_REG      0x0100
#define DP_REG       0x0001
#define STR_RESERVED 0x1EFE /* reserved status bits that always read back 1 */

#define CLRF(c, flag)  ((c)->STR = (uint16_t)((((c)->STR & (uint16_t)~(flag)) | STR_RESERVED) & MASK16))
#define SETF(c, flag)  ((c)->STR = (uint16_t)((((c)->STR | (flag)) | STR_RESERVED) & MASK16))
#define ARP_OF(c)      (unsigned)(((c)->STR & ARP_REG) >> 8)
#define DP_OF(c)       (unsigned)(((c)->STR & DP_REG) << 7)

/* ---- operation ids -------------------------------------------------- */
enum {
    OP_ILLEGAL = 0,
    OP_ADD_SH, OP_SUB_SH, OP_LAC_SH, OP_SAR_AR0, OP_SAR_AR1, OP_LAR_AR0,
    OP_LAR_AR1, OP_IN, OP_OUT, OP_SACL, OP_SACH_SH, OP_ADDH, OP_ADDS,
    OP_SUBH, OP_SUBS, OP_SUBC, OP_ZALH, OP_ZALS, OP_TBLR, OP_LARP_MAR,
    OP_DMOV, OP_LT, OP_LTD, OP_LTA, OP_MPY, OP_LDPK, OP_LDP, OP_LARK_AR0,
    OP_LARK_AR1, OP_XOR, OP_AND, OP_OR, OP_LST, OP_SST, OP_TBLW, OP_LACK,
    OP_GROUP7F, OP_MPYK, OP_BANZ, OP_BV, OP_BIOZ, OP_CALL, OP_BR, OP_BLZ,
    OP_BLEZ, OP_BGZ, OP_BGEZ, OP_BNZ, OP_BZ,
    /* 0x7F group */
    OP_NOP, OP_DINT, OP_EINT, OP_ABST, OP_ZAC, OP_ROVM, OP_SOVM, OP_CALA,
    OP_RET, OP_PAC, OP_APAC, OP_SPAC, OP_PUSH, OP_POP
};

static uint8_t op_id_main[256], op_cyc_main[256];
static uint8_t op_id_7f[32], op_cyc_7f[32];
static int tables_built = 0;

static void build_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) { op_id_main[i] = OP_ILLEGAL; op_cyc_main[i] = 0; }
    for (i = 0; i < 32; i++)  { op_id_7f[i]  = OP_ILLEGAL; op_cyc_7f[i]  = 0; }

#define M(idx, id, cyc) do { op_id_main[(idx)] = (uint8_t)(id); op_cyc_main[(idx)] = (uint8_t)(cyc); } while (0)
#define S(idx, id, cyc) do { op_id_7f[(idx)]   = (uint8_t)(id); op_cyc_7f[(idx)]   = (uint8_t)(cyc); } while (0)

    for (i = 0x00; i < 0x10; i++) M(i, OP_ADD_SH, 1);
    for (i = 0x10; i < 0x20; i++) M(i, OP_SUB_SH, 1);
    for (i = 0x20; i < 0x30; i++) M(i, OP_LAC_SH, 1);
    M(0x30, OP_SAR_AR0, 1);  M(0x31, OP_SAR_AR1, 1);
    M(0x38, OP_LAR_AR0, 1);  M(0x39, OP_LAR_AR1, 1);
    for (i = 0x40; i < 0x48; i++) M(i, OP_IN, 2);
    for (i = 0x48; i < 0x50; i++) M(i, OP_OUT, 2);
    M(0x50, OP_SACL, 1);
    for (i = 0x58; i < 0x60; i++) M(i, OP_SACH_SH, 1);
    M(0x60, OP_ADDH, 1); M(0x61, OP_ADDS, 1); M(0x62, OP_SUBH, 1);
    M(0x63, OP_SUBS, 1); M(0x64, OP_SUBC, 1); M(0x65, OP_ZALH, 1);
    M(0x66, OP_ZALS, 1); M(0x67, OP_TBLR, 3); M(0x68, OP_LARP_MAR, 1);
    M(0x69, OP_DMOV, 1); M(0x6A, OP_LT, 1);   M(0x6B, OP_LTD, 1);
    M(0x6C, OP_LTA, 1);  M(0x6D, OP_MPY, 1);  M(0x6E, OP_LDPK, 1);
    M(0x6F, OP_LDP, 1);  M(0x70, OP_LARK_AR0, 1); M(0x71, OP_LARK_AR1, 1);
    M(0x78, OP_XOR, 1);  M(0x79, OP_AND, 1);  M(0x7A, OP_OR, 1);
    M(0x7B, OP_LST, 1);  M(0x7C, OP_SST, 1);  M(0x7D, OP_TBLW, 3);
    M(0x7E, OP_LACK, 1); M(0x7F, OP_GROUP7F, 0);
    for (i = 0x80; i < 0xA0; i++) M(i, OP_MPYK, 1);
    M(0xF4, OP_BANZ, 1); M(0xF5, OP_BV, 1);   M(0xF6, OP_BIOZ, 1);
    M(0xF8, OP_CALL, 2); M(0xF9, OP_BR, 2);   M(0xFA, OP_BLZ, 1);
    M(0xFB, OP_BLEZ, 1); M(0xFC, OP_BGZ, 1);  M(0xFD, OP_BGEZ, 1);
    M(0xFE, OP_BNZ, 1);  M(0xFF, OP_BZ, 1);

    S(0x00, OP_NOP, 1);  S(0x01, OP_DINT, 1); S(0x02, OP_EINT, 1);
    S(0x08, OP_ABST, 1); S(0x09, OP_ZAC, 1);  S(0x0A, OP_ROVM, 1);
    S(0x0B, OP_SOVM, 1); S(0x0C, OP_CALA, 2); S(0x0D, OP_RET, 2);
    S(0x0E, OP_PAC, 1);  S(0x0F, OP_APAC, 1); S(0x10, OP_SPAC, 1);
    S(0x1C, OP_PUSH, 2); S(0x1D, OP_POP, 2);
#undef M
#undef S
    tables_built = 1;
}

/* ---- AR / ARP update (indirect addressing side effects) ------------- */
static void update_ar(tms32010_t *c)
{
    unsigned low = c->opcode & 0xFF;
    if (low & 0x30) {
        unsigned arp = ARP_OF(c);
        uint16_t tmp = c->AR[arp];
        if (low & 0x20) tmp = (uint16_t)(tmp + 1);
        if (low & 0x10) tmp = (uint16_t)(tmp - 1);
        c->AR[arp] = (uint16_t)((c->AR[arp] & 0xFE00) | (tmp & 0x01FF));
    }
}

static void update_arp(tms32010_t *c)
{
    unsigned low = c->opcode & 0xFF;
    if ((~low) & 0x08) {
        if (low & 0x01) SETF(c, ARP_REG);
        else            CLRF(c, ARP_REG);
    }
}

/* ---- memory helpers -------------------------------------------------- */
static void getdata(tms32010_t *c, int shift, int signext)
{
    uint16_t opcode = c->opcode;
    unsigned low = opcode & 0xFF;
    uint8_t addr;
    uint16_t val;
    uint32_t alu;

    if (low & 0x80) {
        addr = (uint8_t)(c->AR[ARP_OF(c)] & 0xFF);
        c->memaccess = addr;
        val = c->data[addr];
        update_ar(c);
        update_arp(c);
    } else {
        addr = (uint8_t)(DP_OF(c) | (opcode & 0x7F));
        c->memaccess = addr;
        val = c->data[addr];
    }
    alu = signext ? (uint32_t)(int32_t)(int16_t)val : (uint32_t)val;
    c->ALU = (alu << shift) & MASK32;
}

static void putdata(tms32010_t *c, uint16_t value)
{
    uint16_t opcode = c->opcode;
    unsigned low = opcode & 0xFF;
    uint8_t addr;

    if (low & 0x80) {
        addr = (uint8_t)(c->AR[ARP_OF(c)] & 0xFF);
        c->memaccess = addr;
        update_ar(c);
        update_arp(c);
    } else {
        addr = (uint8_t)(DP_OF(c) | (opcode & 0x7F));
        c->memaccess = addr;
    }
    c->data[addr] = value;
}

static void putdata_sar(tms32010_t *c, int regnum)
{
    uint16_t opcode = c->opcode;
    unsigned low = opcode & 0xFF;
    uint8_t addr;

    if (low & 0x80) {
        addr = (uint8_t)(c->AR[ARP_OF(c)] & 0xFF);
        c->memaccess = addr;
        update_ar(c);
        update_arp(c);
    } else {
        addr = (uint8_t)(DP_OF(c) | (opcode & 0x7F));
        c->memaccess = addr;
    }
    /* NB: AR is read *after* the update, matching putdata_sar in MAME. */
    c->data[addr] = c->AR[regnum];
}

static void putdata_sst(tms32010_t *c, uint16_t value)
{
    uint16_t opcode = c->opcode;
    unsigned low = opcode & 0xFF;
    uint8_t addr;

    if (low & 0x80) {
        addr = (uint8_t)(c->AR[ARP_OF(c)] & 0xFF);
        c->memaccess = addr;
        update_ar(c); /* note: no ARP update here -- matches putdata_sst */
    } else {
        addr = (uint8_t)(0x80 | (opcode & 0xFF));
        c->memaccess = addr;
    }
    c->data[addr] = value;
}

/* ---- overflow helpers ------------------------------------------------ */
/* NB: sign tests below are explicit bit-31 checks rather than casts to
 * int32_t. Python's ints are arbitrary precision, so the original uses
 * s32()/comparisons freely; in C, converting an out-of-range uint32_t to
 * int32_t is implementation-defined and signed overflow is UB. Bit tests
 * and unsigned arithmetic reproduce the Python semantics exactly while
 * staying fully defined. */
static void calc_add_overflow(tms32010_t *c, uint32_t addval)
{
    uint32_t oldacc = c->oldacc;
    uint32_t v = (~(oldacc ^ addval) & (oldacc ^ c->ACC)) & MASK32;
    if (v & 0x80000000u) {
        SETF(c, OV_FLAG);
        if (c->STR & OVM_FLAG)
            c->ACC = (oldacc & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
    }
}

static void calc_sub_overflow(tms32010_t *c, uint32_t subval)
{
    uint32_t oldacc = c->oldacc;
    uint32_t v = ((oldacc ^ subval) & (oldacc ^ c->ACC)) & MASK32;
    if (v & 0x80000000u) {
        SETF(c, OV_FLAG);
        if (c->STR & OVM_FLAG)
            c->ACC = (oldacc & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
    }
}

/* ---- stack ----------------------------------------------------------- */
static uint16_t pop_stack(tms32010_t *c)
{
    uint16_t d = c->STACK[3];
    c->STACK[3] = c->STACK[2];
    c->STACK[2] = c->STACK[1];
    c->STACK[1] = c->STACK[0];
    return (uint16_t)(d & TMS_ADDR_MASK);
}

static void push_stack(tms32010_t *c, uint16_t d)
{
    c->STACK[0] = c->STACK[1];
    c->STACK[1] = c->STACK[2];
    c->STACK[2] = c->STACK[3];
    c->STACK[3] = (uint16_t)(d & TMS_ADDR_MASK);
}

static void cond_branch(tms32010_t *c, int taken)
{
    if (taken) {
        c->PC = (uint16_t)(c->program[c->PC & TMS_ADDR_MASK] & TMS_ADDR_MASK);
        c->branch_taken = 1;
    } else {
        c->PC = (uint16_t)((c->PC + 1) & TMS_ADDR_MASK);
    }
}

/* ---- lifecycle ------------------------------------------------------- */
void tms_reset(tms32010_t *c)
{
    c->PC = 0;
    c->ACC = 0;
    c->int_pending = 0;
    CLRF(c, OV_FLAG | ARP_REG | DP_REG);
    SETF(c, OVM_FLAG | INTM_FLAG); /* net result: STR == 0x7efe */
}

void tms_init(tms32010_t *c, const uint16_t *program_words, int nwords,
              void *owner, tms_io_read_fn io_read, tms_io_write_fn io_write,
              tms_bio_read_fn bio_read)
{
    int i;
    if (!tables_built) build_tables();

    for (i = 0; i < TMS_PROGRAM_SIZE; i++) c->program[i] = 0;
    for (i = 0; i < TMS_DATA_SIZE; i++)    c->data[i] = 0;
    if (nwords > TMS_PROGRAM_SIZE) nwords = TMS_PROGRAM_SIZE;
    for (i = 0; i < nwords; i++) c->program[i] = program_words[i];

    c->PC = c->PREVPC = 0;
    c->STR = 0;
    c->ACC = c->ALU = c->Preg = c->oldacc = 0;
    c->Treg = 0;
    c->AR[0] = c->AR[1] = 0;
    c->STACK[0] = c->STACK[1] = c->STACK[2] = c->STACK[3] = 0;
    c->opcode = 0;
    c->memaccess = 0;
    c->int_pending = 0;
    c->in_reset = 1;
    c->branch_taken = 0;

    c->owner = owner;
    c->io_read = io_read;
    c->io_write = io_write;
    c->bio_read = bio_read;

    tms_reset(c);
}

void tms_set_reset_line(tms32010_t *c, int asserted)
{
    if (asserted) { c->in_reset = 1; tms_reset(c); }
    else            c->in_reset = 0;
}

void tms_set_int_line(tms32010_t *c, int asserted)
{
    /* Pending interrupts cannot be cleared externally -- only servicing one
     * clears int_pending. Matches MAME's execute_set_input, which only ORs
     * the pending flag in. */
    if (asserted) c->int_pending = 1;
}

static int service_interrupt(tms32010_t *c)
{
    c->int_pending = 0;
    SETF(c, INTM_FLAG);
    push_stack(c, c->PC);
    c->PC = 0x0002;
    return 3; /* PUSH (2) + DINT (1), per MAME's Ext_IRQ comment */
}

/* ---- execution ------------------------------------------------------- */
int tms_step(tms32010_t *c)
{
    int cycles = 0, base;
    uint16_t opcode, pc;
    unsigned op_h, id;

    if (c->int_pending) {
        uint16_t prev = c->opcode;
        unsigned prev_h = (prev >> 8) & 0xFF;
        /* don't interrupt right after MPY, MPYK, or EINT (matches MAME) */
        if ((c->STR & INTM_FLAG) == 0 && prev_h != 0x6D &&
            (prev_h & 0xE0) != 0x80 && prev != 0x7F82) {
            cycles += service_interrupt(c);
        }
    }

    pc = c->PC;
    c->PREVPC = pc;
    opcode = c->program[pc & TMS_ADDR_MASK];
    c->opcode = opcode;
    c->PC = (uint16_t)((pc + 1) & TMS_ADDR_MASK);

    op_h = (opcode >> 8) & 0xFF;
    if (op_h != 0x7F) { id = op_id_main[op_h]; base = op_cyc_main[op_h]; }
    else              { id = op_id_7f[opcode & 0x1F]; base = op_cyc_7f[opcode & 0x1F]; }

    c->branch_taken = 0;

    switch (id) {
    case OP_ADD_SH:
        c->oldacc = c->ACC;
        getdata(c, (int)(op_h & 0xF), 1);
        c->ACC = (c->ACC + c->ALU) & MASK32;
        calc_add_overflow(c, c->ALU);
        break;
    case OP_SUB_SH:
        c->oldacc = c->ACC;
        getdata(c, (int)(op_h & 0xF), 1);
        c->ACC = (c->ACC - c->ALU) & MASK32;
        calc_sub_overflow(c, c->ALU);
        break;
    case OP_LAC_SH:
        getdata(c, (int)(op_h & 0xF), 1);
        c->ACC = c->ALU & MASK32;
        break;
    case OP_SAR_AR0: putdata_sar(c, 0); break;
    case OP_SAR_AR1: putdata_sar(c, 1); break;
    case OP_LAR_AR0: getdata(c, 0, 0); c->AR[0] = (uint16_t)(c->ALU & MASK16); break;
    case OP_LAR_AR1: getdata(c, 0, 0); c->AR[1] = (uint16_t)(c->ALU & MASK16); break;
    case OP_IN: {
        uint16_t v = (uint16_t)(c->io_read(c->owner, (int)(op_h & 7)) & MASK16);
        c->ALU = v;
        putdata(c, v);
        break;
    }
    case OP_OUT:
        getdata(c, 0, 0);
        c->io_write(c->owner, (int)(op_h & 7), (uint16_t)(c->ALU & MASK16));
        break;
    case OP_SACL: putdata(c, (uint16_t)(c->ACC & MASK16)); break;
    case OP_SACH_SH: {
        unsigned sh = op_h & 7;
        uint32_t val = (c->ACC << sh) & MASK32; /* C truncates shift to 32 bits */
        putdata(c, (uint16_t)((val >> 16) & MASK16));
        break;
    }
    case OP_ADDH: {
        uint16_t oldacc_h, alu_l, alu_h, new_h;
        c->oldacc = c->ACC;
        getdata(c, 0, 0);
        oldacc_h = (uint16_t)((c->oldacc >> 16) & MASK16);
        alu_l    = (uint16_t)(c->ALU & MASK16);
        /* alu_h is always 0 after getdata(0,0); kept explicit to match
         * MAME's overflow-check operand exactly (it compares against the
         * high word here, not the low word used in the addition itself). */
        alu_h    = (uint16_t)((c->ALU >> 16) & MASK16);
        new_h    = (uint16_t)((oldacc_h + alu_l) & MASK16);
        c->ACC   = ((uint32_t)new_h << 16) | (c->ACC & MASK16);
        if ((int16_t)((uint16_t)(~(oldacc_h ^ alu_h) & (oldacc_h ^ new_h))) < 0) {
            SETF(c, OV_FLAG);
            if (c->STR & OVM_FLAG) {
                new_h = ((int16_t)oldacc_h < 0) ? (uint16_t)0x8000 : (uint16_t)0x7FFF;
                c->ACC = ((uint32_t)new_h << 16) | (c->ACC & MASK16);
            }
        }
        break;
    }
    case OP_ADDS:
        c->oldacc = c->ACC;
        getdata(c, 0, 0);
        c->ACC = (c->ACC + c->ALU) & MASK32;
        calc_add_overflow(c, c->ALU);
        break;
    case OP_SUBH:
        c->oldacc = c->ACC;
        getdata(c, 16, 0);
        c->ACC = (c->ACC - c->ALU) & MASK32;
        calc_sub_overflow(c, c->ALU);
        break;
    case OP_SUBS:
        c->oldacc = c->ACC;
        getdata(c, 0, 0);
        c->ACC = (c->ACC - c->ALU) & MASK32;
        calc_sub_overflow(c, c->ALU);
        break;
    case OP_SUBC: {
        uint32_t alu;
        c->oldacc = c->ACC;
        getdata(c, 15, 0);
        alu = (c->ACC - c->ALU) & MASK32;
        if (((c->oldacc ^ alu) & (c->oldacc ^ c->ACC)) & 0x80000000u)
            SETF(c, OV_FLAG);
        if (!(alu & 0x80000000u)) c->ACC = ((alu << 1) + 1) & MASK32;
        else                      c->ACC = (c->ACC << 1) & MASK32;
        break;
    }
    case OP_ZALH: getdata(c, 0, 0); c->ACC = (c->ALU & MASK16) << 16; break;
    case OP_ZALS: getdata(c, 0, 0); c->ACC = c->ALU & MASK16; break;
    case OP_TBLR: {
        uint16_t v = c->program[(c->ACC & MASK16) & TMS_ADDR_MASK];
        putdata(c, v);
        c->STACK[0] = c->STACK[1]; /* documented hardware quirk, verbatim */
        break;
    }
    case OP_LARP_MAR:
        if (c->opcode & 0x80) { update_ar(c); update_arp(c); }
        break;
    case OP_DMOV:
        getdata(c, 0, 0);
        c->data[(uint8_t)(c->memaccess + 1)] = (uint16_t)(c->ALU & MASK16);
        break;
    case OP_LT: getdata(c, 0, 0); c->Treg = (uint16_t)(c->ALU & MASK16); break;
    case OP_LTD:
        c->oldacc = c->ACC;
        getdata(c, 0, 0);
        c->Treg = (uint16_t)(c->ALU & MASK16);
        c->data[(uint8_t)(c->memaccess + 1)] = (uint16_t)(c->ALU & MASK16);
        c->ACC = (c->ACC + c->Preg) & MASK32;
        calc_add_overflow(c, c->Preg);
        break;
    case OP_LTA:
        c->oldacc = c->ACC;
        getdata(c, 0, 0);
        c->Treg = (uint16_t)(c->ALU & MASK16);
        c->ACC = (c->ACC + c->Preg) & MASK32;
        calc_add_overflow(c, c->Preg);
        break;
    case OP_MPY: {
        int32_t a, t;
        getdata(c, 0, 0);
        a = (int16_t)(c->ALU & MASK16);
        t = (int16_t)c->Treg;
        c->Preg = (uint32_t)(a * t) & MASK32;
        if (c->Preg == 0x40000000u) c->Preg = 0xC0000000u;
        break;
    }
    case OP_MPYK: {
        /* 13-bit immediate, sign-extended: (opcode << 3) as int16, >> 3 */
        int32_t val = (int16_t)((uint16_t)(c->opcode << 3));
        val >>= 3;
        c->Preg = (uint32_t)((int32_t)(int16_t)c->Treg * val) & MASK32;
        break;
    }
    case OP_LDPK:
        if (c->opcode & 1) SETF(c, DP_REG); else CLRF(c, DP_REG);
        break;
    case OP_LDP:
        getdata(c, 0, 0);
        if (c->ALU & 1) SETF(c, DP_REG); else CLRF(c, DP_REG);
        break;
    case OP_LARK_AR0: c->AR[0] = (uint16_t)(c->opcode & 0xFF); break;
    case OP_LARK_AR1: c->AR[1] = (uint16_t)(c->opcode & 0xFF); break;
    case OP_XOR:
        getdata(c, 0, 0);
        c->ACC = (c->ACC & 0xFFFF0000u) | (((c->ACC & MASK16) ^ (c->ALU & MASK16)) & MASK16);
        break;
    case OP_AND: getdata(c, 0, 0); c->ACC &= c->ALU; break;
    case OP_OR:
        getdata(c, 0, 0);
        c->ACC = (c->ACC & 0xFFFF0000u) | (((c->ACC & MASK16) | (c->ALU & MASK16)) & MASK16);
        break;
    case OP_LST: {
        uint16_t saved = c->opcode;
        uint16_t alu;
        if (c->opcode & 0x80) c->opcode = (uint16_t)(c->opcode | 0x08); /* suppress ARP update */
        getdata(c, 0, 0);
        c->opcode = saved;
        alu = (uint16_t)(c->ALU & (uint16_t)~INTM_FLAG & MASK16);
        c->STR = (uint16_t)(c->STR & INTM_FLAG);
        c->STR = (uint16_t)(c->STR | alu);
        c->STR = (uint16_t)(c->STR | STR_RESERVED);
        break;
    }
    case OP_SST:  putdata_sst(c, c->STR); break;
    case OP_TBLW:
        getdata(c, 0, 0);
        c->program[(c->ACC & MASK16) & TMS_ADDR_MASK] = (uint16_t)(c->ALU & MASK16);
        c->STACK[0] = c->STACK[1]; /* documented hardware quirk, verbatim */
        break;
    case OP_LACK: c->ACC = (uint32_t)(c->opcode & 0xFF); break;

    case OP_BANZ: {
        unsigned arp = ARP_OF(c);
        uint16_t tmp;
        if (c->AR[arp] & 0x01FF) {
            c->PC = (uint16_t)(c->program[c->PC & TMS_ADDR_MASK] & TMS_ADDR_MASK);
            c->branch_taken = 1;
        } else {
            c->PC = (uint16_t)((c->PC + 1) & TMS_ADDR_MASK);
        }
        tmp = (uint16_t)(c->AR[arp] - 1);
        c->AR[arp] = (uint16_t)((c->AR[arp] & 0xFE00) | (tmp & 0x01FF));
        break;
    }
    case OP_BV: {
        int taken = (c->STR & OV_FLAG) ? 1 : 0;
        if (taken) CLRF(c, OV_FLAG);
        cond_branch(c, taken);
        break;
    }
    case OP_BIOZ: cond_branch(c, c->bio_read(c->owner) != 0); break;
    case OP_CALL:
        c->PC = (uint16_t)((c->PC + 1) & TMS_ADDR_MASK);
        push_stack(c, c->PC);
        c->PC = (uint16_t)(c->program[(uint16_t)(c->PC - 1) & TMS_ADDR_MASK] & TMS_ADDR_MASK);
        break;
    case OP_BR:   c->PC = (uint16_t)(c->program[c->PC & TMS_ADDR_MASK] & TMS_ADDR_MASK); break;
    case OP_BLZ:  cond_branch(c,  (c->ACC & 0x80000000u) != 0); break;
    case OP_BLEZ: cond_branch(c, ((c->ACC & 0x80000000u) != 0) || c->ACC == 0); break;
    case OP_BGZ:  cond_branch(c, ((c->ACC & 0x80000000u) == 0) && c->ACC != 0); break;
    case OP_BGEZ: cond_branch(c,  (c->ACC & 0x80000000u) == 0); break;
    case OP_BNZ:  cond_branch(c, c->ACC != 0); break;
    case OP_BZ:   cond_branch(c, c->ACC == 0); break;

    /* --- 0x7F group --- */
    case OP_NOP:  break;
    case OP_DINT: SETF(c, INTM_FLAG); break;
    case OP_EINT: CLRF(c, INTM_FLAG); break;
    case OP_ABST:
        if (c->ACC & 0x80000000u) {
            c->ACC = (0u - c->ACC) & MASK32; /* unsigned negate: defined for 0x80000000 */
            if ((c->STR & OVM_FLAG) && c->ACC == 0x80000000u)
                c->ACC = (c->ACC - 1) & MASK32;
        }
        break;
    case OP_ZAC:  c->ACC = 0; break;
    case OP_ROVM: CLRF(c, OVM_FLAG); break;
    case OP_SOVM: SETF(c, OVM_FLAG); break;
    case OP_CALA:
        push_stack(c, c->PC);
        c->PC = (uint16_t)(c->ACC & MASK16 & TMS_ADDR_MASK);
        break;
    case OP_RET:  c->PC = pop_stack(c); break;
    case OP_PAC:  c->ACC = c->Preg & MASK32; break;
    case OP_APAC:
        c->oldacc = c->ACC;
        c->ACC = (c->ACC + c->Preg) & MASK32;
        calc_add_overflow(c, c->Preg);
        break;
    case OP_SPAC:
        c->oldacc = c->ACC;
        c->ACC = (c->ACC - c->Preg) & MASK32;
        calc_sub_overflow(c, c->Preg);
        break;
    case OP_PUSH: push_stack(c, (uint16_t)(c->ACC & MASK16)); break;
    case OP_POP:  c->ACC = pop_stack(c) & MASK16; break;

    case OP_ILLEGAL:
    default:
        break; /* logged as illegal in MAME; silently ignored here */
    }

    cycles += base;
    if (c->branch_taken) {
        /* MAME's add_branch_cycle(): taken conditional branches (and BANZ)
         * cost their own base cycle count a second time. */
        cycles += base;
    }

    /* Forward-progress guard: illegal opcodes carry 0 cycles in MAME's
     * table, which would make tms_run()'s budget loop spin forever. The DSP
     * ROM never executes one (the Python reference, which would likewise
     * hang, runs clean), but a hard hang inside a screen reader is far
     * worse than a 1-cycle accounting difference in an already-broken
     * state, so guarantee progress here. */
    if (cycles <= 0) cycles = 1;
    return cycles;
}

int tms_run(tms32010_t *c, int cycles)
{
    int budget = cycles;
    if (c->in_reset) return 0;
    while (budget > 0)
        budget -= tms_step(c);
    return budget;
}
