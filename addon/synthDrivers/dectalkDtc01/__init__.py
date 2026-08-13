"""NVDA synth driver for the emulated DECtalk DTC-01 (1984).

The real DTC-01's firmware runs inside a native emulator (see
`emu/native.py` and `native/` in the project tree); this module is only the
NVDA-facing layer: settings, text -> DECtalk command stream, audio pumping,
and index reporting.

Ships **no ROM bytes** -- the DTC-01 firmware is DEC/Fonix copyright. The
user supplies their own dump; `emu/rom_loader.py` recognises the chips by
content hash (so filenames don't matter) and refuses to start otherwise.
See DESIGN.md §0.
"""

import builtins
import os
import queue
import re
import threading
import time
import unicodedata
from collections import OrderedDict

import config
import nvwave
from logHandler import log
from speech.commands import IndexCommand
from synthDriverHandler import (
	SynthDriver,
	VoiceInfo,
	synthDoneSpeaking,
	synthIndexReached,
)

# NumericDriverSetting lives in autoSettingsUtils.driverSetting (NVDA 2019.3+);
# older builds re-exported it from driverHandler. It is only needed for the
# extra Design Voice sliders, so if neither location has it we drop those
# settings rather than failing to import -- a moved helper should never cost
# the user the whole synthesiser.
try:
	from autoSettingsUtils.driverSetting import NumericDriverSetting, BooleanDriverSetting
except ImportError:
	try:
		from driverHandler import NumericDriverSetting, BooleanDriverSetting
	except ImportError:
		NumericDriverSetting = BooleanDriverSetting = None

# Say all is detected rather than configured: joining lines across
# newlines only makes sense while a whole document is being read. Outside
# say all there is no "next line" to flush a held fragment, so navigation
# text would sit in the buffer and be heard one utterance late.
try:
	from speech.sayAll import SayAllHandler as _SayAllHandler
except ImportError:
	_SayAllHandler = None

from .emu import rom_loader
from .emu.native import NativeMachine, NativeUnavailable
from .protocol import commands as dtcmd
from .ratebooster import RateBooster

_ = getattr(builtins, "_", lambda text: text)

_HERE = os.path.dirname(__file__)

SAMPLE_RATE = 10000  # the DTC-01's DAC is a hard 10kHz (DESIGN.md §1)

# Audio is pumped in 100ms blocks. The emulator runs ~9-10x realtime, so a
# block costs ~10ms of CPU; nvwave.WavePlayer.feed() blocks once its buffer
# is full, which is what paces us back to realtime.
# 25ms blocks. This was 100ms, which is far too coarse for character echo:
# a spoken character is only ~0.3s, so the end-of-utterance debounce alone
# (4 blocks) cost 400ms and every character took ~0.7s to retire, which is
# slower than someone types. Finer blocks cost more ctypes calls (40/s
# instead of 10/s) -- irrelevant next to the emulation itself.
BLOCK_SAMPLES = 250

# An utterance is finished when the firmware reports both SPC FIFOs drained
# *and* the audio has stayed quiet for a moment. The debounce matters
# because the FIFOs briefly empty between clauses mid-sentence.
SILENCE_THRESHOLD = 120
SILENCE_BLOCKS = 6          # 150ms of quiet (was 400ms)
# ...but the firmware goes quiet *inside* an utterance while it works out how
# to say something -- measured at 450ms before a long number ("20366 of
# 20366"), where our own buffers are empty too, so it looks exactly like the
# end. At 150ms the utterance was declared finished mid-way and the rest
# surfaced attached to the next one. Short text has no such gap (measured 0ms
# for "Desktop list" and "Paperback 5 of 12"), so key echo keeps the snappy
# threshold and only text long enough to contain a pause pays for the wait.
LONG_TEXT_CHARS = 40
LONG_SILENCE_BLOCKS = 28    # 700ms, comfortably past the 450ms observed

# Silence only counts as "finished" once we've actually heard speech. Right
# after feed_text the DUART FIFO has already drained and the firmware is
# still doing letter-to-sound work, so the pipeline momentarily looks idle
# and silent -- without this guard an utterance ends after ~400ms of lead-in
# and gets truncated. Measured lead-in is ~0.3s of emulated time; the cap
# below is counted in emulated blocks (not wall clock) so it behaves the
# same regardless of host speed.
MAX_LEADIN_BLOCKS = 160     # 4s emulated

# Ceiling on flushing abandoned speech after a cancel, in emulated blocks.
DRAIN_MAX_BLOCKS = 1600     # 40s emulated (~4s wall at ~10x)
# On cancel, first try to run the abandoned utterance out inline. Short echo
# text finishes well within this, which keeps the instance clean and avoids
# burning a spare -- important when typing, where cancels come faster than
# dirty instances can be recycled. Longer text overruns it and we swap.
# Expressed in emulated blocks so behaviour is host-independent -- but the
# *wall* cost of a given budget falls as the core gets faster. 40 was chosen as
# "~100ms wall at ~10x"; at ~19.7x it spends only ~51ms, leaving half the
# intended budget unused. Measured swap rate on echo-length text cancelled
# after 4 blocks: 40 -> 53%, 80 -> 20%, 120 -> 0%. 80 restores the wall cost
# the value was picked for; 120 would exceed the cancelled-keystroke latency
# budget already tuned below.
QUICK_DRAIN_BLOCKS = 80     # 2s emulated (~100ms wall at ~19.7x)
# ...but give up much sooner if the abandoned utterance had not even started
# speaking, since then we're just waiting on letter-to-sound with nothing to
# reclaim. This is the difference between ~120ms and ~60ms of added latency
# on every cancelled keystroke.
QUICK_DRAIN_LEADIN = 16     # 0.4s emulated (~40ms wall)
# Hard bound on the last-resort recovery when every instance is dirty.
# Beyond this we reset instead, so the worker can never stop feeding the
# audio device for an unbounded stretch.
FALLBACK_DRAIN_BLOCKS = 80  # 2s emulated (~200ms wall)
# Ceiling on a single utterance, so a pathological input can't wedge the queue.
UTTERANCE_MAX_BLOCKS = 12000  # 5 minutes of emulated audio
# How long the worker waits for work before using the lull to flush a dirty
# emulator instance.
IDLE_CLEAN_INTERVAL = 0.05
# Blocks of flushing done per speaking iteration (interleaved, see
# _cleanSlice) and per idle tick (where we can afford much more).
# Sized so a slice comfortably fits inside one block's playback period:
# a block is 25ms of audio and costs ~2.5ms to generate, so cleaning 3
# blocks (~7.5ms) keeps a tick near 40% utilisation. This was briefly 12,
# which is ~30ms of work between feeds that must be 25ms apart -- the
# driver then ran late on every tick while recycling an instance, starving
# the output device. Smaller audio blocks mean *more* ticks, so per-tick
# work has to shrink with them, not grow.
CLEAN_SLICE_BLOCKS = 3
IDLE_CLEAN_BLOCKS = 160     # idle ticks have the whole period spare
# Emulator instances kept in rotation. Each is ~0.5MB and boots in ~0.5s on
# the worker thread (speech is available as soon as the first one is up).
# More instances = more consecutive cancels absorbed before we have to fall
# back to flushing one synchronously.
EMULATOR_INSTANCES = 3
# Ceiling for Sonic time-compression when rate boost is enabled.
RATE_BOOST_MAX = 3.0
# Sentence enders. In smooth mode these -- not NVDA's line boundaries --
# decide where one utterance stops and the next begins.
SENTENCE_ENDERS = ".!?"
# "Variable Val" is the DTC-01's user-definable voice slot -- on real
# hardware it holds whatever "[:dv ... save]" last stored. It is therefore
# the one voice whose parameters should survive switching away and back,
# while the seven fixed voices reset to their own factory defaults.
VARIABLE_VOICE = "val"
# How far below a voice's own loudness the volume slider can reach.
# Volume 100 == the voice as designed; 0 == this many dB quieter.
VOLUME_RANGE_DB = 40
# How long to hold text that has no sentence ender yet, waiting to see if
# the next line continues it. Say all replies within a few ms once its
# index is reported, so this only really elapses at the end of a document.
PENDING_FLUSH_SECONDS = 0.25
# How often to summarise pipeline health into the NVDA log.
STATS_LOG_EVERY = 50
STATS_LOG_SECONDS = 30.0

# Power-on announcement (see _consumeBootAnnouncement). Measured: begins
# ~0.8s after reset and runs ~2.6s. Both caps are in emulated 100ms blocks.
BOOT_ANNOUNCE_WAIT_BLOCKS = 240  # 6s to start speaking before we give up
BOOT_MAX_BLOCKS = 1000           # 25s hard ceiling on the whole announcement

_romDirCache = None
_romVersionCache = None


def romVersion():
	"""Which firmware version to run, from $DTC01_ROM_VERSION.

	Env-var gate rather than a settings-panel entry. v1.8 does speak
	correctly now (DESIGN.md §21), but the constants above the emulator were
	all measured against v2.0 -- FIRMWARE_LINE_BYTES (§19's input cliff),
	the boot-announcement windows below, and the NVRAM defaults, whose real
	v1.8 image is still unknown. Those need re-measuring before v1.8 is a
	choice worth putting in front of a user. A bad value falls back to the
	default instead of leaving the user with no synth at all.
	"""
	global _romVersionCache
	if _romVersionCache is None:
		try:
			_romVersionCache = rom_loader.resolve_version()
		except ValueError as e:
			log.warning(f"DTC-01: {e}; using {rom_loader.DEFAULT_VERSION}")
			_romVersionCache = rom_loader.DEFAULT_VERSION
		if _romVersionCache != rom_loader.DEFAULT_VERSION:
			log.warning(
				f"DTC-01: {rom_loader.VERSION_ENV} selects "
				f"{rom_loader.ROM_SETS[_romVersionCache].description} -- "
				f"it speaks, but timing and NVRAM defaults are still "
				f"calibrated for "
				f"{rom_loader.ROM_SETS[rom_loader.DEFAULT_VERSION].description}")
	return _romVersionCache


def _candidateRomDirs():
	env = os.environ.get("DTC01_ROM_DIR")
	if env:
		yield env
	try:
		import globalVars
		yield os.path.join(globalVars.appArgs.configPath, "dectalkDtc01", "roms")
	except Exception:
		appdata = os.environ.get("APPDATA")
		if appdata:
			yield os.path.join(appdata, "nvda", "dectalkDtc01", "roms")
	yield os.path.join(_HERE, "roms")


TRACE_FLAG_NAME = "dectalkDtc01/trace.flag"


def _configDir():
	"""NVDA's user config directory, or None outside NVDA."""
	try:
		import globalVars
		return globalVars.appArgs.configPath
	except Exception:
		appdata = os.environ.get("APPDATA")
		return os.path.join(appdata, "nvda") if appdata else None


def _traceEnabled():
	"""True if <NVDA config>/dectalkDtc01/trace.flag exists.

	A flag file rather than a setting, because this is a diagnostic and the
	settings panel is deliberately kept short; and rather than NVDA's global
	debug level, which is noisy enough to perturb the timing that a
	speech-timing bug depends on. Checked once, at driver construction.
	"""
	base = _configDir()
	if not base:
		return False
	try:
		return os.path.isfile(os.path.join(base, "dectalkDtc01", "trace.flag"))
	except Exception:
		return False


def _snip(text, limit=60):
	"""Shorten text for a log line, showing that it was shortened."""
	text = str(text).replace("\r", "\\r").replace("\n", "\\n")
	return text if len(text) <= limit else text[:limit] + "..."


def findRomDir(refresh=False):
	"""First directory holding a complete, checksum-valid ROM set, or None.

	"Complete" means for the *selected* firmware version only -- a dump of
	just one version is a valid dump, so requiring both would reject it.
	"""
	global _romDirCache
	if _romDirCache is not None and not refresh:
		return _romDirCache or None
	version = romVersion()
	for path in _candidateRomDirs():
		if not path or not os.path.isdir(path):
			continue
		try:
			rom_loader.validate_rom_dir(path, version)
		except rom_loader.RomValidationError:
			continue
		except Exception:
			log.debugWarning(f"DTC-01: error validating ROMs in {path}", exc_info=True)
			continue
		_romDirCache = path
		return path
	_romDirCache = ""
	return None


def _makePlayer():
	try:
		return nvwave.WavePlayer(
			channels=1,
			samplesPerSec=SAMPLE_RATE,
			bitsPerSample=16,
			outputDevice=config.conf["audio"]["outputDevice"],
		)
	except Exception:
		return nvwave.WavePlayer(1, SAMPLE_RATE, 16)


# The firmware speaks a clause only once it sees a clause boundary; a bare
# carriage return does NOT flush it (verified against the real ROM: "This\r"
# stays silent indefinitely, "This.\r" speaks in 0.1s). Anything NVDA sends
# without trailing punctuation -- single words from word echo, character
# echo, most control announcements -- would otherwise sit in the buffer
# until the *next* utterance's "[" command bracket pushed it out, which is
# heard as every utterance arriving one behind.
# Only these actually flush the firmware's buffer -- measured against the
# ROM. Semicolon, colon and dash do NOT: text ending in one of those is
# buffered and never spoken. An earlier list included ";" and ":", which
# would silently swallow anything ending in "Note:" or "Warning:".
_TERMINATORS = ".!?,"


def _terminate(text):
	"""Ensure a chunk ends at a clause boundary so the firmware speaks it.

	Text already ending in a real sentence mark is left alone. Anything else
	gets a **comma**, never a full stop.

	Say all hands over one *line* at a time, and a line ending without
	punctuation is by definition mid-sentence. Closing it with a period made
	the firmware apply sentence-final intonation, so a hard wrap inside a
	sentence was read as "then I am." -- audibly a full stop where the
	sentence carries on. Period and comma cost the same time here (measured
	against the ROM: 0.80s either way), so the comma is free and changes
	only the intonation.

	A previous version chose between them by asking whether more text was
	already queued. That never fired: NVDA's say all waits for our index
	before sending the next line, so the queue is always empty at this
	point. The decision has to come from the text itself.
	"""
	text = text.rstrip()
	if not text:
		return text
	if text[-1] in _TERMINATORS:
		return text
	return text + ","


# Longest input line the firmware accepts. Measured: payloads up to 133 bytes
# speak in full and grow linearly with length; at 136 and beyond the output
# collapses to a fixed ~1.74s stub regardless of how much was sent. 120 leaves
# margin rather than sitting on a cliff whose exact edge is between the two.
FIRMWARE_LINE_BYTES = 120

# Where to break an over-long line, best first. Splitting at a sentence end
# costs nothing -- the piece still ends in real sentence punctuation. Clause
# marks are next. A space is the last resort before a hard cut, and only a
# single unbroken run longer than the limit reaches that.
_SPLIT_PREFERENCE = (".!?", ",;:", " ")


def _splitForFirmware(text, budget, firstOverhead=0):
	"""Break text into pieces that each fit one firmware input line.

	`budget` counts the whole line, so room is reserved for the terminator
	`_terminate` may add and for the carriage return. `firstOverhead` is the
	command prefix, which only rides on the first line.

	Text already short enough comes back as a single piece, unchanged -- the
	common case must not be perturbed.
	"""
	room = budget - 2                      # ",", "\r"
	rest = (text or "").strip()
	if not rest or len(rest) + firstOverhead <= room:
		return [rest] if rest else []
	pieces = []
	limit = room - firstOverhead
	while rest:
		if len(rest) <= limit:
			pieces.append(rest)
			break
		window = rest[:limit]
		cut = -1
		for chars in _SPLIT_PREFERENCE:
			cut = max(window.rfind(c) for c in chars)
			if cut > 0:
				break
		if cut <= 0:
			cut = limit - 1                # one unbroken run: cut it
		pieces.append(rest[:cut + 1].strip())
		rest = rest[cut + 1:].strip()
		limit = room
	return [p for p in pieces if p]


def _cleanText(text):
	"""Fold to the 7-bit ASCII the 1984 firmware understands, then neutralise
	square brackets so user text can't be mistaken for a command sequence."""
	text = unicodedata.normalize("NFKD", text)
	text = text.encode("ascii", "replace").decode("ascii")
	text = re.sub(r"[^\x20-\x7e]", " ", text)
	return dtcmd.sanitize_text(text)


class SynthDriver(SynthDriver):
	name = "dectalkDtc01"
	description = _("DECtalk DTC-01")

	supportedSettings = (
		SynthDriver.VoiceSetting(),
		SynthDriver.RateSetting(minStep=5),
		SynthDriver.RateBoostSetting(),
		SynthDriver.PitchSetting(minStep=5),
		SynthDriver.InflectionSetting(minStep=5),
		SynthDriver.VolumeSetting(minStep=5),
	) + (
		# The remaining Design Voice parameters the 1984 firmware actually
		# implements (DESIGN.md §6, Table 5-3). Deliberately not exposing
		# anything the ROM lacks, however familiar the name from later
		# DECtalk releases.
		(
			NumericDriverSetting("headSize", _("&Head size"), minStep=5),
			NumericDriverSetting("breathiness", _("&Breathiness"), minStep=5),
			NumericDriverSetting("richness", _("&Richness"), minStep=5),
			NumericDriverSetting("smoothness", _("&Smoothness"), minStep=5),
			# The ROM's in-chain level control (g5, "Loudness"). Distinct
			# from Volume, which scales the finished audio: lowering this
			# prevents the overload that extreme head-size settings cause,
			# where output gain could only attenuate an already-clipped
			# signal. Measured: head size at minimum clips 625 samples flat
			# against the rail; this brings that to zero.
			NumericDriverSetting("loudness", _("Formant &gain"), minStep=5),
		) if NumericDriverSetting is not None else ()
	)
	supportedCommands = {IndexCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	@classmethod
	def check(cls):
		if findRomDir() is None:
			return False
		try:
			from .emu.native import _find_dll
			_find_dll()
		except Exception:
			return False
		return True

	# -- lifecycle ---------------------------------------------------------
	def __init__(self):
		super().__init__()
		self._rate = 50
		self._rateBoost = False
		self._pitch = 50
		self._inflection = 50
		self._volume = 100
		self._voice = "paul"
		self._headSize = 50
		self._breathiness = 50   # 50 == this voice's own default, as for every slider
		self._richness = 50
		self._smoothness = 50
		self._loudness = 50
		self._valParams = None   # remembered sliders for Variable Val
		# Text held back waiting for a sentence boundary (smooth mode).
		self._pendingText = ""
		self._pendingSince = 0.0
		self._booster = RateBooster(SAMPLE_RATE)

		# Two emulator instances, used ping-pong. The firmware cannot be
		# interrupted (no abort character exists -- verified against the ROM),
		# so a cancelled utterance has to be played out before its machine is
		# reusable, costing ~0.5s. Swapping to an already-clean spare makes
		# cancel instant; the dirty one is flushed later while the worker is
		# idle. Note this cannot be done on another thread: the vendored
		# Musashi core keeps CPU state in globals, so only one machine may be
		# executing at a time (see dtc01.h).
		self._stats = {
			"utterances": 0, "cancels": 0, "quickDrainOk": 0, "swaps": 0,
			"fallbacks": 0, "resets": 0, "fallbackSeconds": 0.0,
			"slowBlocks": 0, "blocks": 0, "worstGapMs": 0.0, "stops": 0,
			# Utterances NVDA queued that a cancel threw away before they were
			# ever spoken. Counted always, because "a phrase went missing" is
			# indistinguishable from normal operation without it.
			"discarded": 0,
		}
		self._trace = _traceEnabled()
		self._uttSeq = 0
		if self._trace:
			log.info("DTC-01: utterance tracing ON (%s exists)" % TRACE_FLAG_NAME)
		self._fedSinceStop = False
		self._samplesFed = 0     # cumulative 16-bit samples delivered to NVDA
		self._lastStatsLog = 0
		self._lastStatsTime = time.monotonic()
		self._machines = []
		self._dirty = []
		self._cleanState = {}
		# Last command prefix each instance has been sent. Re-sending
		# "[:np][:ra 235]" before every chunk costs a 200ms clause break --
		# a mid-stream voice command forces the firmware to pause (the
		# manual warns a voice change needs silence around it, DESIGN.md
		# §6b). Sending it only when it actually changes removes that pause
		# from every line break. Cleared per instance whenever it reboots.
		self._lastPrefix = {}
		self._activeIdx = 0
		self._machineReady = threading.Event()
		self._player = _makePlayer()

		self._jobs = queue.Queue()
		self._generation = 1
		self._stateLock = threading.Lock()
		self._stopping = threading.Event()
		self._needsDrain = False

		self._worker = threading.Thread(
			target=self._workerLoop, name="DECtalk DTC-01 synth", daemon=True
		)
		self._worker.start()

	@property
	def _machine(self):
		if not self._machines:
			return None
		return self._machines[self._activeIdx]

	def terminate(self):
		self._stopping.set()
		self.cancel()
		self._jobs.put(None)
		self._worker.join(timeout=5)
		try:
			self._player.close()
		except Exception:
			pass
		try:
			self._booster.close()
		except Exception:
			pass
		for machine in self._machines:
			try:
				machine.close()
			except Exception:
				pass
		self._machines = []
		super().terminate()

	# -- settings -----------------------------------------------------------
	def _get_availableVoices(self):
		labels = {
			"paul": _("Perfect Paul"),
			"betty": _("Beautiful Betty"),
			"harry": _("Huge Harry"),
			"frank": _("Frail Frank"),
			"kit": _("Kit the Kid"),
			"rita": _("Rough Rita"),
			"ursula": _("Uppity Ursula"),
			"val": _("Variable Val"),
		}
		return OrderedDict(
			(key, VoiceInfo(key, labels.get(key, key), language="en"))
			for key in dtcmd.VOICES
		)

	def _get_voice(self):
		return self._voice

	# The parameter sliders, in the order they are saved/restored.
	_PARAM_ATTRS = ("_pitch", "_inflection", "_headSize",
					"_breathiness", "_richness", "_smoothness", "_loudness")

	def _captureParams(self):
		return tuple(getattr(self, attr) for attr in self._PARAM_ATTRS)

	def _applyParams(self, values):
		for attr, value in zip(self._PARAM_ATTRS, values):
			setattr(self, attr, value)

	def _set_voice(self, value):
		if value not in dtcmd.VOICES or value == self._voice:
			return
		if self._voice == VARIABLE_VOICE:
			# Variable Val is the hardware's user-definable slot -- the one
			# voice whose whole purpose is to hold settings you chose. Keep
			# its sliders so they come back when it is reselected.
			self._valParams = self._captureParams()
		self._voice = value
		if value == VARIABLE_VOICE and self._valParams is not None:
			self._applyParams(self._valParams)
			return
		# Every other voice carries its own Design Voice defaults, and the
		# sliders are relative to them (50 = this voice's default). Carrying
		# slider positions across would silently reinterpret them against
		# different numbers, so they snap back to the new voice's defaults.
		self._applyParams((50,) * len(self._PARAM_ATTRS))

	def _get_rate(self):
		return self._rate

	def _set_rate(self, value):
		self._rate = max(0, min(100, int(value)))

	def _get_volume(self):
		return self._volume

	def _set_volume(self, value):
		self._volume = max(0, min(100, int(value)))
		for machine in self._machines:
			try:
				machine.volume = self._volume
			except Exception:
				log.debugWarning("DTC-01: could not apply volume", exc_info=True)

	def _unusedLoudnessGain(self):
		"""Volume as the firmware's own loudness control (g5).

		This replaces gain applied to the finished audio, which could only
		ever scale a signal that had *already* clipped. g5 sits inside the
		synthesis chain, so lowering it prevents the overload instead of
		attenuating the damage: at head size 40 the output clips 625 samples
		flat against the rail, and g5 55 brings that to zero while landing
		at essentially the stock level.

		Full volume is the voice's own designed loudness rather than the
		parameter's 80dB ceiling -- pushing past the voice's default is
		exactly what provokes the clipping.
		"""
		default, lo, _hi = dtcmd.voice_param(self._voice, "g5")
		floor = max(lo, default - VOLUME_RANGE_DB)
		return int(round(floor + (default - floor) * (self._volume / 100.0)))

	def _sayAllActive(self):
		"""True while NVDA is reading continuously.

		Line joining is applied only here. During say all NVDA supplies the
		following line as soon as the current one's index is reported, so a
		fragment held for a sentence ender is always flushed promptly. In
		ordinary navigation nothing follows, so the same behaviour would
		leave each utterance waiting -- heard as speech running one item
		behind. Detected rather than exposed as a setting, since there is no
		situation where a user would want the say-all behaviour applied to
		navigation.
		"""
		if _SayAllHandler is None:
			return False
		try:
			return bool(_SayAllHandler.isRunning())
		except Exception:
			return False

	def _get_rateBoost(self):
		return self._rateBoost

	def _set_rateBoost(self, value):
		self._rateBoost = bool(value)

	def _get_pitch(self):
		return self._pitch

	def _set_pitch(self, value):
		self._pitch = max(0, min(100, int(value)))

	def _get_inflection(self):
		return self._inflection

	def _set_inflection(self, value):
		self._inflection = max(0, min(100, int(value)))

	def _get_headSize(self):
		return self._headSize

	def _set_headSize(self, value):
		self._headSize = max(0, min(100, int(value)))

	def _get_breathiness(self):
		return self._breathiness

	def _set_breathiness(self, value):
		self._breathiness = max(0, min(100, int(value)))

	def _get_richness(self):
		return self._richness

	def _set_richness(self, value):
		self._richness = max(0, min(100, int(value)))

	def _get_smoothness(self):
		return self._smoothness

	def _set_smoothness(self, value):
		self._smoothness = max(0, min(100, int(value)))

	def _get_loudness(self):
		return self._loudness

	def _set_loudness(self, value):
		self._loudness = max(0, min(100, int(value)))

	@staticmethod
	def _scale(value, lo, hi):
		"""NVDA's 0-100 slider onto a firmware parameter's real range."""
		return int(round(lo + (hi - lo) * (max(0, min(100, value)) / 100.0)))

	def _rateParams(self):
		"""Split NVDA's rate slider into a firmware rate and a Sonic speed.

		The firmware clamps at 350 wpm, which is slow by screen-reader
		standards. With rate boost off the whole slider maps onto the
		hardware's own 120-350 range. With it on, the first half reaches
		350 wpm and the rest is time-compression on the rendered audio --
		faster than the 1984 hardware could go, with its pitch intact.
		"""
		lo, hi = dtcmd.RATE_MIN_WPM, dtcmd.RATE_MAX_WPM
		if not self._rateBoost:
			return self._scale(self._rate, lo, hi), 1.0
		if self._rate <= 50:
			return self._scale(self._rate * 2, lo, hi), 1.0
		return hi, 1.0 + (self._rate - 50) / 50.0 * (RATE_BOOST_MAX - 1.0)

	def _nativeRateWpm(self):
		return self._rateParams()[0]

	def _designVoiceCommand(self):
		"""[:dv ...] for the parameters that differ from their defaults.

		Only non-default values are sent: the prefix is prepended to every
		utterance, and a longer one costs parsing time before speech starts.
		Ranges are the firmware's own (DESIGN.md §6).
		"""
		params = {}
		for sliderValue, abbr, name in (
			(self._pitch, "ap", "averagePitch"),
			(self._inflection, "pr", "pitchRange"),
			(self._headSize, "hs", "headSize"),
			(self._breathiness, "br", "breathiness"),
			(self._richness, "ri", "richness"),
			(self._smoothness, "sm", "smoothness"),
			(self._loudness, "g5", "loudness"),
		):
			if sliderValue == 50:
				continue  # 50 means "leave this voice's own default alone"
			params[name] = dtcmd.scale_from_default(self._voice, abbr, sliderValue)
		if not params:
			return ""
		return dtcmd.design_voice_command(**params)

	def _commandPrefix(self):
		# Voice first: selecting a voice resets the design-voice parameters,
		# so the [:dv] block has to follow it, not precede it.
		return (dtcmd.voice_command(self._voice)
				+ dtcmd.rate_command(self._nativeRateWpm())
				+ self._designVoiceCommand())

	# -- NVDA speech API ----------------------------------------------------
	def speak(self, speechSequence):
		events = []
		parts = []

		def flush():
			text = _cleanText("".join(parts)).strip()
			parts.clear()
			if text:
				events.append(("text", text))

		for item in speechSequence:
			if isinstance(item, str):
				parts.append(item)
			elif isinstance(item, IndexCommand):
				flush()
				events.append(("index", item.index))
		flush()
		if not events:
			if self._trace:
				log.info("DTC-01 trace: speak() produced no events from %r"
						 % _snip("".join(str(i) for i in speechSequence if isinstance(i, str))))
			return
		with self._stateLock:
			generation = self._generation
			self._uttSeq += 1
			uttId = self._uttSeq
		if self._trace:
			said = " | ".join(_snip(v) for k, v in events if k == "text")
			log.info("DTC-01 trace: speak #%d gen=%d -> %r" % (uttId, generation, said))
		self._jobs.put((generation, events, uttId))

	def cancel(self):
		with self._stateLock:
			self._generation += 1
			self._needsDrain = True
			self._stats["cancels"] += 1
		# Anything still queued here is speech NVDA asked for that will never
		# be spoken. Usually correct -- the user moved on -- but it is also the
		# most likely way a phrase goes missing, so it is always counted and,
		# when tracing, named.
		discarded = []
		while True:
			try:
				job = self._jobs.get_nowait()
				self._jobs.task_done()
				discarded.append(job)
			except queue.Empty:
				break
		# terminate() puts a None sentinel on this same queue, so a cancel
		# racing shutdown can pull it out. Keep it out of the count and, more
		# importantly, out of the unpacking below.
		discarded = [j for j in discarded if j is not None]
		if discarded:
			with self._stateLock:
				self._stats["discarded"] += len(discarded)
			if self._trace:
				for _gen, events, uttId in discarded:
					said = " | ".join(_snip(v) for k, v in events if k == "text")
					log.info("DTC-01 trace: DISCARDED #%d by cancel -> %r" % (uttId, said))
		# Only reset the device if we have actually queued audio since the
		# last reset. With key echo, cancel() fires on every keystroke --
		# hundreds of times a minute -- and each stop() is a real operation
		# on the output stream. Skipping the no-op ones removes a large
		# amount of needless churn from the audio path.
		if self._pendingText and self._trace:
			log.info("DTC-01 trace: DROPPED held fragment by cancel -> %r"
					 % _snip(self._pendingText))
		self._pendingText = ""   # interrupted: don't speak held fragments later
		self._booster.reset()
		if self._fedSinceStop:
			self._fedSinceStop = False
			try:
				# Also unblocks a worker parked inside WavePlayer.feed().
				self._player.stop()
				self._stats["stops"] += 1
			except Exception:
				log.debugWarning("DTC-01: audio cancellation failed", exc_info=True)

	def pause(self, switch):
		try:
			self._player.pause(switch)
		except Exception:
			log.debugWarning("DTC-01: audio pause failed", exc_info=True)

	# -- worker -------------------------------------------------------------
	def _isCurrent(self, generation):
		with self._stateLock:
			return not self._stopping.is_set() and generation == self._generation

	def _bootMachines(self, count=EMULATOR_INSTANCES):
		romDir = findRomDir()
		if romDir is None:
			log.error("DTC-01: no valid ROM set found; synth cannot start")
			return
		version = romVersion()
		for _ in range(count):
			try:
				machine = NativeMachine(romDir, rom_version=version)
			except NativeUnavailable:
				log.error("DTC-01: native emulator DLL missing for this architecture",
						  exc_info=True)
				break
			except Exception:
				log.error("DTC-01: failed to create emulator", exc_info=True)
				break
			machine.volume = self._volume
			self._consumeBootAnnouncement(machine)
			self._machines.append(machine)
			self._dirty.append(False)
			# Usable as soon as the first one is up; the spare only affects
			# how fast cancellation recovers.
			if len(self._machines) == 1:
				self._machineReady.set()
				log.info(f"DTC-01: ready ({machine.version}), "
						 f"{rom_loader.ROM_SETS[version].description} ROMs from {romDir}")
		log.info(f"DTC-01: {len(self._machines)} emulator instance(s) booted")

	def _swapToCleanMachine(self):
		"""Called when the active machine still holds cancelled speech.

		Prefers swapping to an already-clean spare (instant) over flushing
		the dirty one (~0.5s). Returns True if a swap happened.
		"""
		# Cheapest case first: if the abandoned utterance is nearly over --
		# which is the norm for typed characters and other short echo text --
		# just run it out here. That keeps this instance clean and avoids
		# consuming a spare, which matters because during fast typing cancels
		# arrive faster than dirty instances can be recycled.
		# Draining is precisely where the firmware's mid-utterance pauses show
		# up, so a short silence threshold here declares an instance clean
		# while it still holds speech -- which then comes out attached to the
		# next utterance. Use the long threshold.
		if self._pump(self._machines[self._activeIdx],
					  maxBlocks=QUICK_DRAIN_BLOCKS,
					  leadInBlocks=QUICK_DRAIN_LEADIN,
					  silenceBlocks=LONG_SILENCE_BLOCKS) == "done":
			self._stats["quickDrainOk"] += 1
			return False

		self._dirty[self._activeIdx] = True
		for i, isDirty in enumerate(self._dirty):
			if not isDirty:
				self._activeIdx = i
				self._stats["swaps"] += 1
				return True
		# Every instance is dirty (sustained rapid cancels). Recover the
		# active one, but do it under a hard time bound: an unbounded drain
		# here blocks the worker without feeding the audio device, which
		# starves playback for as long as it takes -- heard as grainy,
		# stretched audio. A short drain handles the common case; anything
		# longer is cut off with a reset, whose cost does not depend on how
		# much speech was abandoned.
		machine = self._machines[self._activeIdx]
		self._stats["fallbacks"] += 1
		started = time.monotonic()
		if self._pump(machine, maxBlocks=FALLBACK_DRAIN_BLOCKS,
					  leadInBlocks=QUICK_DRAIN_LEADIN,
					  silenceBlocks=LONG_SILENCE_BLOCKS) != "done":
			self._stats["resets"] += 1
			try:
				machine.reset()
				# Rebooted: its voice/rate settings are back to defaults.
				self._lastPrefix.pop(self._activeIdx, None)
				self._consumeBootAnnouncement(machine)
			except Exception:
				log.error("DTC-01: emulator reset failed", exc_info=True)
		self._stats["fallbackSeconds"] += time.monotonic() - started
		self._dirty[self._activeIdx] = False
		self._cleanState.pop(self._activeIdx, None)
		return False

	def _cleanSlice(self, maxBlocks=CLEAN_SLICE_BLOCKS):
		"""Advance the flushing of one dirty instance by a few blocks.

		This is called *while speaking*, not only during lulls. During
		continuous navigation the worker is never idle -- every new line
		cancels the previous one -- so lull-only cleaning never runs and
		every instance ends up dirty, which is what left arrowing stuck at
		~390ms. There is plenty of headroom to do this: playback throttles
		the worker to realtime while the emulator runs ~10x, so ~90% of the
		time is spent waiting on audio anyway.

		Progress is kept per instance so a slice can stop anywhere and
		resume on the next call.
		"""
		for i, isDirty in enumerate(self._dirty):
			if not isDirty or i == self._activeIdx:
				continue
			machine = self._machines[i]
			state = self._cleanState.setdefault(i, {"heard": False, "quiet": 0, "lead": 0})
			for _ in range(maxBlocks):
				block = machine.run_block(BLOCK_SAMPLES)
				if not block:
					break
				if machine.peak_of(block) >= SILENCE_THRESHOLD:
					state["heard"] = True
					state["quiet"] = 0
					continue
				if not state["heard"]:
					state["lead"] += 1
					if state["lead"] >= MAX_LEADIN_BLOCKS and machine.is_idle:
						break
					continue
				if machine.is_idle:
					state["quiet"] += 1
					if state["quiet"] >= SILENCE_BLOCKS:
						break
				else:
					state["quiet"] = 0
			else:
				return  # slice used up, still dirty; resume next call
			self._dirty[i] = False
			self._cleanState.pop(i, None)
			return

	def _pump(self, machine, onBlock=None, isCancelled=None,
			  maxBlocks=BOOT_MAX_BLOCKS, leadInBlocks=MAX_LEADIN_BLOCKS,
			  trimSilence=False, onTick=None, silenceBlocks=SILENCE_BLOCKS):
		"""Run the emulator until whatever it is currently rendering has been
		fully spoken, optionally handing each audio block to `onBlock`.

		Every place that waits for the firmware needs the same two-phase
		rule, so they all share this one implementation:

		  1. Wait for speech to actually *start*. Immediately after text is
		     accepted the FIFOs are briefly empty while letter-to-sound runs,
		     so the pipeline momentarily looks idle and silent. Treating that
		     as "finished" truncates the utterance.
		  2. Only then treat sustained idle+silence as the end.

		Getting this wrong in one place and not another is exactly how the
		power-on announcement ended up being played as the first utterance
		(everything one behind) while ordinary speech worked fine.

		With `trimSilence`, the silence the firmware emits *before* it starts
		speaking (measured: ~400ms per utterance, while it does
		letter-to-sound) is dropped instead of played, as is the trailing
		silence used to confirm the end. Pauses *inside* an utterance are
		preserved -- they're held back only until speech resumes. This is
		pure dead air; removing it takes ~400ms off the time-to-first-sound
		for every single utterance.

		Returns "done" (reached the end), "cancelled" (isCancelled fired),
		or "exhausted" (hit maxBlocks first).
		"""
		heard = False
		quiet = 0
		leadIn = 0
		blocks = 0
		pendingSilence = []

		def emit(b):
			if onBlock is not None:
				onBlock(b)

		def flushPending():
			if pendingSilence:
				for b in pendingSilence:
					emit(b)
				pendingSilence.clear()

		while blocks < maxBlocks:
			if isCancelled is not None and isCancelled():
				return "cancelled"
			if onTick is not None:
				onTick()
			block = machine.run_block(BLOCK_SAMPLES)
			blocks += 1
			if not block:
				return "done"

			isSpeech = machine.peak_of(block) >= SILENCE_THRESHOLD

			if not trimSilence:
				emit(block)
			elif isSpeech:
				flushPending()
				emit(block)
			elif not heard:
				pass  # lead-in silence, before any speech -- dropped
			elif not machine.is_idle:
				# A pause *inside* an utterance: the pipeline still has work,
				# so more speech is coming. Emit immediately. Holding these
				# back starves the output device mid-word, which is heard as
				# a gap -- the bug that made audio grainy under key echo.
				flushPending()
				emit(block)
			else:
				# Silent *and* the pipeline has drained, so this is almost
				# certainly the tail. Hold it: if the utterance ends here it
				# is discarded, which removes ~150ms of dead air from every
				# chunk boundary -- the bulk of the pause heard at each line
				# break during say all. If speech does resume, it is flushed
				# above and nothing is lost.
				pendingSilence.append(block)
				if len(pendingSilence) > SILENCE_BLOCKS:
					flushPending()

			if isSpeech:
				heard = True
				quiet = 0
				continue

			if not heard:
				leadIn += 1
				# Nothing audible is coming (silent firmware revision, or text
				# that reduces to punctuation) -- don't stall the queue.
				if leadIn >= leadInBlocks and machine.is_idle:
					return "done"
				continue

			if machine.is_idle:
				quiet += 1
				if quiet >= silenceBlocks:
					return "done"
			else:
				quiet = 0
		return "exhausted"

	def _consumeBootAnnouncement(self, machine):
		"""Play out and discard the firmware's power-on announcement.

		Like the real hardware, the DTC-01 speaks when it powers up (~2.6s
		of speech beginning ~0.8s after reset). If it isn't consumed here it
		is still queued when the first utterance arrives, so the user hears
		the announcement in place of their first utterance and every
		utterance after that lands one behind -- which is exactly how this
		presented: typing "is" spoke "This".
		"""
		self._pump(machine, maxBlocks=BOOT_MAX_BLOCKS,
				   leadInBlocks=BOOT_ANNOUNCE_WAIT_BLOCKS)

	def _workerLoop(self):
		# Boot off NVDA's main thread so startup isn't blocked by the
		# firmware's self-test.
		try:
			self._bootMachines()
		except Exception:
			log.error("DTC-01: emulator startup failed", exc_info=True)
		finally:
			self._machineReady.set()

		while not self._stopping.is_set():
			try:
				job = self._jobs.get(timeout=IDLE_CLEAN_INTERVAL)
			except queue.Empty:
				# No more text is coming right now. If smooth mode is holding
				# a fragment waiting for a sentence ender that never arrived
				# (end of a document), speak it rather than leaving it stuck.
				try:
					if (self._pendingText
							and time.monotonic() - self._pendingSince >= PENDING_FLUSH_SECONDS):
						self._flushPendingText()
						continue
				except Exception:
					log.error("DTC-01: pending-text flush failed", exc_info=True)
					self._pendingText = ""
				# Otherwise use the lull to flush any instance still holding
				# cancelled speech, so the next cancel can swap instantly.
				try:
					self._cleanSlice(maxBlocks=IDLE_CLEAN_BLOCKS)
				except Exception:
					log.error("DTC-01: background flush failed", exc_info=True)
				continue
			try:
				if job is None:
					return
				if self._machine is None:
					continue
				generation, events, uttId = job
				if self._isCurrent(generation):
					self._render(generation, events, uttId)
				elif self._trace:
					said = " | ".join(_snip(v) for k, v in events if k == "text")
					log.info("DTC-01 trace: STALE #%d (gen %d) never rendered -> %r"
							 % (uttId, generation, said))
			except Exception:
				log.error("DTC-01: synthesis failed", exc_info=True)
			finally:
				self._jobs.task_done()

	def _drainAbandonedSpeech(self):
		"""After a cancel the firmware may still hold buffered text. Run it
		out with the audio discarded, so the next utterance doesn't open with
		the tail of the one the user just interrupted."""
		machine = self._machine
		if machine is None:
			return
		self._pump(machine, maxBlocks=DRAIN_MAX_BLOCKS,
				   silenceBlocks=LONG_SILENCE_BLOCKS)

	def _render(self, generation, events, uttId=-1):
		# The stats call has to be in a finally: nearly every utterance is
		# cancelled when key echo is on, and each of those takes an early
		# return below. With the call at the end it was only reached by
		# utterances that ran to completion, so during exactly the workload
		# worth measuring it never logged anything at all.
		started = self._samplesFed
		try:
			self._renderInner(generation, events)
		finally:
			if self._trace:
				# Outcome as observed, not as intended: whether audio actually
				# reached the device is the fact that separates "spoken" from
				# "silently dropped", and it is not visible anywhere else.
				log.info("DTC-01 trace: end #%d %s, %.2fs audio fed"
						 % (uttId,
							"complete" if self._isCurrent(generation) else "superseded",
							(self._samplesFed - started) / float(SAMPLE_RATE)))
			self._maybeLogStats()

	def _renderInner(self, generation, events):
		with self._stateLock:
			needsDrain = self._needsDrain
			self._needsDrain = False
		if needsDrain:
			self._swapToCleanMachine()

		if self._sayAllActive():
			self._renderSmooth(generation, events)
			return

		for kind, value in events:
			if not self._isCurrent(generation):
				return
			if kind == "index":
				# Synthetic indexing (DESIGN.md §7). Normally the preceding
				# chunk has fully drained by now, so this index is at a
				# guaranteed-correct position. In smooth mode the chunk is
				# deliberately still unspoken -- see _speakChunk -- so the
				# index runs ahead of the audio by roughly a line, which is
				# the trade the setting makes.
				synthIndexReached.notify(synth=self, index=value)
				continue
			if not self._speakChunk(generation, value):
				return
		if self._isCurrent(generation):
			try:
				self._player.idle()
			except Exception:
				pass
			synthDoneSpeaking.notify(synth=self)

	def _renderSmooth(self, generation, events):
		"""Split utterances on the text's own sentence boundaries.

		NVDA hands over one *line* at a time, which has nothing to do with
		where sentences end -- a hard wrap lands mid-sentence, and a list of
		short lines is several fragments of one thought. Splitting there is
		what makes wrapped prose sound chopped.

		So lines are accumulated and only sent when the accumulated text
		actually contains a sentence ender, and then only up to the last one.
		Consequences: a run-on sentence spanning several lines is spoken as
		one continuous utterance, and several short lines are joined until a
		full stop turns up.

		The index for a line is reported as soon as that line is accepted,
		not once it has been spoken -- say all will not send the next line
		until it sees the index, so holding it back would deadlock against
		our own need for more text. That is the trade this setting makes:
		position tracking leads the audio slightly.
		"""
		for kind, value in events:
			if not self._isCurrent(generation):
				return
			if kind == "index":
				synthIndexReached.notify(synth=self, index=value)
				continue
			joiner = "" if (not self._pendingText or self._pendingText.endswith(" ")) else " "
			self._pendingText += joiner + value
			self._pendingSince = time.monotonic()

		# Speak everything up to the last sentence ender; hold the remainder
		# in case the next line continues it.
		cut = max(self._pendingText.rfind(c) for c in SENTENCE_ENDERS)
		if cut >= 0:
			ready = self._pendingText[:cut + 1]
			self._pendingText = self._pendingText[cut + 1:].lstrip()
			if ready.strip() and not self._speakChunk(generation, ready.strip()):
				return
		if self._isCurrent(generation) and not self._pendingText:
			try:
				self._player.idle()
			except Exception:
				pass
			synthDoneSpeaking.notify(synth=self)

	def _flushPendingText(self):
		"""Speak text held for a sentence ender that never arrived -- the end
		of a document, or a line that simply has no full stop."""
		with self._stateLock:
			generation = self._generation
		text = self._pendingText.strip()
		self._pendingText = ""
		if not text or self._machine is None:
			return
		self._speakChunk(generation, text)
		if self._isCurrent(generation):
			try:
				self._player.idle()
			except Exception:
				pass
			synthDoneSpeaking.notify(synth=self)

	def _maybeLogStats(self):
		"""Periodically summarise how the audio pipeline is holding up.

		Exists because the interesting failure -- audio going grainy after a
		while with key echo -- has not reproduced outside NVDA, so the real
		numbers have to come from the real environment.
		"""
		st = self._stats
		st["utterances"] += 1
		now = time.monotonic()
		dueByCount = st["utterances"] - self._lastStatsLog >= STATS_LOG_EVERY
		dueByTime = now - self._lastStatsTime >= STATS_LOG_SECONDS
		if not (dueByCount or dueByTime):
			return
		self._lastStatsLog = st["utterances"]
		self._lastStatsTime = now
		slowPct = (100.0 * st["slowBlocks"] / st["blocks"]) if st["blocks"] else 0.0
		log.info(
			"DTC-01 stats: utterances=%d cancels=%d discarded=%d | quickDrainOk=%d "
			"swaps=%d fallbacks=%d resets=%d fallbackTime=%.2fs | blocks=%d "
			"late=%d (%.1f%%) worstGap=%.0fms | stops=%d | dirty=%d/%d"
			% (st["utterances"], st["cancels"], st["discarded"], st["quickDrainOk"],
			   st["swaps"], st["fallbacks"], st["resets"], st["fallbackSeconds"],
			   st["blocks"], st["slowBlocks"], slowPct, st["worstGapMs"],
			   st["stops"], sum(self._dirty), len(self._machines))
		)
		st["worstGapMs"] = 0.0

	def _speakChunk(self, generation, text):
		"""Speak one chunk, splitting it to fit the firmware. False if cancelled.

		The firmware silently discards an input line past ~134 bytes -- output
		collapses to a fixed ~1.7s stub however much longer the text is, so a
		long list item or chat message lost most of its content and sometimes
		produced nothing audible at all.

		The pieces are spoken one at a time rather than handed over together:
		the firmware's buffer holds only about two lines, so writing them all
		at once merely moves the loss further out (299 and 500 characters both
		capped at the same 14.4s). Feeding the next line only after the
		previous has been pumped is what say all has always done between
		lines, so the resulting cadence is already known to be acceptable.
		"""
		prefix = self._commandPrefix()
		# The prefix rides on the first line only, so it eats into that line's
		# budget and no other. After the first, _speakLine finds it already in
		# effect on this instance and sends nothing.
		head = 0 if self._lastPrefix.get(self._activeIdx) == prefix else len(prefix) + 1
		for piece in _splitForFirmware(text, FIRMWARE_LINE_BYTES, head):
			if not self._speakLine(generation, piece):
				return False
		return True

	def _speakLine(self, generation, text):
		"""Feed one firmware-sized line and pump its audio right through.
		Returns False if cancelled.

		This always speaks what it is given, to completion. Line joining is
		done upstream in _renderSmooth by accumulating text until a sentence
		boundary; by the time a chunk reaches here it is a complete unit.

		An earlier version also bailed out of pumping as soon as another job
		was queued, so that the next chunk could flush this one. Once say-all
		indexes started firing on receipt there was *always* a job queued, so
		it fed text and yielded without ever playing it -- say all read the
		first word and nothing more while the cursor ran to the end of the
		document.
		"""
		machine = self._machine
		body = _terminate(text)
		prefix = self._commandPrefix()
		if self._lastPrefix.get(self._activeIdx) == prefix:
			prefix = ""   # already in effect on this instance
		else:
			self._lastPrefix[self._activeIdx] = prefix
		payload = ((prefix + " " if prefix else "") + body + "\r").encode(
			"ascii", "replace")
		machine.feed_text(payload)
		if self._trace:
			log.info("DTC-01 trace:   -> inst %d fed %d bytes %r"
					 % (self._activeIdx, len(payload),
						_snip(payload.decode("ascii", "replace"), 80)))

		booster = self._booster
		booster.speed = self._rateParams()[1]
		feedFailed = False
		lastFeed = [None]
		blockMs = BLOCK_SAMPLES / (SAMPLE_RATE / 1000.0)

		def onBlock(block):
			nonlocal feedFailed
			if feedFailed:
				return
			try:
				out = booster.process(block)
				if out:
					self._player.feed(out)
			except Exception:
				log.debugWarning("DTC-01: audio feed failed", exc_info=True)
				feedFailed = True
				return
			if not out:
				# Sonic buffers; nothing came out this time, so there is no
				# delivery timing to record yet.
				return
			# Track how evenly blocks reach the device. Gaps materially
			# longer than a block's own duration mean the device had to wait
			# on us, which is what audible graininess sounds like.
			self._fedSinceStop = True
			# Audio that actually reached the device. The trace uses this to
			# tell a dropped utterance from a spoken one; nothing else records
			# it, and "we sent the text" is not the same fact.
			self._samplesFed += len(out) // 2
			now = time.monotonic()
			if lastFeed[0] is not None:
				gap = (now - lastFeed[0]) * 1000.0
				self._stats["blocks"] += 1
				if gap > blockMs * 2.0:
					self._stats["slowBlocks"] += 1
				if gap > self._stats["worstGapMs"]:
					self._stats["worstGapMs"] = gap
			lastFeed[0] = now

		status = self._pump(
			machine,
			onBlock=onBlock,
			isCancelled=lambda: feedFailed or not self._isCurrent(generation),
			maxBlocks=UTTERANCE_MAX_BLOCKS,
			trimSilence=True,
			onTick=self._cleanSlice,
			# Wait longer for the end only where a mistake is audible. During
			# say all the next chunk continues the same text, so speech left
			# over from an early "done" simply plays before it, in the right
			# order -- and waiting there costs a pause at every line break,
			# which is the thing say all was tuned to remove. While
			# navigating, the next utterance is an unrelated item and the
			# leftover is heard against the wrong text.
			silenceBlocks=(LONG_SILENCE_BLOCKS
						   if len(text) > LONG_TEXT_CHARS and not self._sayAllActive()
						   else SILENCE_BLOCKS),
		)
		try:
			tail = booster.flush()
			if tail and not feedFailed:
				self._player.feed(tail)
		except Exception:
			log.debugWarning("DTC-01: rate-boost flush failed", exc_info=True)

		if status == "cancelled":
			# Whatever the firmware still holds must be flushed before the
			# next utterance, or its tail is heard against the wrong text.
			with self._stateLock:
				self._needsDrain = True
			return False
		return True
