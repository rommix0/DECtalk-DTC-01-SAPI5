"""A settings category for the DECtalk DTC-01 driver's firmware location.

This lives in NVDA's own settings dialog rather than in the synthesizer's
settings, and that placement is the whole point. The driver refuses to load
without a valid ROM set, so a new installation has *no* synthesizer to open
settings for -- the one moment the user most needs to be told where the
firmware goes is the moment the synthesizer's own settings do not exist.

Shows where the driver looked and what it found, and lets the user name a
folder of their own.
"""

import os

import globalPluginHandler
import gui
import wx
from gui import guiHelper
from gui.settingsDialogs import NVDASettingsDialog, SettingsPanel
from logHandler import log

import addonHandler

try:
	addonHandler.initTranslation()
except Exception:
	def _(text):
		return text


# The driver package owns the config spec and the search order; this module
# only presents them. Imported lazily-ish but at module load, so a failure
# here disables the panel rather than breaking NVDA's settings dialog.
try:
	from synthDrivers.dectalkDtc01 import (
		configuredRomDir, defaultRomDir, ensureConfigSpec, findRomDir,
		installedFirmwares, refreshRomDirs, CONFIG_SECTION,
	)
	from synthDrivers.dectalkDtc01.emu import rom_loader
	_AVAILABLE = True
except Exception:
	log.debugWarning("DTC-01: settings panel unavailable", exc_info=True)
	_AVAILABLE = False


def _sameDir(a, b):
	"""Whether two paths name the same folder, as Windows compares them."""
	if not a or not b:
		return False
	try:
		return (os.path.normcase(os.path.normpath(a))
				== os.path.normcase(os.path.normpath(b)))
	except Exception:
		return False


class DTC01SettingsPanel(SettingsPanel):
	# Translators: the title of the DECtalk DTC-01 settings category.
	title = _("DECtalk DTC-01")

	def makeSettings(self, settingsSizer):
		helper = guiHelper.BoxSizerHelper(self, sizer=settingsSizer)

		self._status = helper.addItem(
			wx.StaticText(self, label=self._statusText()))

		# Translators: label for the ROM folder entry field.
		self._path = helper.addLabeledControl(
			_("Firmware ROM &folder (clear it to return to the default):"),
			wx.TextCtrl)
		# Pre-filled with the folder in use rather than left blank. Tabbing
		# onto an empty edit box tells a screen reader user nothing about
		# where the firmware currently lives, and the status text above is
		# easy to pass over. Blank still means "use the default", and a value
		# equal to the default is stored as blank -- see onSave.
		self._path.Value = configuredRomDir() or defaultRomDir()

		# Translators: button that opens a folder picker for the ROM folder.
		browse = helper.addItem(wx.Button(self, label=_("&Browse...")))
		browse.Bind(wx.EVT_BUTTON, self._onBrowse)

		# Translators: button that re-checks the ROM folder for firmware.
		recheck = helper.addItem(wx.Button(self, label=_("&Check now")))
		recheck.Bind(wx.EVT_BUTTON, self._onRecheck)

		helper.addItem(wx.StaticText(self, label=_(
			# Translators: note explaining when a changed folder takes effect.
			"A changed folder takes effect when the synthesizer next starts.")))

	def _statusText(self):
		"""What the driver found, and where a new install should put ROMs."""
		default = defaultRomDir()
		try:
			found = installedFirmwares()
		except Exception:
			log.debugWarning("DTC-01: could not scan for ROMs", exc_info=True)
			found = []
		if not found:
			# Translators: shown when no usable firmware was found. {folder}
			# is the folder the ROM files should be copied into.
			return _(
				"No firmware found. Copy your DTC-01 ROM files into:\n{folder}\n"
				"Filenames do not matter -- chips are recognised by content."
			).format(folder=default or _("(unknown)"))
		names = ", ".join(rom_loader.ROM_SETS[v].description for v in found)
		where = findRomDir(refresh=True, version=found[0]) or default
		# Translators: shown when firmware was found. {names} lists the
		# firmware versions, {folder} is where they were found.
		return _("Found {names}\nin: {folder}").format(names=names, folder=where)

	def _refreshStatus(self):
		refreshRomDirs()
		self._status.SetLabel(self._statusText())
		self.Layout()

	def _onBrowse(self, event):
		start = self._path.Value or defaultRomDir() or ""
		# Translators: title of the folder picker for the ROM folder.
		with wx.DirDialog(self, _("Select the folder holding your DTC-01 ROM files"),
						  defaultPath=start,
						  style=wx.DD_DEFAULT_STYLE | wx.DD_DIR_MUST_EXIST) as dialog:
			if dialog.ShowModal() == wx.ID_OK:
				self._path.Value = dialog.GetPath()

	def _onRecheck(self, event):
		# Save first so the check reflects what is typed, not what was stored.
		self.onSave()
		self._refreshStatus()

	def onSave(self):
		ensureConfigSpec()
		value = (self._path.Value or "").strip()
		# The box is pre-filled with the default, so opening the panel and
		# pressing OK would otherwise pin that literal path into the config.
		# Storing blank keeps it a *reference* to wherever NVDA's config
		# directory is, which is what makes a copy stay correct if it later
		# becomes portable, or moves.
		if value and _sameDir(value, defaultRomDir()):
			value = ""
		try:
			import config
			config.conf[CONFIG_SECTION]["romDir"] = value
		except Exception:
			log.error("DTC-01: could not save the ROM folder setting", exc_info=True)
			return
		refreshRomDirs()

	def isValid(self):
		"""Warn about a folder with no firmware, but do not block saving.

		A user may well be pointing at a folder they are about to populate,
		and refusing the whole settings dialog for that would be obstructive.
		"""
		value = (self._path.Value or "").strip()
		if value and not os.path.isdir(value):
			gui.messageBox(
				# Translators: shown when the entered ROM folder does not exist.
				_("That folder does not exist:\n{folder}").format(folder=value),
				# Translators: title of the invalid-folder message.
				_("DECtalk DTC-01"), wx.OK | wx.ICON_WARNING)
			return False
		return True


class GlobalPlugin(globalPluginHandler.GlobalPlugin):

	def __init__(self):
		super().__init__()
		if not _AVAILABLE:
			return
		if DTC01SettingsPanel not in NVDASettingsDialog.categoryClasses:
			NVDASettingsDialog.categoryClasses.append(DTC01SettingsPanel)

	def terminate(self):
		try:
			NVDASettingsDialog.categoryClasses.remove(DTC01SettingsPanel)
		except Exception:
			pass
		super().terminate()
