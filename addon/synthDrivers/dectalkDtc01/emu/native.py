"""ctypes binding to the native DTC-01 emulator DLL (native/dtc01.c).

Why ctypes and a plain DLL rather than a CPython extension: NVDA ships its
own Python and its version moves independently of whatever Python this was
developed against, so a CPython-ABI extension would need rebuilding per
NVDA Python version. A flat C DLL sidesteps that entirely. The
Python<->C boundary is crossed once per audio chunk (not per instruction),
so ctypes' higher per-call overhead is irrelevant.

The pure-Python emulator in this package remains the readable reference
implementation and the correctness oracle; this is the one that's fast
enough to actually drive a screen reader (see DESIGN.md §13-14).
"""

from __future__ import annotations

import ctypes
import platform
import struct
import sys
import threading
from pathlib import Path

from . import rom_loader

# The vendored Musashi core keeps 68000 state in process globals, so the DLL
# can only ever be executing one machine at a time; it swaps each handle's
# CPU context in on entry. Two threads touching *different* machines
# concurrently corrupts that state and crashes the process (observed:
# Windows 0xC00000FF). The driver is careful to drive everything from its
# single worker thread, but a crash inside a screen reader is a bad failure
# mode to leave one mistake away, so every entry point that can execute the
# CPU or poke shared state serialises on this lock. Uncontended in normal
# use, and cheap next to a 100ms block of emulation.
_EXEC_LOCK = threading.RLock()


class NativeUnavailable(RuntimeError):
    """Raised when no DLL matching this interpreter's architecture is found."""


def _arch_tag() -> str:
    machine = platform.machine().lower()
    bits = struct.calcsize("P") * 8
    # NB: on Windows-on-ARM, an x64-emulated interpreter reports
    # PROCESSOR_ARCHITECTURE=AMD64 while platform.machine() may still say
    # ARM64, so trust the pointer size for the 32/64 split and only treat
    # this as arm64 when the interpreter itself is genuinely ARM64.
    if machine in ("arm64", "aarch64") and sys.version.lower().find("arm64") >= 0:
        return "arm64"
    return "x64" if bits == 64 else "x86"


def _find_dll(explicit: str | None = None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            raise NativeUnavailable(f"DLL not found: {p}")
        return p

    tag = _arch_tag()
    name = f"dtc01_{tag}.dll"
    here = Path(__file__).resolve().parent
    candidates = [
        here / name,                       # shipped alongside the addon
        here / "lib" / name,
        here.parents[3] / "build" / name,  # repo build/ during development
    ]
    for c in candidates:
        if c.is_file():
            return c
    raise NativeUnavailable(
        f"no {name} found (looked in: {', '.join(str(c) for c in candidates)}). "
        f"Build it with tools/build_native.bat {tag}."
    )


class NativeMachine:
    """Mirrors the useful surface of emu.machine.DectalkMachine, but pulls
    audio in blocks instead of via a per-sample callback."""

    def __init__(self, rom_dir: str, dll_path: str | None = None,
                 rom_version: str | None = None):
        self._dll_path = _find_dll(dll_path)
        self.rom_version = rom_loader.resolve_version(rom_version)
        lib = ctypes.CDLL(str(self._dll_path))
        self._lib = lib

        lib.dtc01_create.restype = ctypes.c_void_p
        lib.dtc01_create.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int,
                                     ctypes.POINTER(ctypes.c_uint16), ctypes.c_int]
        lib.dtc01_destroy.restype = None
        lib.dtc01_destroy.argtypes = [ctypes.c_void_p]
        lib.dtc01_reset.restype = None
        lib.dtc01_reset.argtypes = [ctypes.c_void_p]
        lib.dtc01_feed_text.restype = ctypes.c_int
        lib.dtc01_feed_text.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
        lib.dtc01_run_samples.restype = ctypes.c_int
        lib.dtc01_run_samples.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int]
        lib.dtc01_read_host_tx.restype = ctypes.c_int
        lib.dtc01_read_host_tx.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
        lib.dtc01_is_idle.restype = ctypes.c_int
        lib.dtc01_is_idle.argtypes = [ctypes.c_void_p]
        for name in ("dtc01_get_led", "dtc01_infifo_count", "dtc01_outfifo_count",
                     "dtc01_pending_text", "dtc01_unmapped_accesses"):
            fn = getattr(lib, name)
            fn.restype = ctypes.c_int
            fn.argtypes = [ctypes.c_void_p]
        lib.dtc01_set_volume.restype = None
        lib.dtc01_set_volume.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dtc01_get_volume.restype = ctypes.c_int
        lib.dtc01_get_volume.argtypes = [ctypes.c_void_p]
        lib.dtc01_time_seconds.restype = ctypes.c_double
        lib.dtc01_time_seconds.argtypes = [ctypes.c_void_p]
        lib.dtc01_read_ram32.restype = ctypes.c_int
        lib.dtc01_read_ram32.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        lib.dtc01_version.restype = ctypes.c_char_p
        lib.dtc01_version.argtypes = []

        main_image, dsp = rom_loader.build_images(rom_dir, self.rom_version)

        main_buf = (ctypes.c_uint8 * len(main_image)).from_buffer_copy(main_image)
        dsp_buf = (ctypes.c_uint16 * len(dsp))(*dsp)

        # Keep the ROM buffers alive for the DLL's lifetime even though it
        # copies them, so a future zero-copy change can't silently break.
        self._main_buf, self._dsp_buf = main_buf, dsp_buf

        with _EXEC_LOCK:
            handle = lib.dtc01_create(main_buf, len(main_image), dsp_buf, len(dsp))
        if not handle:
            raise RuntimeError("dtc01_create failed (bad ROM sizes or out of memory)")
        self._h = ctypes.c_void_p(handle)

        self._sample_buf = (ctypes.c_int16 * 8192)()
        self._tx_buf = (ctypes.c_uint8 * 1024)()

    # -- lifecycle -------------------------------------------------------
    def close(self) -> None:
        if getattr(self, "_h", None):
            with _EXEC_LOCK:
                self._lib.dtc01_destroy(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def reset(self) -> None:
        with _EXEC_LOCK:
            self._lib.dtc01_reset(self._h)

    # -- I/O ---------------------------------------------------------------
    def feed_text(self, data: bytes) -> int:
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        with _EXEC_LOCK:
            return self._lib.dtc01_feed_text(self._h, buf, len(data))

    def run_samples(self, count: int) -> list[int]:
        """Run until `count` signed-16-bit PCM samples are produced."""
        out: list[int] = []
        buf = self._sample_buf
        chunk_max = len(buf)
        while len(out) < count:
            want = min(chunk_max, count - len(out))
            with _EXEC_LOCK:
                got = self._lib.dtc01_run_samples(self._h, buf, want)
            if got <= 0:
                break
            out.extend(buf[:got])
        return out

    def run_seconds(self, seconds: float) -> list[int]:
        return self.run_samples(int(round(seconds * 10000)))

    def run_block(self, count: int) -> bytes:
        """Run up to `count` samples and return them as raw little-endian
        signed-16-bit PCM, ready to hand straight to nvwave.WavePlayer.

        This is the driver's hot path, so it deliberately avoids building a
        Python list: the bytes come straight out of the ctypes buffer. (It
        also has to -- NVDA ships Python 3.13, which removed audioop, so any
        per-sample work in Python would be hand-rolled and slow. Volume is
        applied inside the DLL instead.)
        """
        if count > len(self._sample_buf):
            count = len(self._sample_buf)
        with _EXEC_LOCK:
            got = self._lib.dtc01_run_samples(self._h, self._sample_buf, count)
        if got <= 0:
            return b""
        return bytes(memoryview(self._sample_buf)[:got].cast("B"))

    def peak_of(self, pcm: bytes) -> int:
        """Peak absolute amplitude of a PCM block from run_block()."""
        if not pcm:
            return 0
        mv = memoryview(pcm).cast("h")
        return max(max(mv), -min(mv))

    @property
    def volume(self) -> int:
        return self._lib.dtc01_get_volume(self._h)

    @volume.setter
    def volume(self, percent: int) -> None:
        self._lib.dtc01_set_volume(self._h, int(percent))

    def read_host_tx(self) -> bytes:
        chunks = []
        while True:
            n = self._lib.dtc01_read_host_tx(self._h, self._tx_buf, len(self._tx_buf))
            if n <= 0:
                break
            chunks.append(bytes(self._tx_buf[:n]))
            if n < len(self._tx_buf):
                break
        return b"".join(chunks)

    # -- introspection -----------------------------------------------------
    @property
    def is_idle(self) -> bool:
        return bool(self._lib.dtc01_is_idle(self._h))

    @property
    def led_state(self) -> int:
        return self._lib.dtc01_get_led(self._h)

    @property
    def infifo_count(self) -> int:
        return self._lib.dtc01_infifo_count(self._h)

    @property
    def outfifo_count(self) -> int:
        return self._lib.dtc01_outfifo_count(self._h)

    @property
    def pending_text(self) -> int:
        return self._lib.dtc01_pending_text(self._h)

    @property
    def unmapped_accesses(self) -> int:
        return self._lib.dtc01_unmapped_accesses(self._h)

    @property
    def time_seconds(self) -> float:
        return self._lib.dtc01_time_seconds(self._h)

    def read_ram32(self, addr: int) -> int | None:
        out = ctypes.c_uint32()
        if self._lib.dtc01_read_ram32(self._h, addr, ctypes.byref(out)):
            return out.value
        return None

    @property
    def version(self) -> str:
        return self._lib.dtc01_version().decode("ascii", "replace")

    @property
    def dll_path(self) -> Path:
        return self._dll_path
