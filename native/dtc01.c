/* DECtalk DTC-01 machine glue -- C port of emu/machine.py.
 *
 * Wires the vendored Musashi 68000, our TMS32010 DSP, our SCN2681 DUART,
 * and the FIFO/latch logic between the two CPUs into one runnable system,
 * replicating MAME's dectalk_state (DESIGN.md sections 1, 3, 4, 5).
 */
#include <stdlib.h>
#include <string.h>

#include "dtc01.h"
#include "tms32010.h"
#include "duart2681.h"
#include "musashi/m68k.h"

#define ROM_SIZE               0x040000u
#define RAM_BASE               0x080000u
#define RAM_SIZE               0x014000u
#define LED_NVRAM_BASE         0x094000u
#define LED_NVRAM_REGION_SIZE  0x000400u
#define DUART_BASE             0x098000u
#define DUART_REGION_SIZE      0x000020u
#define SPC_TLC_BASE           0x09C000u
#define SPC_TLC_REGION_SIZE    0x000008u

#define IRQ_TLC   4
#define IRQ_SPC   5
#define IRQ_DUART 6

#define INFIFO_DEPTH  32
#define OUTFIFO_DEPTH 16

#define DAC_SAMPLE_HZ 10000.0
#define M68K_HZ       10000000.0
#define DSP_CYCLE_HZ  5000000.0  /* matches MAME's (clocks+3)/4 on a 20MHz part */

/* Both clock ratios are exact integers, which is what lets the scheduler run
 * on integer counters instead of accumulating seconds in a double. */
#define M68K_PER_DSP  2          /* 10MHz / 5MHz  */
#define M68K_PER_DAC  1000       /* 10MHz / 10kHz */

/* Upper bound on how many 68000 cycles run before the DSP and DUART are
 * serviced. 1 reproduces the historical one-instruction-at-a-time schedule
 * exactly. Larger values amortise Musashi's per-call state save/restore at
 * the cost of a coarser interleave; override at build time to measure. */
/* 32 measured +82% with the DSP no more than ~3.2us (3% of a DAC period)
 * behind the 68000. Larger values keep paying: 128 gave +135% and 1000 gave
 * +171%, both with underruns, RMS, peak and envelope length unchanged -- but
 * they model the two processors as running concurrently ever more loosely,
 * and this firmware's 68000/DSP handshake is the part that took longest to
 * get right. 32 is the conservative end of the useful range. */
#ifndef M68K_BATCH_CYCLES
#define M68K_BATCH_CYCLES 32
#endif
typedef char dtc01_clock_ratios_are_exact[
    ((int)M68K_HZ == (int)DSP_CYCLE_HZ * M68K_PER_DSP &&
     (int)M68K_HZ == (int)DAC_SAMPLE_HZ * M68K_PER_DAC) ? 1 : -1];

#define PENDING_SIZE 65536
#define HOSTTX_SIZE  4096

struct dtc01 {
    uint8_t  rom[ROM_SIZE];
    uint8_t  ram[RAM_SIZE];
    uint8_t  nvram[0x100];
    int      led_state;

    tms32010_t dsp;
    scn2681_t  duart;

    uint16_t infifo[INFIFO_DEPTH];
    int      infifo_count, infifo_head, infifo_tail;
    uint16_t outfifo[OUTFIFO_DEPTH];
    int      outfifo_count, outfifo_head, outfifo_tail;

    int      infifo_semaphore;
    int      spc_error_latch;
    int      spc_flags_latch;   /* bit0 speech-init, bit6 spc-irq-enable */
    int      tlc_flags_latch;

    int      irq_lines[8];
    int      pending_irq;

    /* Scheduling is exact integer arithmetic in units of 68000 cycles: the
     * DSP runs at half the 68000 clock and the DAC emits one sample per
     * M68K_PER_DAC cycles, both exact ratios. Doing this in double cost a
     * division per emulated instruction (~1M/sec of audio) and bought
     * nothing -- these counters cannot drift. */
    uint64_t m68k_cycles;       /* total elapsed, for dtc01_time_seconds() */
    int      dsp_half_debt;     /* unconverted 68000 cycles, < 2 */
    int      dac_debt;          /* 68000 cycles since the last DAC sample */

    uint8_t  pending[PENDING_SIZE];   /* host text not yet in the DUART FIFO */
    int      pending_head, pending_tail, pending_count;

    uint8_t  hosttx[HOSTTX_SIZE];     /* bytes the firmware sent back */
    int      hosttx_head, hosttx_tail, hosttx_count;

    int      unmapped;
    int      outfifo_underruns; /* DAC ticks that found the DSP's fifo empty */
    int      dac_ticks;
    int      volume;   /* 0-100, applied to DAC output; see dtc01_set_volume */
    void    *m68k_ctx;
};

/* Factory-default X2212 NVRAM image. Decoded from the ROM's own embedded
 * copy at main-CPU address 0x1A7AE (DESIGN.md s10 verified this matches
 * byte-for-byte); a blank/zeroed NVRAM routes the firmware into an "NVR
 * FAULT" setup dead-end instead of normal operation.
 *
 * Decoded from v2.0. MAME's notes say v1.8 expects a different image, and
 * v1.8's copy is not at 0x1A7AE. It stays hardcoded anyway: v1.8 boots to
 * the same LED state and host prompt with this image and speaks correctly,
 * and the NVRAM was ruled out as the cause of its old audio bug (that was
 * the DAC-tick scheduling, DESIGN.md s21). Whether the config this decodes
 * to is *right* for v1.8 is still unverified. */
static const uint8_t DEFAULT_NVRAM_OFFSETS[] = {
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
    0x20, 0x24, 0x28, 0x2C, 0xFC, 0xFD, 0xFE, 0xFF
};
static const uint8_t DEFAULT_NVRAM_VALUES[] = {
    0x05, 0x00, 0x06, 0x01, 0x06, 0x0B, 0x02, 0x02,
    0x01, 0x01, 0x00, 0x01, 0x0D, 0x02, 0x05, 0x0B
};

/* Musashi keeps CPU state in globals, so exactly one machine is "active"
 * at a time and its context is swapped in on entry. See dtc01.h. */
static dtc01_t *g_active = NULL;

/* m68k_init() builds Musashi's opcode jump table; without it the handler
 * pointers are null and the first executed instruction faults. It is
 * process-global and idempotent-by-guard, not per-machine. */
static int g_m68k_inited = 0;
static void ensure_m68k_init(void)
{
    if (!g_m68k_inited) {
        m68k_init();
        g_m68k_inited = 1;
    }
}

static void set_irq(dtc01_t *m, int level, int asserted);

/* ---- SPC / FIFO glue (1:1 with dectalk_state, DESIGN.md s4) ---------- */
static void outfifo_check(dtc01_t *m)
{
    /* Matches the driver's outfifo_check(): only driven from the 10kHz
     * sample-pop path, not from every DSP fifo write. tms_set_int_line
     * only acts on assert (pending interrupts can't be cleared
     * externally), so the deassert call is an intentional no-op kept for
     * fidelity. */
    tms_set_int_line(&m->dsp, m->outfifo_count < OUTFIFO_DEPTH);
}

static void clear_all_fifos(dtc01_t *m)
{
    memset(m->outfifo, 0, sizeof(m->outfifo));
    memset(m->infifo, 0, sizeof(m->infifo));
    m->outfifo_count = m->outfifo_head = m->outfifo_tail = 0;
    m->infifo_count = m->infifo_head = m->infifo_tail = 0;
    outfifo_check(m);
}

static void dsp_semaphore_w(dtc01_t *m, int state)
{
    m->infifo_semaphore = state;
    set_irq(m, IRQ_SPC, state && (m->spc_flags_latch & 0x40));
}

static void m68k_infifo_w(dtc01_t *m, uint16_t data)
{
    if (m->infifo_count == INFIFO_DEPTH) return;
    m->infifo[m->infifo_head] = data;
    m->infifo_head = (m->infifo_head + 1) & (INFIFO_DEPTH - 1);
    m->infifo_count++;
}

static uint16_t m68k_spcflags_r(dtc01_t *m)
{
    uint16_t data = (uint16_t)m->spc_flags_latch;
    if (m->spc_error_latch)  data |= 0x20;
    if (m->infifo_semaphore) data |= 0x80;
    return data;
}

static void m68k_spcflags_w(dtc01_t *m, uint16_t data)
{
    m->spc_flags_latch = data & 0x41;
    if (data & 0x01) {
        clear_all_fifos(m);
        tms_set_reset_line(&m->dsp, 1);
        m->spc_error_latch = 0;
        dsp_semaphore_w(m, 0);
    } else {
        tms_set_reset_line(&m->dsp, 0);
    }
    if (data & 0x02) {
        m->spc_error_latch = 0;
        dsp_semaphore_w(m, 0);
    }
    if (data & 0x40) {
        if (m->infifo_semaphore) set_irq(m, IRQ_SPC, 1);
    } else {
        set_irq(m, IRQ_SPC, 0);
    }
}

static void m68k_tlcflags_w(dtc01_t *m, uint16_t data)
{
    m->tlc_flags_latch = data & 0x4140;
    set_irq(m, IRQ_TLC, 0); /* telephone hardware not modeled -- never fires */
}

static uint16_t dsp_pop_outfifo(dtc01_t *m)
{
    uint16_t data = m->outfifo[m->outfifo_tail];
    m->dac_ticks++;
    /* An empty fifo makes the DAC hold its previous value. That is correct
     * hardware behaviour, and normal while the DSP is held in reset between
     * utterances -- it just yields silence. But if it happens while the DSP
     * is *running*, the emulator is not feeding the 10kHz DAC fast enough
     * and samples get repeated, which is audible as grainy, stretched
     * speech even though the audio reaches the device perfectly on time. */
    if (m->outfifo_count == 0 && !m->dsp.in_reset) {
        /* Only count it when the value being held is actually part of a
         * waveform. Between utterances the DSP runs but emits nothing, so
         * the fifo is legitimately empty and the DAC holds silence -- that
         * is not a defect and would otherwise swamp the measurement. */
        int16_t held = (int16_t)(uint16_t)(data & 0xFFF0);
        if (held > 120 || held < -120)
            m->outfifo_underruns++;
    }
    if (m->outfifo_count > 0) {
        m->outfifo_tail = (m->outfifo_tail + 1) & (OUTFIFO_DEPTH - 1);
        m->outfifo_count--;
    }
    outfifo_check(m);
    /* Repeats the last value when empty -- what real hardware does. */
    return (uint16_t)(((data & 0xFFF0) ^ 0x8000) & 0xFFFF);
}

/* ---- DSP callbacks ---------------------------------------------------- */
static uint16_t dsp_io_read(void *owner, int port)
{
    dtc01_t *m = (dtc01_t *)owner;
    if (port == 1) {
        uint16_t data = m->infifo[m->infifo_tail];
        if (m->infifo_count > 0) {
            m->infifo_tail = (m->infifo_tail + 1) & (INFIFO_DEPTH - 1);
            m->infifo_count--;
        }
        return data;
    }
    return 0xFFFF;
}

static void dsp_io_write(void *owner, int port, uint16_t value)
{
    dtc01_t *m = (dtc01_t *)owner;
    if (port == 0) {
        dsp_semaphore_w(m, 1);
        m->spc_error_latch = value & 1;
    } else if (port == 1) {
        tms_set_int_line(&m->dsp, 0); /* inert no-op, kept for fidelity */
        if (m->outfifo_count != OUTFIFO_DEPTH) {
            m->outfifo[m->outfifo_head] = value;
            m->outfifo_head = (m->outfifo_head + 1) & (OUTFIFO_DEPTH - 1);
            m->outfifo_count++;
        }
    }
}

static int dsp_bio_read(void *owner)
{
    dtc01_t *m = (dtc01_t *)owner;
    return m->infifo_semaphore ? 1 : 0;
}

/* ---- DUART callbacks -------------------------------------------------- */
static void duart_tx_b(void *owner, uint8_t byte)
{
    dtc01_t *m = (dtc01_t *)owner;
    if (m->hosttx_count >= HOSTTX_SIZE) { /* drop oldest to stay live */
        m->hosttx_tail = (m->hosttx_tail + 1) % HOSTTX_SIZE;
        m->hosttx_count--;
    }
    m->hosttx[m->hosttx_head] = byte;
    m->hosttx_head = (m->hosttx_head + 1) % HOSTTX_SIZE;
    m->hosttx_count++;
}

static void duart_irq(void *owner, int active)
{
    set_irq((dtc01_t *)owner, IRQ_DUART, active);
}

/* ---- interrupts ------------------------------------------------------- */
/* Our IRQ lines are level-triggered: a device holds its line asserted
 * until the firmware services it. Musashi, with M68K_EMULATE_INT_ACK off,
 * auto-clears its own CPU_INT_LEVEL as soon as it takes an interrupt
 * (m68kcpu.h "Automatically clear IRQ if we are not using an acknowledge
 * scheme") and expects the board to re-assert. So tracking the level here
 * and only notifying Musashi on *change* is not enough -- it delivers
 * exactly one interrupt and then goes deaf. The run loop therefore
 * re-asserts pending_irq every instruction; this function just maintains
 * the level. */
static void set_irq(dtc01_t *m, int level, int asserted)
{
    int lvl, i;
    m->irq_lines[level] = asserted;
    lvl = 0;
    for (i = IRQ_TLC; i <= IRQ_DUART; i++)
        if (m->irq_lines[i] && i > lvl) lvl = i;
    m->pending_irq = lvl;
    if (g_active == m) m68k_set_irq((unsigned int)lvl);
}

/* ---- memory map ------------------------------------------------------- */
static uint16_t spc_tlc_read16(dtc01_t *m, uint32_t off)
{
    switch (off & 0x7) {
    case 0: return m68k_spcflags_r(m);
    case 4: return (uint16_t)m->tlc_flags_latch; /* tone/ring unimplemented */
    case 6: return 0;
    default: return 0xFFFF;
    }
}

static void spc_tlc_write16(dtc01_t *m, uint32_t off, uint16_t value)
{
    switch (off & 0x7) {
    case 0: m68k_spcflags_w(m, value); break;
    case 2: m68k_infifo_w(m, value); break;
    case 4: m68k_tlcflags_w(m, value); break;
    default: break;
    }
}

static uint32_t bus_read8(dtc01_t *m, uint32_t addr)
{
    uint32_t off;
    if (addr < ROM_SIZE) return m->rom[addr];
    off = addr - RAM_BASE;
    if (off < RAM_SIZE) return m->ram[off];
    if (addr >= LED_NVRAM_BASE && addr < LED_NVRAM_BASE + LED_NVRAM_REGION_SIZE) {
        uint32_t o = addr - LED_NVRAM_BASE;
        if (o < 0x200 && !(addr & 1)) return m->nvram[(o / 2) & 0xFF];
        return 0xFF;
    }
    if (addr >= DUART_BASE && addr < DUART_BASE + DUART_REGION_SIZE)
        return duart_read(&m->duart, (int)(addr - DUART_BASE));
    if (addr >= SPC_TLC_BASE && addr < SPC_TLC_BASE + SPC_TLC_REGION_SIZE) {
        uint32_t o = addr - SPC_TLC_BASE;
        uint16_t word = spc_tlc_read16(m, o & ~1u);
        /* byte access into a word-only register block */
        return ((o & 1) == 0) ? ((word >> 8) & 0xFF) : (word & 0xFF);
    }
    m->unmapped++;
    return 0xFF;
}

static void bus_write8(dtc01_t *m, uint32_t addr, uint8_t value)
{
    uint32_t off;
    if (addr < ROM_SIZE) return; /* ROM is read-only; ignore stray writes */
    off = addr - RAM_BASE;
    if (off < RAM_SIZE) { m->ram[off] = value; return; }
    if (addr >= LED_NVRAM_BASE && addr < LED_NVRAM_BASE + LED_NVRAM_REGION_SIZE) {
        uint32_t o = addr - LED_NVRAM_BASE;
        if (o < 0x200) {
            if (addr & 1) m->led_state = value;
            else          m->nvram[(o / 2) & 0xFF] = value;
        }
        /* else: NVRAM recall/store trigger region -- persistence not
         * modeled (DESIGN.md s8), the buffer is already current. */
        return;
    }
    if (addr >= DUART_BASE && addr < DUART_BASE + DUART_REGION_SIZE) {
        duart_write(&m->duart, (int)(addr - DUART_BASE), value);
        return;
    }
    if (addr >= SPC_TLC_BASE && addr < SPC_TLC_BASE + SPC_TLC_REGION_SIZE)
        return; /* word-only on real hardware; ignore stray byte writes */
    m->unmapped++;
}

static uint32_t bus_read16(dtc01_t *m, uint32_t addr)
{
    uint32_t off;
    if (addr + 1 < ROM_SIZE) return ((uint32_t)m->rom[addr] << 8) | m->rom[addr + 1];
    off = addr - RAM_BASE;
    if (off + 1 < RAM_SIZE) return ((uint32_t)m->ram[off] << 8) | m->ram[off + 1];
    if (addr >= SPC_TLC_BASE && addr < SPC_TLC_BASE + SPC_TLC_REGION_SIZE)
        return spc_tlc_read16(m, addr - SPC_TLC_BASE);
    return (bus_read8(m, addr) << 8) | bus_read8(m, addr + 1);
}

static void bus_write16(dtc01_t *m, uint32_t addr, uint16_t value)
{
    uint32_t off;
    if (addr + 1 < ROM_SIZE) return;
    off = addr - RAM_BASE;
    if (off + 1 < RAM_SIZE) {
        m->ram[off]     = (uint8_t)(value >> 8);
        m->ram[off + 1] = (uint8_t)(value & 0xFF);
        return;
    }
    if (addr >= SPC_TLC_BASE && addr < SPC_TLC_BASE + SPC_TLC_REGION_SIZE) {
        spc_tlc_write16(m, addr - SPC_TLC_BASE, value);
        return;
    }
    bus_write8(m, addr, (uint8_t)(value >> 8));
    bus_write8(m, addr + 1, (uint8_t)(value & 0xFF));
}

/* ---- Musashi memory callbacks ---------------------------------------- */
#define ADDR24(a) ((a) & 0x00FFFFFFu)

unsigned int m68k_read_memory_8(unsigned int address)
{
    return g_active ? bus_read8(g_active, ADDR24(address)) : 0;
}
unsigned int m68k_read_memory_16(unsigned int address)
{
    return g_active ? bus_read16(g_active, ADDR24(address)) : 0;
}
unsigned int m68k_read_memory_32(unsigned int address)
{
    uint32_t a = ADDR24(address);
    if (!g_active) return 0;
    return (bus_read16(g_active, a) << 16) | bus_read16(g_active, ADDR24(a + 2));
}
void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    if (g_active) bus_write8(g_active, ADDR24(address), (uint8_t)value);
}
void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    if (g_active) bus_write16(g_active, ADDR24(address), (uint16_t)value);
}
void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    uint32_t a = ADDR24(address);
    if (!g_active) return;
    bus_write16(g_active, a, (uint16_t)(value >> 16));
    bus_write16(g_active, ADDR24(a + 2), (uint16_t)(value & 0xFFFF));
}

/* ---- context activation ---------------------------------------------- */
static void activate(dtc01_t *m)
{
    if (g_active == m) return;
    if (g_active) m68k_get_context(g_active->m68k_ctx);
    m68k_set_context(m->m68k_ctx);
    g_active = m;
    m68k_set_irq((unsigned int)m->pending_irq);
}

/* ---- host text plumbing ---------------------------------------------- */
static void drain_pending_text(dtc01_t *m)
{
    while (m->pending_count > 0) {
        uint8_t byte = m->pending[m->pending_tail];
        if (duart_feed_rx_b(&m->duart, &byte, 1) != 1) break;
        m->pending_tail = (m->pending_tail + 1) % PENDING_SIZE;
        m->pending_count--;
    }
}

/* ---- lifecycle -------------------------------------------------------- */
static void machine_soft_state_init(dtc01_t *m)
{
    size_t i;
    memset(m->nvram, 0, sizeof(m->nvram));
    for (i = 0; i < sizeof(DEFAULT_NVRAM_OFFSETS); i++)
        m->nvram[DEFAULT_NVRAM_OFFSETS[i]] = DEFAULT_NVRAM_VALUES[i];

    m->led_state = 0;
    memset(m->infifo, 0, sizeof(m->infifo));
    memset(m->outfifo, 0, sizeof(m->outfifo));
    m->infifo_count = m->infifo_head = m->infifo_tail = 0;
    m->outfifo_count = m->outfifo_head = m->outfifo_tail = 0;
    m->infifo_semaphore = 0;
    m->spc_error_latch = 0;
    m->spc_flags_latch = 0;
    m->tlc_flags_latch = 0;
    memset(m->irq_lines, 0, sizeof(m->irq_lines));
    m->pending_irq = 0;
    m->m68k_cycles = 0;
    m->dsp_half_debt = 0;
    m->dac_debt = 0;
    m->pending_head = m->pending_tail = m->pending_count = 0;
    m->hosttx_head = m->hosttx_tail = m->hosttx_count = 0;
    m->unmapped = 0;
}

DTC01_API dtc01_t *dtc01_create(const uint8_t *main_rom, int main_rom_len,
                                const uint16_t *dsp_rom, int dsp_rom_words)
{
    dtc01_t *m;
    if (!main_rom || !dsp_rom) return NULL;
    if (main_rom_len < (int)ROM_SIZE) return NULL;
    if (dsp_rom_words <= 0 || dsp_rom_words > TMS_PROGRAM_SIZE) return NULL;

    m = (dtc01_t *)calloc(1, sizeof(dtc01_t));
    if (!m) return NULL;

    m->m68k_ctx = calloc(1, m68k_context_size());
    if (!m->m68k_ctx) { free(m); return NULL; }

    memcpy(m->rom, main_rom, ROM_SIZE);
    machine_soft_state_init(m);
    /* Set outside machine_soft_state_init so dtc01_reset() doesn't discard
     * the user's volume along with the machine state. */
    m->volume = 100;

    tms_init(&m->dsp, dsp_rom, dsp_rom_words, m, dsp_io_read, dsp_io_write, dsp_bio_read);
    duart_init(&m->duart, m, duart_tx_b, duart_irq);
    /* Always-connected virtual link: assert CTS/DSR so the firmware's
     * modem-style connect state machine passes straight through instead of
     * waiting on a real modem handshake (DESIGN.md s1). */
    duart_set_input_bit(&m->duart, 0, 1); /* CTS */
    duart_set_input_bit(&m->duart, 2, 1); /* DSR */

    /* Bring up the 68000 in this machine's context. pulse_reset reads the
     * reset vector through the memory callbacks, so g_active must already
     * point at us. */
    if (g_active) m68k_get_context(g_active->m68k_ctx);
    g_active = m;
    ensure_m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_irq(0);
    m68k_get_context(m->m68k_ctx);

    return m;
}

DTC01_API void dtc01_destroy(dtc01_t *m)
{
    if (!m) return;
    if (g_active == m) g_active = NULL;
    free(m->m68k_ctx);
    free(m);
}

DTC01_API void dtc01_reset(dtc01_t *m)
{
    if (!m) return;
    machine_soft_state_init(m);
    duart_reset(&m->duart);
    duart_set_input_bit(&m->duart, 0, 1);
    duart_set_input_bit(&m->duart, 2, 1);
    tms_set_reset_line(&m->dsp, 1);

    if (g_active && g_active != m) m68k_get_context(g_active->m68k_ctx);
    g_active = m;
    ensure_m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    m68k_set_irq(0);
    m68k_get_context(m->m68k_ctx);
}

/* ---- public entry points ---------------------------------------------- */
DTC01_API int dtc01_feed_text(dtc01_t *m, const uint8_t *data, int len)
{
    int i;
    if (!m || !data || len < 0) return 0;
    for (i = 0; i < len; i++) {
        if (m->pending_count >= PENDING_SIZE) break;
        m->pending[m->pending_head] = data[i];
        m->pending_head = (m->pending_head + 1) % PENDING_SIZE;
        m->pending_count++;
    }
    /* Push straight through where possible so short strings behave exactly
     * like the Python reference's immediate FIFO fill. */
    drain_pending_text(m);
    return i;
}

DTC01_API int dtc01_run_samples(dtc01_t *m, int16_t *out, int max_samples)
{
    int produced = 0;

    if (!m || !out || max_samples <= 0) return 0;
    activate(m);

    while (produced < max_samples) {
        int cycles, spent, budget;

        if (m->pending_count > 0) drain_pending_text(m);

        /* Re-assert the (level-triggered) IRQ line -- Musashi clears its
         * own copy each time it takes an interrupt. See set_irq(). */
        if (m->pending_irq) m68k_set_irq((unsigned int)m->pending_irq);

        /* Musashi saves and restores its whole CPU state around every
         * m68k_execute() call, so asking for one instruction at a time pays
         * that overhead ~1M times per second of audio. Batching amortises it.
         *
         * The batch is capped at the cycles remaining before the next DAC
         * sample, which keeps the 10kHz DAC exactly periodic: m68k_execute()
         * runs until the budget is met and then finishes the instruction in
         * progress, which is the same instruction that would have crossed the
         * boundary one-at-a-time. What does change is how coarsely the DSP
         * and DUART are interleaved with the 68000 -- hence the tunable. */
        budget = M68K_PER_DAC - m->dac_debt;
        if (budget > M68K_BATCH_CYCLES) budget = M68K_BATCH_CYCLES;
        if (budget < 1) budget = 1;
        cycles = m68k_execute(budget);
        if (cycles <= 0) cycles = 1;
        m->m68k_cycles += (uint64_t)cycles;

        /* Still seconds, still a division: measured at ~1% of throughput,
         * inside this machine's noise floor. Converting the DUART to
         * fixed-point would buy nothing and would disturb the counter/timer
         * whose behaviour is the reason speech works at all. */
        duart_step(&m->duart, (double)cycles / M68K_HZ);

        /* DSP: exactly one cycle per M68K_PER_DSP 68000 cycles. tms_run
         * returns the unspent budget (<= 0, an overshoot); as before it
         * replaces the debt rather than adding to it.
         *
         * The cycles are handed over in chunks that stop at each DAC-tick
         * boundary, so the DSP never runs *past* a tick before the tick is
         * delivered. Capping `budget` above is not enough: m68k_execute()
         * finishes the instruction in progress, so `cycles` overshoots the
         * boundary by most of an instruction. The DAC tick is what asserts
         * the DSP's outfifo interrupt, and v1.8's DSP times out if that
         * interrupt is late -- it has only ~146 cycles of slack (DESIGN.md
         * s21) -- whereupon it raises a soft error, the firmware takes its
         * resend path, and the audio shreds. MAME gets this from
         * `config.m_perfect_cpu_quantum = subtag("dsp")`. v2.0's DSP has a
         * laxer wait and is bit-identical either way. */
        {
            int remaining = cycles;
            while (remaining > 0) {
                int chunk = remaining;
                if (m->dac_debt < M68K_PER_DAC) {
                    int until_tick = M68K_PER_DAC - m->dac_debt;
                    if (until_tick < chunk) chunk = until_tick;
                }

                m->dsp_half_debt += chunk;
                if (!m->dsp.in_reset) {
                    spent = m->dsp_half_debt / M68K_PER_DSP;
                    if (spent > 0)
                        m->dsp_half_debt = tms_run(&m->dsp, spent) * M68K_PER_DSP;
                }

                m->dac_debt += chunk;
                remaining -= chunk;

                /* If the caller's buffer is full the tick stays owed in
                 * dac_debt and is drained on the next call -- the guard above
                 * then stops splitting, so this cannot spin. */
                while (m->dac_debt >= M68K_PER_DAC && produced < max_samples) {
                    uint16_t raw;
                    int16_t pcm;
                    m->dac_debt -= M68K_PER_DAC;
                    raw = dsp_pop_outfifo(m);
                    /* raw is the offset-binary DAC word; ^0x8000 recovers
                     * signed PCM (see dtc01.h). */
                    pcm = (int16_t)(uint16_t)(raw ^ 0x8000);
                    if (m->volume < 100)
                        pcm = (int16_t)(((int32_t)pcm * m->volume) / 100);
                    out[produced++] = pcm;
                }
            }
        }
    }
    return produced;
}

DTC01_API int dtc01_read_host_tx(dtc01_t *m, uint8_t *out, int max_len)
{
    int n = 0;
    if (!m || !out || max_len <= 0) return 0;
    while (n < max_len && m->hosttx_count > 0) {
        out[n++] = m->hosttx[m->hosttx_tail];
        m->hosttx_tail = (m->hosttx_tail + 1) % HOSTTX_SIZE;
        m->hosttx_count--;
    }
    return n;
}

DTC01_API int dtc01_is_idle(const dtc01_t *m)
{
    if (!m) return 1;
    return (m->infifo_count == 0 && m->outfifo_count == 0 &&
            m->pending_count == 0 && duart_rx_b_pending(&m->duart) == 0) ? 1 : 0;
}

/* The firmware parks the DSP in reset between utterances, so this is its own
 * answer to "am I still synthesizing?" -- unlike an absence of audio, which
 * is also true during a pause inside an utterance. */
DTC01_API int dtc01_dsp_active(const dtc01_t *m)       { return m ? !m->dsp.in_reset : 0; }
DTC01_API int dtc01_get_led(const dtc01_t *m)          { return m ? m->led_state : 0; }
DTC01_API int dtc01_infifo_count(const dtc01_t *m)     { return m ? m->infifo_count : 0; }
DTC01_API int dtc01_outfifo_count(const dtc01_t *m)    { return m ? m->outfifo_count : 0; }
DTC01_API int dtc01_pending_text(const dtc01_t *m)     { return m ? m->pending_count : 0; }
DTC01_API int dtc01_unmapped_accesses(const dtc01_t *m){ return m ? m->unmapped : 0; }
DTC01_API int dtc01_outfifo_underruns(const dtc01_t *m){ return m ? m->outfifo_underruns : 0; }
DTC01_API int dtc01_dac_ticks(const dtc01_t *m){ return m ? m->dac_ticks : 0; }
/* Derived on demand -- the scheduler counts cycles, not seconds. */
DTC01_API double dtc01_time_seconds(const dtc01_t *m)  { return m ? (double)m->m68k_cycles / M68K_HZ : 0.0; }

DTC01_API int dtc01_read_ram32(const dtc01_t *m, uint32_t addr, uint32_t *out)
{
    uint32_t off;
    if (!m || !out) return 0;
    off = addr - RAM_BASE;
    if (off + 3 >= RAM_SIZE) return 0;
    *out = ((uint32_t)m->ram[off] << 24) | ((uint32_t)m->ram[off + 1] << 16) |
           ((uint32_t)m->ram[off + 2] << 8) | m->ram[off + 3];
    return 1;
}

DTC01_API void dtc01_set_volume(dtc01_t *m, int percent)
{
    if (!m) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    m->volume = percent;
}

DTC01_API int dtc01_get_volume(const dtc01_t *m) { return m ? m->volume : 0; }

DTC01_API void dtc01_debug_duart(const dtc01_t *m, int *running, int *remaining,
                                 int *ready, int *acr, int *imr, double *clock_hz)
{
    if (!m) return;
    if (running)   *running   = m->duart.counter_running;
    if (remaining) *remaining = (int)m->duart.counter_remaining;
    if (ready)     *ready     = m->duart.counter_ready;
    if (acr)       *acr       = m->duart.acr;
    if (imr)       *imr       = m->duart.imr;
    if (clock_hz)  *clock_hz  = m->duart.counter_clock_cache;
}

DTC01_API const char *dtc01_version(void)
{
    return "dtc01-native 1.0 (Musashi 4.60 68000 + TMS32010 + SCN2681)";
}
