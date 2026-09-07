"""Settings category and first-run prompt for the DECtalk DTC-01 driver.

This lives in NVDA's own settings dialog rather than in the synthesizer's
settings, and that placement is the whole point: the driver refuses to load
without a valid ROM set, so a fresh installation has *no* synthesizer whose
settings could be opened. The moment the user most needs to be told where the
firmware goes is the moment the synthesizer's own settings do not exist.

For the same reason there is a first-run prompt: if no ROMs can be found when
NVDA starts, a dialog explains what is needed and offers to open the folder to
put them in, rather than leaving the add-on silently absent from the
synthesizer list.

No ROMs ship with this add-on -- the firmware is DEC / Fonix copyright. The
user supplies their own dump; chips are recognised by content hash, so their
filenames do not matter.
"""

import os
import subprocess

import globalPluginHandler
import gui
import wx
from gui import guiHelper
from gui.settingsDialogs import NVDASettingsDialog, SettingsPanel
from logHandler import log

import addonHandler

try:
	# Binds `_` and `ngettext` in this module to the add-on's own catalogue.
	addonHandler.initTranslation()
except Exception:
	def _(text):
		return text

	def ngettext(singular, plural, n):
		return singular if n == 1 else plural


# The driver package owns the config spec and the search order; this module
# only presents them. Imported at module load, and any failure here disables
# the panel rather than breaking NVDA's settings dialog.
try:
	from synthDrivers.dectalkDtc01 import (
		CONFIG_SECTION, configuredRomDir, defaultRomDir, ensureConfigSpec,
		findRomDir, installedFirmwares, refreshRomDirs,
	)
	from synthDrivers.dectalkDtc01.emu import rom_loader
	_AVAILABLE = True
except Exception:
	log.debugWarning("DTC-01: settings panel unavailable", exc_info=True)
	_AVAILABLE = False


# The first-run prompt uses NVDA's modern message-dialog API (gui.message,
# NVDA 2025.1+). gui.runScriptModalDialog -- what an earlier draft used, and
# what the sibling DTC-03 add-on still uses -- has been deprecated since
# 2025.1 and warns on every call under the 2027.1 alpha. If the module is
# somehow absent the prompt is simply skipped: it is a convenience, and the
# settings panel below is the real way to point the driver at the firmware.
try:
	from gui.message import DefaultButton, DialogType, MessageDialog
	_HAVE_MESSAGE_DIALOG = True
except Exception:
	log.debugWarning("DTC-01: gui.message.MessageDialog unavailable", exc_info=True)
	_HAVE_MESSAGE_DIALOG = False


#: Shown once per NVDA session, not once ever: while the ROMs are missing the
#: add-on cannot work at all, so a reminder each start is informative rather
#: than nagging. Suppressed for the rest of the session once seen.
_promptedThisSession = False


def _sameDir(a, b):
	"""Whether two paths name the same folder, as Windows compares them."""
	if not a or not b:
		return False
	try:
		return (os.path.normcase(os.path.normpath(a))
				== os.path.normcase(os.path.normpath(b)))
	except Exception:
		return False


def _openFolder(path):
	"""Open `path` in Explorer, creating it first if need be.

	Creating it matters: the default location does not exist until something
	makes it, and "here is where the files go" is not much use if the folder
	is not there to paste into.
	"""
	try:
		os.makedirs(path, exist_ok=True)
	except Exception:
		log.error("DTC-01: could not create %s" % path, exc_info=True)
		return False
	try:
		os.startfile(path)                      # noqa: S606 - Windows shell
		return True
	except Exception:
		try:
			subprocess.Popen(["explorer", path])
			return True
		except Exception:
			log.error("DTC-01: could not open %s" % path, exc_info=True)
			return False


def _firmwareLabel(version):
	"""Human-readable name for a firmware key ("v20" -> "DTC-01 Version 2.0")."""
	try:
		return rom_loader.ROM_SETS[version].description
	except Exception:
		return version


def _searchedDir():
	"""The folder the driver is (or would be) looking in, for a report."""
	if not _AVAILABLE:
		return ""
	found = []
	try:
		found = installedFirmwares()
	except Exception:
		log.debugWarning("DTC-01: could not scan for ROMs", exc_info=True)
	where = None
	if found:
		where = findRomDir(refresh=True, version=found[0])
	return where or configuredRomDir() or defaultRomDir()


def _romStatus():
	"""A short sentence describing what the driver can currently find."""
	if not _AVAILABLE:
		return _("The DTC-01 driver could not be loaded.")
	try:
		found = installedFirmwares()
	except Exception:
		log.debugWarning("DTC-01: could not scan for ROMs", exc_info=True)
		found = []
	if found:
		names = ", ".join(_firmwareLabel(v) for v in found)
		# Translators: shown in the DTC-01 settings when firmware was found.
		return _("Found firmware: {names}\nIn: {path}").format(
			names=names, path=_searchedDir() or _("(unknown)"))
	# Translators: shown in the DTC-01 settings when no firmware was found.
	return _("No DTC-01 firmware found. Copy your ROM files into the folder "
	         "below; the synthesizer cannot be used until they are in place. "
	         "Filenames do not matter -- the chips are recognised by content.")


def _romReport():
	"""The same finding, at the length a dialog can afford.

	The panel's status line has to stay short enough to sit above the folder
	box; this is what "Check now" reports, so it can name every firmware it
	found, say which are still missing, and end with what to do about it.
	"""
	if not _AVAILABLE:
		# Translators: reported by "Check now" when the driver failed to load.
		return _("The DTC-01 driver could not be loaded, so its ROM files "
		         "cannot be checked. See NVDA's log for details.")

	try:
		found = installedFirmwares()
	except Exception:
		log.debugWarning("DTC-01: could not scan for ROMs", exc_info=True)
		found = []
	where = _searchedDir()
	missing = [v for v in rom_loader.ROM_SETS if v not in found]

	if not found:
		# Translators: reported by "Check now" when no complete ROM set was
		# found. {path} is the folder that was searched.
		return _("No complete DTC-01 firmware set was found.\n\n"
		         "Searched: {path}\n\n"
		         "The synthesizer cannot be used until a full set of ROM files "
		         "is in place. None are supplied with this add-on; they are DEC "
		         "/ Fonix copyright and must come from your own DTC-01.\n\n"
		         "Filenames do not matter -- the chips are recognised by "
		         "content.").format(path=where or _("(no folder set)"))

	lines = [
		# Translators: heading of the "Check now" report. {count} is how many
		# firmware versions were found.
		ngettext("Found {count} DTC-01 firmware version:",
		         "Found {count} DTC-01 firmware versions:",
		         len(found)).format(count=len(found)),
		"",
	]
	lines += ["    " + _firmwareLabel(v) for v in found]
	# Translators: in the "Check now" report; {path} is the ROM folder.
	lines += ["", _("In: {path}").format(path=where or _("(unknown)"))]
	if missing:
		# Translators: in the "Check now" report, introducing the list of
		# firmware versions whose ROM files are incomplete or absent.
		lines += ["", _("Not found, and so not offered in the synthesizer's "
		                "firmware list:"), ""]
		lines += ["    " + _firmwareLabel(v) for v in missing]
	return "\n".join(lines)


class DTC01SettingsPanel(SettingsPanel):
	# Translators: the title of the DECtalk DTC-01 settings category.
	title = _("DECtalk DTC-01")

	def makeSettings(self, settingsSizer):
		helper = guiHelper.BoxSizerHelper(self, sizer=settingsSizer)

		self._status = helper.addItem(wx.StaticText(self, label=_romStatus()))

		# Translators: label for the ROM folder entry field.
		self._romDir = helper.addLabeledControl(
			_("Firmware ROM &folder (clear it to return to the default):"),
			wx.TextCtrl)
		# Pre-filled with the folder actually in use rather than left blank:
		# tabbing onto an empty edit box tells a screen reader user nothing
		# about where the firmware currently lives, and the status text above
		# is easy to pass over. Blank still means "use the default", and a
		# value equal to the default is stored as blank -- see onSave.
		self._romDir.Value = ((configuredRomDir() or defaultRomDir())
		                      if _AVAILABLE else "")

		buttons = guiHelper.ButtonHelper(wx.HORIZONTAL)
		# Translators: button that opens a folder picker for the ROM folder.
		browse = buttons.addButton(self, label=_("&Browse..."))
		browse.Bind(wx.EVT_BUTTON, self._onBrowse)
		# Translators: button that opens the ROM folder in Explorer.
		openIt = buttons.addButton(self, label=_("&Open ROM folder"))
		openIt.Bind(wx.EVT_BUTTON, self._onOpenFolder)
		# Translators: button that re-scans the ROM folder for firmware.
		recheck = buttons.addButton(self, label=_("&Check now"))
		recheck.Bind(wx.EVT_BUTTON, self._onRecheck)
		helper.addItem(buttons)

		helper.addItem(wx.StaticText(self, label=_(
			# Translators: note under the ROM folder controls.
			"Clear the folder to use the default location. No ROM files are "
			"supplied with this add-on; they are DEC / Fonix copyright and must "
			"come from your own DTC-01. A changed folder takes effect when the "
			"synthesizer next starts.")))

	def _onBrowse(self, event):
		start = self._romDir.Value or (defaultRomDir() if _AVAILABLE else "")
		with wx.DirDialog(self,
		                  # Translators: title of the folder picker.
		                  _("Select the folder holding your DTC-01 ROM files"),
		                  defaultPath=start or "",
		                  style=wx.DD_DEFAULT_STYLE | wx.DD_DIR_MUST_EXIST) as dialog:
			if dialog.ShowModal() == wx.ID_OK:
				self._romDir.Value = dialog.GetPath()

	def _onOpenFolder(self, event):
		target = self._romDir.Value or (defaultRomDir() if _AVAILABLE else "")
		if target:
			_openFolder(target)

	def _onRecheck(self, event):
		"""Re-scan, then *report* -- in a dialog, not just on the panel.

		The panel's status line is updated too, but it is not the answer to
		the question the button asks. A label quietly changing behind the
		focused button is announced by nothing; the user has to go looking for
		it with screen review to discover whether anything happened at all. A
		modal dialog takes focus, is read out on arrival, and is dismissed with
		the OK button or Escape.
		"""
		self.onSave()
		if _AVAILABLE:
			refreshRomDirs()
		self._status.SetLabel(_romStatus())
		self.Layout()
		gui.messageBox(_romReport(),
		               # Translators: title of the dialog reporting what the
		               # "Check now" button found.
		               _("DECtalk DTC-01 firmware"),
		               wx.OK | wx.ICON_INFORMATION, self)

	def onSave(self):
		if not _AVAILABLE:
			return
		ensureConfigSpec()
		value = (self._romDir.Value or "").strip()
		# The box is pre-filled with the folder in use, so blanking it -- or
		# leaving it on the default path -- both mean "use the default".
		# Storing blank rather than the literal default keeps the setting a
		# reference to wherever NVDA's config directory is, which stays correct
		# if the copy later becomes portable, or moves.
		if not value or _sameDir(value, defaultRomDir()):
			value = ""
		try:
			import config
			config.conf[CONFIG_SECTION]["romDir"] = value
		except Exception:
			log.error("DTC-01: could not save the ROM folder setting", exc_info=True)
			return
		refreshRomDirs()

	def isValid(self):
		"""Warn about a folder that does not exist, but do not block saving.

		A user may well be pointing at a folder they are about to populate, and
		refusing the whole settings dialog for that would be obstructive. An
		empty box is always fine -- it means "use the default".
		"""
		value = (self._romDir.Value or "").strip()
		if value and not os.path.isdir(value):
			gui.messageBox(
				# Translators: shown when the entered ROM folder does not exist.
				_("That folder does not exist:\n{folder}").format(folder=value),
				# Translators: title of the invalid-folder message.
				_("DECtalk DTC-01"), wx.OK | wx.ICON_WARNING)
			return False
		return True


def _openSettingsPanel():
	"""Open NVDA's settings dialog on the DTC-01 category."""
	try:
		wx.CallAfter(gui.mainFrame.popupSettingsDialog,
		             NVDASettingsDialog, DTC01SettingsPanel)
	except Exception:
		log.error("DTC-01: could not open the settings panel", exc_info=True)


def _showFirstRunPrompt(romDir):
	"""Non-blocking dialog shown at startup when no ROMs can be found.

	Built on gui.message.MessageDialog and shown with wx.CallAfter, the
	replacement NVDA documents for the deprecated gui.runScriptModalDialog.
	Non-modal on purpose: it must not block NVDA's core during start-up, and
	the user can leave it open while they populate the folder.
	"""
	if not _HAVE_MESSAGE_DIALOG:
		log.debugWarning("DTC-01: no MessageDialog; skipping the first-run prompt")
		return
	text = _(
		"The DECtalk DTC-01 synthesizer needs the original firmware from a "
		"DTC-01 to work. None is included with this add-on: the ROM images are "
		"DEC / Fonix copyright, so you must supply a dump from your own unit.\n\n"
		"Put the ROM files in this folder, then choose DECtalk DTC-01 in NVDA's "
		"synthesizer settings. Their filenames do not matter -- the chips are "
		"recognised by content:\n\n{path}").format(path=romDir)
	dialog = MessageDialog(
		gui.mainFrame,
		text,
		# Translators: title of the dialog shown when no ROMs are installed.
		_("DECtalk DTC-01: firmware needed"),
		dialogType=DialogType.WARNING,
		buttons=None,
	)
	# Translators: button that opens the folder the ROMs go in.
	dialog.addButton(wx.ID_ANY, _("&Open this folder"),
	                 callback=lambda: _openFolder(romDir), closesDialog=False)
	# Translators: button that opens the DTC-01 settings category.
	dialog.addButton(wx.ID_ANY, _("Open &settings"), callback=_openSettingsPanel)
	# Close is also the fallback: Escape, the title-bar close, and NVDA
	# shutting down all dismiss the dialog through it.
	dialog.addCloseButton(fallbackAction=True)
	wx.CallAfter(dialog.Show)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

	def __init__(self):
		super().__init__()
		if not _AVAILABLE:
			return
		try:
			ensureConfigSpec()
			if DTC01SettingsPanel not in NVDASettingsDialog.categoryClasses:
				NVDASettingsDialog.categoryClasses.append(DTC01SettingsPanel)
		except Exception:
			log.error("DTC-01: could not register the settings panel",
			          exc_info=True)
		# Deferred: at construction NVDA's main frame may not exist yet.
		wx.CallAfter(self._promptIfNoRoms)

	def _promptIfNoRoms(self):
		global _promptedThisSession
		if _promptedThisSession or not _AVAILABLE:
			return
		try:
			refreshRomDirs()
			if installedFirmwares():
				return
			_promptedThisSession = True
			target = configuredRomDir() or defaultRomDir()
			if not target:
				return
			_showFirstRunPrompt(target)
		except Exception:
			log.debugWarning("DTC-01: first-run prompt failed", exc_info=True)

	def terminate(self):
		try:
			NVDASettingsDialog.categoryClasses.remove(DTC01SettingsPanel)
		except Exception:
			pass
		super().terminate()
