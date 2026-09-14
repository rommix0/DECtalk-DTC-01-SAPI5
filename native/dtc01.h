/* DECtalk DTC-01 machine -- public C API, designed to be called through
 * ctypes from the NVDA synth driver.
 *
 * Deliberately a flat C ABI (no CPython headers): NVDA ships its own
 * Python and its version moves independently of the dev environment, so
 * binding via ctypes to a plain DLL avoids per-Python-version builds
 * entirely. The Python<->C boundary is crossed only once per audio chunk,
 * so ctypes' higher per-call overhead is irrelevant.
 *
 * Threading: the vendored Musashi core keeps its CPU state in globals, so
 * this layer swaps the 68000 context per handle on entry (see dtc01.c).
 * That makes multiple handles correct but NOT concurrently usable from
 * several threads. Use one handle at a time per process.
 */
#ifndef DTC01_H
#define DTC01_H

#include <stdint.h>

#if defined(_WIN32)
#  define DTC01_API __declspec(dllexport)
#else
#  define DTC01_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtc01 dtc01_t;

/* main_rom: 0x40000 bytes, big-endian word image (DESIGN.md s2).
 * dsp_rom:  0x800 words (uint16), the interleaved DSP program image.
 * Returns NULL on allocation failure or bad sizes. */
DTC01_API dtc01_t *dtc01_create(const uint8_t *main_rom, int main_rom_len,
                                const uint16_t *dsp_rom, int dsp_rom_words);
DTC01_API void dtc01_destroy(dtc01_t *m);
DTC01_API void dtc01_reset(dtc01_t *m);

/* State snapshots: rewind a machine instantly instead of re-running the
 * firmware from reset. The SAPI engine saves one right after the power-on
 * announcement and restores it before every utterance, so abandoning an
 * interrupted utterance costs a memcpy rather than a replay of the boot
 * (4-8 s of emulated time, which is what made cancelled speech slow).
 *
 * A snapshot holds everything that changes as the machine runs -- 68000,
 * DSP, DUART, RAM, NVRAM, the FIFOs and host queues -- but not the ROM
 * images, the volume, or the diagnostic counters, which stay the target's.
 * It restores into any machine created from the same ROM images by this same
 * loaded copy of the DLL (Musashi's context holds pointers into it); anything
 * else is rejected and the target is left untouched. In-memory only: this is
 * not a file format.
 *
 * dtc01_state_save returns bytes written, or 0 if len is smaller than
 * dtc01_state_size(). dtc01_state_restore returns 1, or 0 if rejected. */
DTC01_API int dtc01_state_size(void);
DTC01_API int dtc01_state_save(dtc01_t *m, uint8_t *buf, int len);
DTC01_API int dtc01_state_restore(dtc01_t *m, const uint8_t *buf, int len);

/* Queue host text for the DUART channel-B (host RS-232) link. Bytes are
 * buffered internally and trickled into the DUART as its receive FIFO
 * drains, so arbitrarily long text is safe. Returns bytes queued (short
 * only if the internal buffer is full). */
DTC01_API int dtc01_feed_text(dtc01_t *m, const uint8_t *data, int len);

/* Run the machine until `max_samples` DAC samples have been produced,
 * writing signed 16-bit PCM into `out`. Returns samples written.
 *
 * The samples are ready-to-play signed PCM: the hardware DAC word
 * ((data & 0xfff0) ^ 0x8000) is offset-binary, and converting it back to
 * signed is another ^0x8000, so the two cancel to (int16_t)(data & 0xfff0).
 * Doing that here means callers cannot repeat the DC-offset bug that made
 * early output rail-pinned and ~30dB too loud (DESIGN.md s12). */
DTC01_API int dtc01_run_samples(dtc01_t *m, int16_t *out, int max_samples);

/* Drain bytes the firmware transmitted back on the host link. */
DTC01_API int dtc01_read_host_tx(dtc01_t *m, uint8_t *out, int max_len);

/* Speech-pipeline idle: both SPC FIFOs empty and no host text still
 * queued. This is the ground-truth signal the synthetic-indexing design
 * polls to decide a chunk has been fully spoken (DESIGN.md s7). */
DTC01_API int dtc01_is_idle(const dtc01_t *m);

/* Nothing queued for the firmware to say: no host text waiting, in our queue
 * or the DUART's, and no frames waiting for the DSP. Unlike dtc01_is_idle
 * this ignores the output FIFO, which some voices' DSP programs keep topped
 * up with silence after speaking (Whispery Wendy on v2.0) -- enough on its
 * own to hold dtc01_is_idle false indefinitely. Pair it with the audio
 * itself being silent. */
DTC01_API int dtc01_input_idle(const dtc01_t *m);

/* Output volume, 0-100 (default 100), applied as gain on the DAC samples.
 * The DTC-01 firmware has no volume command -- the real unit had a physical
 * volume knob on the box (Owner's Manual: "a built-in loudspeaker and
 * volume control") -- so this emulates that potentiometer at the same point
 * in the signal path. Done in C because Python 3.13 (which NVDA ships)
 * removed audioop, and per-sample scaling in Python would be wasteful. */
DTC01_API void dtc01_set_volume(dtc01_t *m, int percent);
DTC01_API int  dtc01_get_volume(const dtc01_t *m);

/* Introspection, for tests/diagnostics. */
DTC01_API int dtc01_get_led(const dtc01_t *m);
/* Non-zero while the firmware is synthesizing. It parks the DSP in reset
 * between utterances, so unlike silence this distinguishes "finished" from
 * "pausing mid-utterance". */
DTC01_API int dtc01_dsp_active(const dtc01_t *m);
DTC01_API int dtc01_infifo_count(const dtc01_t *m);
DTC01_API int dtc01_outfifo_count(const dtc01_t *m);
DTC01_API int dtc01_pending_text(const dtc01_t *m);
/* Count of accesses to addresses outside the documented memory map. The
 * firmware never does this on the known-good boot+speak path (verified
 * against the Python reference), so a nonzero value means divergence. */
DTC01_API int dtc01_unmapped_accesses(const dtc01_t *m);
/* DAC ticks that found the DSP output fifo empty while the DSP was running,
 * i.e. samples the emulator had to repeat. Audible as grainy/stretched
 * speech. Should stay at (or very near) zero. */
DTC01_API int dtc01_outfifo_underruns(const dtc01_t *m);
DTC01_API int dtc01_dac_ticks(const dtc01_t *m);
DTC01_API double dtc01_time_seconds(const dtc01_t *m);
DTC01_API int dtc01_read_ram32(const dtc01_t *m, uint32_t addr, uint32_t *out);
DTC01_API void dtc01_debug_duart(const dtc01_t *m, int *running, int *remaining,
                                 int *ready, int *acr, int *imr, double *clock_hz);
DTC01_API const char *dtc01_version(void);

#ifdef __cplusplus
}
#endif
#endif /* DTC01_H */
