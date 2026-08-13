"""Dev-only: exercise the NVDA synth driver's logic without NVDA.

The driver imports NVDA-only modules (nvwave, synthDriverHandler, ...), so
this installs minimal stubs for them and then drives the real
`SynthDriver` class: speaking, index reporting, cancellation, and settings.
Catches logic bugs before paying the install-and-restart-NVDA round trip.

The emulator underneath is the real one -- only NVDA's shell is faked.
"""

from __future__ import annotations

import sys
import threading
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADDON = ROOT / "addon"


# --------------------------------------------------------------------------
# Minimal NVDA stubs
# --------------------------------------------------------------------------
class _FakePlayer:
    """Stands in for nvwave.WavePlayer.

    Crucially it *throttles* like the real one: WavePlayer.feed() blocks
    once its buffer is full, which is what paces the emulator (which runs
    ~10x faster than realtime) back down to realtime. Without that
    back-pressure a whole utterance renders in a fraction of a second and
    cancellation tests become meaningless.
    """

    REALTIME = True

    def __init__(self, *a, **kw):
        self.data = bytearray()
        self.stopped = False
        self.idled = 0
        self.paused = None

    def feed(self, block):
        if self.stopped:
            return
        self.data += block
        if self.REALTIME:
            # 2 bytes/sample at 10kHz; sleep most of that to emulate playback.
            time.sleep((len(block) / 2 / 10000.0) * 0.9)

    def idle(self):
        self.idled += 1

    def stop(self):
        self.stopped = True

    def pause(self, switch):
        self.paused = switch

    def close(self):
        pass


class _Notifier:
    def __init__(self, name):
        self.name = name
        self.events = []

    def notify(self, **kwargs):
        self.events.append(kwargs)


class _Setting:
    def __init__(self, *a, **kw):
        pass


class _SynthDriverBase:
    RateSetting = _Setting
    VolumeSetting = _Setting
    PitchSetting = _Setting
    InflectionSetting = _Setting
    RateBoostSetting = _Setting

    class VoiceSetting(_Setting):
        pass

    def __init__(self):
        pass

    def terminate(self):
        pass


class _VoiceInfo:
    def __init__(self, id, displayName, language=None):
        self.id, self.displayName, self.language = id, displayName, language


class IndexCommand:
    def __init__(self, index):
        self.index = index


def install_stubs():
    synthIndexReached = _Notifier("index")
    synthDoneSpeaking = _Notifier("done")

    nvwave = types.ModuleType("nvwave"); nvwave.WavePlayer = _FakePlayer
    config = types.ModuleType("config"); config.conf = {"audio": {"outputDevice": "default"}}

    logmod = types.ModuleType("logHandler")

    class _Log:
        def info(self, *a, **k): print("   [log.info]", *a)
        def warning(self, *a, **k): print("   [log.WARN]", *a)
        def error(self, *a, **k): print("   [log.ERROR]", *a)
        def debugWarning(self, *a, **k): pass
        def exception(self, *a, **k): print("   [log.EXC]", *a)
    logmod.log = _Log()

    speech = types.ModuleType("speech")
    speech_commands = types.ModuleType("speech.commands")
    speech_commands.IndexCommand = IndexCommand
    speech.commands = speech_commands

    # The driver applies line-joining only while say all is running, so the
    # harness has to be able to switch that on -- otherwise every test
    # silently exercises the ordinary navigation path.
    sayall = types.ModuleType("speech.sayAll")

    class _SayAllHandler:
        running = False

        @classmethod
        def isRunning(cls):
            return cls.running
    sayall.SayAllHandler = _SayAllHandler
    speech.sayAll = sayall
    sys.modules["speech.sayAll"] = sayall

    sdh = types.ModuleType("synthDriverHandler")
    sdh.SynthDriver = _SynthDriverBase
    sdh.VoiceInfo = _VoiceInfo
    sdh.synthIndexReached = synthIndexReached
    sdh.synthDoneSpeaking = synthDoneSpeaking

    # Mirror NVDA 2026's real module layout: the DriverSetting classes live
    # in autoSettingsUtils.driverSetting, and driverHandler does NOT
    # re-export them. An earlier version of this harness stubbed them into
    # driverHandler, which let a wrong import pass offline and then fail to
    # load inside NVDA -- stubs have to match where things actually are.
    asu = types.ModuleType("autoSettingsUtils")
    ds = types.ModuleType("autoSettingsUtils.driverSetting")
    ds.DriverSetting = _Setting
    ds.NumericDriverSetting = _Setting
    ds.BooleanDriverSetting = _Setting
    asu.driverSetting = ds
    # StringParameterInfo lives in autoSettingsUtils.utils, a *different*
    # module from the settings classes. Leaving it unstubbed made the guarded
    # import fall through to None, which silently dropped the firmware
    # selector from supportedSettings -- the test passed while testing less
    # than it appeared to.
    utils = types.ModuleType("autoSettingsUtils.utils")

    class _StringParameterInfo:
        def __init__(self, id, displayName):
            self.id, self.displayName = id, displayName
    utils.StringParameterInfo = _StringParameterInfo
    asu.utils = utils
    sys.modules["autoSettingsUtils"] = asu
    sys.modules["autoSettingsUtils.driverSetting"] = ds
    sys.modules["autoSettingsUtils.utils"] = utils
    sys.modules["driverHandler"] = types.ModuleType("driverHandler")

    gv = types.ModuleType("globalVars")
    gv.appArgs = types.SimpleNamespace(configPath=str(ROOT / "_nonexistent_config"))

    for name, mod in [("nvwave", nvwave), ("config", config), ("logHandler", logmod),
                      ("speech", speech), ("speech.commands", speech_commands),
                      ("synthDriverHandler", sdh), ("globalVars", gv)]:
        sys.modules[name] = mod
    return synthIndexReached, synthDoneSpeaking


def main() -> int:
    indexNotifier, doneNotifier = install_stubs()

    # ROMs come from the dev tree for this test.
    import os
    os.environ["DTC01_ROM_DIR"] = str(ROOT / "roms_extracted")

    sys.path.insert(0, str(ADDON))
    from synthDrivers.dectalkDtc01 import SynthDriver, findRomDir

    print(f"ROM dir resolved: {findRomDir()}")
    print(f"check(): {SynthDriver.check()}")
    assert SynthDriver.check(), "check() must pass with valid ROMs + DLL"

    synth = SynthDriver()
    print("voices:", list(synth.availableVoices) if hasattr(synth, "availableVoices")
          else list(synth._get_availableVoices()))

    # wait for the background boot
    t0 = time.time()
    synth._machineReady.wait(timeout=30)
    print(f"booted in {time.time()-t0:.2f}s; machine={'ok' if synth._machine else 'FAILED'}")
    assert synth._machine is not None

    ok = True

    # ---- boot announcement must already be consumed --------------------
    # The firmware speaks at power-on like the real hardware. If that audio
    # is still queued it lands on the first utterance, and every utterance
    # after it is heard one behind (typing "is" speaks "This").
    #
    # Checked behaviourally rather than by reading blocks off the machine
    # directly: only the driver's worker thread may execute an emulator
    # instance (Musashi keeps CPU state in globals), and the worker is still
    # booting the remaining instances at this point.
    _FakePlayer.REALTIME = False
    synth._player = _FakePlayer()
    synth.speak(["Ready"])
    synth._jobs.join()
    firstLen = len(bytes(synth._player.data)) // 2
    print(f"first utterance after boot: {firstLen/10000:.2f}s "
          f"(expect < 2.5s; ~4s means the announcement leaked into it)")
    if firstLen / 10000 >= 2.5:
        print("  ** FAIL: boot announcement was not consumed")
        ok = False
    # that probe utterance fires its own notifications; don't let them count
    # against the checks below
    indexNotifier.events.clear()
    doneNotifier.events.clear()

    # ---- settings ----------------------------------------------------
    synth._set_voice("harry")
    synth._set_rate(100)
    synth._set_volume(80)
    print(f"prefix at rate=100 voice=harry -> {synth._commandPrefix()!r}")
    assert "[:nh]" in synth._commandPrefix()
    assert f"[:ra {synth._nativeRateWpm()}]" in synth._commandPrefix()
    assert synth._machine.volume == 80, synth._machine.volume

    synth._set_voice("paul"); synth._set_rate(50)

    # ---- speak with indexes -------------------------------------------
    print("\nspeaking 3 chunks with interleaved indexes ...")
    seq = ["Hello there.", IndexCommand(1), "This is chunk two.", IndexCommand(2), "And three."]
    _FakePlayer.REALTIME = False   # measure raw synthesis speed here
    t0 = time.time()
    synth.speak(seq)
    synth._jobs.join()
    wall = time.time() - t0
    _FakePlayer.REALTIME = True    # realistic pacing for the cancel test

    audio = bytes(synth._player.data)
    idx = [e["index"] for e in indexNotifier.events]
    print(f"  wall={wall:.2f}s  audio={len(audio)//2} samples ({len(audio)//2/10000:.2f}s)")
    print(f"  indexes fired: {idx}")
    print(f"  doneSpeaking fired: {len(doneNotifier.events)}")

    if idx != [1, 2]:
        print("  ** FAIL: expected indexes [1, 2] in order"); ok = False
    if len(audio) < 10000:
        print("  ** FAIL: implausibly little audio"); ok = False
    if len(doneNotifier.events) != 1:
        print("  ** FAIL: expected exactly one doneSpeaking"); ok = False

    # index ordering vs audio: each index must fire after its chunk drained,
    # so the emulator should have been idle at each notification.
    print(f"  realtime factor: {(len(audio)//2/10000)/wall:.1f}x")

    # ---- cancel --------------------------------------------------------
    print("\ntesting cancel mid-utterance ...")
    indexNotifier.events.clear(); doneNotifier.events.clear()
    synth._player = _FakePlayer()
    long_seq = ["This is a much longer sentence that should still be speaking when we cancel it."]
    synth.speak(long_seq)
    time.sleep(0.35)          # let it get going
    synth.cancel()
    synth._jobs.join()
    cancelled_audio = len(bytes(synth._player.data)) // 2
    print(f"  audio before cancel: {cancelled_audio} samples; "
          f"doneSpeaking fired: {len(doneNotifier.events)} (expect 0)")
    if doneNotifier.events:
        print("  ** FAIL: doneSpeaking should not fire for a cancelled utterance"); ok = False

    # ---- speak again after cancel (must not replay the cancelled tail) --
    print("\nspeaking again after cancel ...")
    synth._player = _FakePlayer()
    indexNotifier.events.clear(); doneNotifier.events.clear()
    synth.speak(["Recovered."])
    synth._jobs.join()
    after = len(bytes(synth._player.data)) // 2
    print(f"  audio after recovery: {after} samples ({after/10000:.2f}s), "
          f"doneSpeaking: {len(doneNotifier.events)}")
    if after < 5000:
        print("  ** FAIL: no audio after cancel/recovery"); ok = False
    if len(doneNotifier.events) != 1:
        print("  ** FAIL: expected doneSpeaking after recovery"); ok = False

    # ---- firmware selector ---------------------------------------------
    # Switching firmware rebuilds every emulator instance, so this checks the
    # synth still speaks afterwards rather than just that the value changed.
    # Only meaningful when the ROM dir holds both sets; skipped otherwise.
    from synthDrivers.dectalkDtc01 import installedFirmwares
    installed = installedFirmwares()
    print(f"\nfirmware selector: installed = {installed}")
    if not hasattr(synth, "_get_availableFirmwares"):
        print("  (skipped: DriverSetting API unavailable)")
    elif len(installed) < 2:
        print("  (skipped: only one firmware set present in the ROM dir)")
    else:
        choices = synth._get_availableFirmwares()
        print(f"  choices: {[(k, v.displayName) for k, v in choices.items()]}")
        if list(choices) != installed:
            print("  ** FAIL: choices do not match installed firmwares"); ok = False
        # _get_/_set_ directly: the stub base class has no AutoPropertyObject
        # metaclass, so the bare `synth.firmware` property NVDA generates
        # does not exist here. Same convention as the settings block above.
        start = synth._get_firmware()
        other = next(v for v in installed if v != start)
        print(f"  switching {start} -> {other}")
        synth._set_firmware(other)
        if synth._get_firmware() != other:
            print("  ** FAIL: firmware did not change"); ok = False
        synth._machineReady.wait(timeout=60)
        if synth._machine is None:
            print("  ** FAIL: no emulator after firmware switch"); ok = False
        else:
            synth._player = _FakePlayer()
            indexNotifier.events.clear(); doneNotifier.events.clear()
            synth.speak(["Firmware switched."])
            synth._jobs.join()
            n = len(bytes(synth._player.data)) // 2
            print(f"  audio on {other}: {n} samples ({n/10000:.2f}s)")
            if n < 5000:
                print("  ** FAIL: no audio after firmware switch"); ok = False
        # Doctor Dennis and Whispery Wendy are v2.0-only. The voice list must
        # follow the firmware, and a v2.0-only voice must not survive a switch
        # to v1.8 -- otherwise [:nd] goes to a ROM that ignores it and the user
        # hears Paul while the panel says Dennis.
        voicesNow = list(synth._get_availableVoices())
        print(f"  voices on {synth._get_firmware()}: {len(voicesNow)}")
        if synth._get_firmware() == "v18":
            if "dennis" in voicesNow or "wendy" in voicesNow:
                print("  ** FAIL: v2.0-only voice offered on v1.8"); ok = False
            synth._set_voice("dennis")
            if synth._get_voice() == "dennis":
                print("  ** FAIL: v2.0-only voice accepted on v1.8"); ok = False
            else:
                print("  v2.0-only voice correctly refused on v1.8")
        else:
            for v in ("dennis", "wendy"):
                if v not in voicesNow:
                    print(f"  ** FAIL: {v} missing from v2.0 voice list"); ok = False
            synth._set_voice("dennis")
            if synth._get_voice() != "dennis":
                print("  ** FAIL: could not select Doctor Dennis on v2.0"); ok = False
            else:
                print("  Doctor Dennis selectable on v2.0")
            # ...and switching away from it must not leave it selected.
            synth._set_firmware(start)
            synth._machineReady.wait(timeout=60)
            if synth._get_voice() == "dennis" and start == "v18":
                print("  ** FAIL: Dennis survived the switch to v1.8"); ok = False

        # An unknown value must be refused, not applied -- NVDA restores
        # saved settings blindly, so a stale config must not strand the synth.
        # Compare against whatever is selected *now*: the voice checks above
        # may legitimately have switched firmware again.
        before = synth._get_firmware()
        synth._set_firmware("v99")
        if synth._get_firmware() != before:
            print("  ** FAIL: unknown firmware was accepted"); ok = False
        else:
            print("  unknown firmware correctly refused")

    synth.terminate()
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
