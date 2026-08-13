"""Check GitHub releases for a newer DECtalk DTC-01 add-on.

Uses the public releases API, which needs no authentication or token for a
public repository. On finding a newer release it downloads the attached
.nvda-addon and asks whether to install and restart.

Deliberately conservative about failure: this runs on every NVDA start, and
a synthesizer add-on must never be made less reliable by its update check.
Every network path is wrapped, failures are logged rather than shown, and
the check runs on a daemon thread so it cannot delay or block speech.
"""

import hashlib
import os
import shutil
import threading
import time
import urllib.request

import addonHandler
import globalPluginHandler
from logHandler import log

ADDON_NAME = "dectalkDtc01"
# Set this to the published repository. Until then the check is inert.
GITHUB_REPO = "borris84/dectalk-dtc01"

RELEASES_URL = "https://api.github.com/repos/%s/releases/latest"
USER_AGENT = "DECtalk DTC-01 NVDA add-on updater"
CHECK_INTERVAL = 24 * 60 * 60      # once a day
STARTUP_DELAY = 30                 # let NVDA settle before any network work
NETWORK_TIMEOUT = 30
DOWNLOAD_TIMEOUT = 180
BLOCK = 8192


def _parseVersion(text):
	"""'v1.2.3' -> (1, 2, 3). Unparseable versions sort lowest so a
	malformed tag can never masquerade as an upgrade."""
	try:
		return tuple(int(p) for p in str(text).strip().lstrip("vV").split("."))
	except Exception:
		return (0,)


def _currentVersion():
	try:
		addon = addonHandler.getCodeAddon()
		return addon.manifest["version"]
	except Exception:
		log.debugWarning("DTC-01 updater: cannot read own version", exc_info=True)
		return None


def _fetchLatest():
	import json
	request = urllib.request.Request(RELEASES_URL % GITHUB_REPO,
									 headers={"User-Agent": USER_AGENT})
	with urllib.request.urlopen(request, timeout=NETWORK_TIMEOUT) as response:
		return json.loads(response.read().decode("utf-8"))


def _addonAsset(release):
	for asset in release.get("assets") or ():
		name = asset.get("name") or ""
		if name.lower().endswith(".nvda-addon"):
			return asset.get("browser_download_url"), name
	return None, None


def _download(url, destination):
	request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
	with urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT) as remote:
		with open(destination, "wb") as local:
			while True:
				block = remote.read(BLOCK)
				if not block:
					break
				local.write(block)


def _offerUpdate(version, url, assetName):
	"""Ask on the main thread; wx and NVDA's dialogs are not thread-safe."""
	import wx
	import gui

	def ask():
		message = _(
			"Version {version} of the DECtalk DTC-01 driver is available. "
			"Download and install it now? NVDA will restart."
		).format(version=version)
		if gui.messageBox(message, _("DECtalk DTC-01 update"),
						  wx.YES_NO | wx.ICON_QUESTION) != wx.YES:
			return
		threading.Thread(target=_installUpdate, args=(url, assetName, version),
						 name="dtc01UpdateDownload", daemon=True).start()

	wx.CallAfter(ask)


def _installUpdate(url, assetName, version):
	import tempfile
	try:
		target = os.path.join(tempfile.gettempdir(), assetName)
		_download(url, target)
	except Exception:
		log.error("DTC-01 updater: download failed", exc_info=True)
		return
	try:
		import wx
		wx.CallAfter(_finishInstall, target, version)
	except Exception:
		log.error("DTC-01 updater: could not schedule the install; "
				  f"the package is downloaded at {target}", exc_info=True)


def _bundledRomDir():
	"""<addon>/synthDrivers/dectalkDtc01/roms, if this is a --with-roms build."""
	addonRoot = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
	path = os.path.join(addonRoot, "synthDrivers", ADDON_NAME, "roms")
	return path if os.path.isdir(path) else None


def _configRomDir():
	"""<NVDA config>/dectalkDtc01/roms -- outside the add-on, so updates never
	touch it, and the driver prefers it over any bundled copy."""
	try:
		import globalVars
		base = globalVars.appArgs.configPath
	except Exception:
		appdata = os.environ.get("APPDATA")
		if not appdata:
			return None
		base = os.path.join(appdata, "nvda")
	return os.path.join(base, ADDON_NAME, "roms")


def _sha1(path):
	h = hashlib.sha1()
	with open(path, "rb") as f:
		for block in iter(lambda: f.read(BLOCK), b""):
			h.update(block)
	return h.hexdigest()


def _hashDir(path):
	"""Content hashes of every file in a directory ({} if it does not exist)."""
	if not os.path.isdir(path):
		return set()
	return {_sha1(os.path.join(path, n)) for n in os.listdir(path)
			if os.path.isfile(os.path.join(path, n))}


def _preserveBundledRoms():
	"""Move a private build's firmware somewhere an update cannot destroy it.

	A `--with-roms` package keeps the ROMs inside the add-on directory, and
	installing any update replaces that directory wholesale -- so updating a
	private build to a public release silently removed the firmware and left
	the synth unable to start. Copying it to the config directory first makes
	the update non-destructive: that location survives updates and already
	outranks the bundled copy in the driver's search order, so nothing else
	has to change.

	Returns True if it is now safe to update, False if firmware would be lost.
	"""
	bundled = _bundledRomDir()
	if not bundled:
		return True                      # public build: nothing to lose
	try:
		names = [n for n in os.listdir(bundled)
				 if os.path.isfile(os.path.join(bundled, n))]
	except Exception:
		log.error("DTC-01 updater: could not read bundled ROMs", exc_info=True)
		return False
	if not names:
		return True
	dest = _configRomDir()
	if not dest:
		log.error("DTC-01 updater: no config directory; cannot preserve ROMs")
		return False
	# Copy whatever the destination is *missing*, rather than skipping the lot
	# because it holds something. A package can bundle more than one firmware
	# (v2.0 and v1.8 since 0.6.0) while the user's own dump covers only one --
	# "already has its own dump" was true when there was only ever one set,
	# and would now let an update delete the firmware the other half needed.
	#
	# Compared by content hash, not filename: the driver identifies chips by
	# hash and dumps use arbitrary names, so equal names prove nothing and
	# different names do not mean the chip is absent.
	try:
		existingHashes = _hashDir(dest)
	except Exception:
		log.error("DTC-01 updater: could not read %s" % dest, exc_info=True)
		return False

	missing = []
	for n in names:
		try:
			digest = _sha1(os.path.join(bundled, n))
		except Exception:
			log.error("DTC-01 updater: could not hash bundled %s" % n, exc_info=True)
			return False
		if digest not in existingHashes:
			missing.append((n, digest))

	if not missing:
		log.info("DTC-01 updater: all %d bundled ROM files already present in "
				 "%s; update is safe" % (len(names), dest))
		return True

	try:
		os.makedirs(dest, exist_ok=True)
		for n, digest in missing:
			target = os.path.join(dest, n)
			if os.path.exists(target):
				# Same name, different content -- keep both rather than
				# overwrite a chip the user may be relying on.
				stem, ext = os.path.splitext(n)
				target = os.path.join(dest, "%s-%s%s" % (stem, digest[:8], ext))
			shutil.copy2(os.path.join(bundled, n), target)
			existingHashes.add(digest)
	except Exception:
		log.error("DTC-01 updater: failed to copy bundled ROMs to %s" % dest,
				  exc_info=True)
		return False
	log.info("DTC-01 updater: copied %d bundled ROM file(s) to %s so the update "
			 "cannot remove them" % (len(missing), dest))
	return True


def _finishInstall(target, version):
	"""Install the downloaded bundle. Must run on the main thread.

	Two things this has to get right, both of which it previously did not:

	1. `addonGui.installAddon` installs but does **not** offer to restart --
	   that is a separate `promptUserForRestart`. Without it the add-on sits
	   staged in `<name>.pendingInstall` and nothing applies it or says so,
	   which looks exactly like "it downloaded and then did nothing".
	2. The outcome has to be logged *after* the attempt. Logging on the way in
	   reported success for merely scheduling the call, so a failed install
	   left a log saying it had been offered and nothing else.
	"""
	import gui
	import wx
	from gui import addonGui
	# Do this before installing: the add-on directory still holds the old
	# build's firmware until NVDA restarts and applies the staged install.
	if not _preserveBundledRoms():
		if gui.messageBox(
				_("This copy of the DECtalk DTC-01 driver has firmware ROMs "
				  "bundled inside it, and they could not be copied somewhere "
				  "safe. Updating will remove them and the synthesizer will "
				  "not start until you supply a ROM dump. Update anyway?"),
				_("DECtalk DTC-01 update"),
				wx.YES_NO | wx.ICON_WARNING) != wx.YES:
			log.info("DTC-01 updater: update declined to protect bundled ROMs")
			return
	try:
		installed = addonGui.installAddon(gui.mainFrame, target)
	except Exception:
		log.error(f"DTC-01 updater: installing {version} from {target} failed",
				  exc_info=True)
		return
	if not installed:
		# Includes the user declining NVDA's own confirmation, which is not an
		# error -- but it is worth being able to tell apart from a failure.
		log.info(f"DTC-01 updater: {version} was not installed "
				 "(declined, or NVDA rejected the bundle)")
		return
	log.info(f"DTC-01 updater: {version} staged; prompting for restart")
	prompt = getattr(addonGui, "promptUserForRestart", None)
	if prompt is not None:
		prompt()
		return
	# Older/newer NVDA without that helper: ask plainly rather than leaving the
	# update silently pending, which is the bug this function exists to fix.
	if gui.messageBox(
			_("The DECtalk DTC-01 driver has been updated to version {version}. "
			  "NVDA must restart for the change to take effect. Restart now?"
			  ).format(version=version),
			_("DECtalk DTC-01 update"),
			wx.YES_NO | wx.ICON_QUESTION) == wx.YES:
		import core
		core.restart()


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

	def __init__(self):
		super().__init__()
		self._stop = threading.Event()
		if not GITHUB_REPO:
			log.debug("DTC-01 updater: no repository configured, not checking")
			return
		threading.Thread(target=self._loop, name="dtc01Updater",
						 daemon=True).start()

	def terminate(self):
		self._stop.set()
		super().terminate()

	def _loop(self):
		if self._stop.wait(STARTUP_DELAY):
			return
		while not self._stop.is_set():
			try:
				self._checkOnce()
			except Exception:
				log.debugWarning("DTC-01 updater: check failed", exc_info=True)
			if self._stop.wait(CHECK_INTERVAL):
				return

	def _checkOnce(self):
		current = _currentVersion()
		if not current:
			return
		release = _fetchLatest()
		latest = release.get("tag_name") or release.get("name") or ""
		if _parseVersion(latest) <= _parseVersion(current):
			log.debug(f"DTC-01 updater: {current} is current (latest {latest})")
			return
		url, assetName = _addonAsset(release)
		if not url:
			log.debugWarning(f"DTC-01 updater: release {latest} has no "
							 f".nvda-addon attached")
			return
		log.info(f"DTC-01 updater: {latest} available (running {current})")
		_offerUpdate(latest, url, assetName)
