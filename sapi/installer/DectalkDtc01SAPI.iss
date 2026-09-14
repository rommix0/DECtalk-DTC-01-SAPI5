; Inno Setup script for the DECtalk DTC-01 SAPI5 voices.
;
; Compiled directly rather than through CPack, for the same reason as
; BstSpeech-sapi-master/installer/BestspeechSAPI.iss: two COM servers (one
; per bitness) need registering into two different registry views, and the
; rollback on uninstall has to mirror that exactly.
;
; DTC-01 is much simpler than BestSpeech's installer: no languages, no
; worker/server process to kill (the DECtalk core is in-proc at both
; bitnesses -- there is no equivalent of BestspeechServer.exe), and no
; custom-voice enumerator. What is ticked here is which firmware's ROMs get
; copied; the engine itself (sapi/src/sapi_main.cpp DllRegisterServer) then
; registers only the voice tokens whose firmware ROMs are actually present
; beside its own DLL (sapi/src/voice_registry.hpp write_voice_tokens's
; firmware filter, dtc01::available_versions) -- so the wizard's tick boxes
; and the registered voices can never drift apart. {app}\voices.ini is
; written for the record only; the engine never reads it.
;
; Build with build_all.bat, which stages output\ and then invokes ISCC on
; this file.

#define AppName "DECtalk DTC-01 SAPI5 Voices"
#define AppVersion "1.2.1"
#define AppPublisher "DECtalk DTC-01 project"
#define OutputDir "..\output"

[Setup]
AppId={{035E6ED9-7052-4CEC-98D3-DBB47A4274C3}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\DECtalkDTC01
DefaultGroupName=DECtalk DTC-01
DisableProgramGroupPage=yes
DisableDirPage=no
OutputDir={#OutputDir}
OutputBaseFilename=DectalkDtc01_SAPI_Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; The component page is the point of this installer, so it is always shown --
; even when the user picked a ready-made type -- and the Ready page lists
; what was chosen.
AlwaysShowComponentsList=yes
ShowComponentSizes=yes

; Voice tokens and the COM classes live in HKLM, so the installer needs
; elevation.
PrivilegesRequired=admin

; In 64-bit install mode {sys} is the real System32 and {syswow64} is
; SysWOW64, which is what lets each COM server be registered into the
; registry view its own hosts read.
ArchitecturesInstallIn64BitMode=x64compatible

UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\DectalkDtc01SAPI.dll

; The installer's own debug log (Inno's built-in setup log), separate from
; the engine's dectalk-sapi.log -- required by the plan so an install-time
; failure can be diagnosed without reproducing it interactively.
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
; The stock wording talks about program features; this page is choosing
; firmware voices.
WizardSelectComponents=Choose which DECtalk firmware voices to install
SelectComponentsDesc=Which DECtalk DTC-01 firmware voices should be installed?
SelectComponentsLabel2=Tick the firmware versions you want. Firmware v2.0 adds 10 voices; firmware v1.8 adds 8 voices (it does not include Doctor Dennis or Whispery Wendy). Both are ticked by default. Clear a tick to leave that firmware's voices out. Click Next when you are ready.

[Types]
Name: "full";   Description: "Everything: both firmware v2.0 and v1.8 voices"
Name: "custom"; Description: "Custom: choose the firmware yourself"; Flags: iscustom

[Components]
Name: "firmware";      Description: "DECtalk firmware voices"; Types: full custom
Name: "firmware\v20";  Description: "Firmware v2.0 (10 voices: Perfect Paul, Beautiful Betty, Huge Harry, Frail Frank, Doctor Dennis, Kit the Kid, Rough Rita, Uppity Ursula, Whispery Wendy, Variable Val)"; Types: full custom
Name: "firmware\v18";  Description: "Firmware v1.8 (8 voices: Perfect Paul, Beautiful Betty, Huge Harry, Frail Frank, Kit the Kid, Rough Rita, Uppity Ursula, Variable Val)"; Types: full custom

[Tasks]
; Optional desktop shortcut to the configuration utility, offered on its own
; wizard page so the choice is reachable with a screen reader.
Name: "desktopicon"; Description: "Create a &desktop icon for the DECtalk configuration utility"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; --- 32-bit engine + core + utilities ---
Source: "..\output\DectalkDtc01SAPI.dll";   DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "..\output\dtc01_x86.dll";          DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "..\output\DectalkConfig.exe";      DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "..\output\DectalkDiagnostics.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete

; --- 64-bit engine + core + diagnostics, for Narrator and other 64-bit hosts ---
Source: "..\output\x64\DectalkDtc01SAPI.dll";   DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "..\output\x64\dtc01_x64.dll";          DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode
Source: "..\output\x64\DectalkDiagnostics.exe"; DestDir: "{app}\x64"; Flags: ignoreversion restartreplace uninsrestartdelete; Check: Is64BitInstallMode

; --- ROMs, flat, gated by firmware component. The 32-bit engine lives in
; {app} and looks at {app}\roms; the 64-bit engine lives in {app}\x64 and
; looks at {app}\x64\roms (see DectalkTtsEngine.cpp's resolve_rom_dir), so
; each ticked firmware's ROMs go to both places in 64-bit install mode.
; ROMs are bundled, not marked private -- the project owner's call -- so
; there is no DO-NOT-DISTRIBUTE banner here.
Source: "..\output\roms\v20\*.rom"; DestDir: "{app}\roms";     Components: firmware\v20; Flags: ignoreversion uninsrestartdelete
Source: "..\output\roms\v18\*.rom"; DestDir: "{app}\roms";     Components: firmware\v18; Flags: ignoreversion uninsrestartdelete
Source: "..\output\roms\v20\*.rom"; DestDir: "{app}\x64\roms"; Components: firmware\v20; Flags: ignoreversion uninsrestartdelete; Check: Is64BitInstallMode
Source: "..\output\roms\v18\*.rom"; DestDir: "{app}\x64\roms"; Components: firmware\v18; Flags: ignoreversion uninsrestartdelete; Check: Is64BitInstallMode

[Icons]
Name: "{group}\DECtalk configuration"; Filename: "{app}\DectalkConfig.exe"
Name: "{group}\Check DECtalk voices"; Filename: "{app}\DectalkDiagnostics.exe"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\DECtalk configuration"; Filename: "{app}\DectalkConfig.exe"; Tasks: desktopicon

[UninstallDelete]
; Written from [Code] rather than copied, so the uninstaller is told about it
; by hand. This only fires on a full uninstall -- it does NOT run on a
; reconfiguring re-install (ticking a different set of firmwares and running
; the installer again), which is why CurStepChanged's ssInstall handler below
; separately wipes {app}\roms / {app}\x64\roms itself before [Files] recopies
; them: without that, [Files] only ever ADDS files, so an unticked firmware's
; ROMs would survive a re-run and available_versions() would still find them.
Type: files; Name: "{app}\voices.ini"
Type: filesandordirs; Name: "{app}\roms"
Type: filesandordirs; Name: "{app}\x64\roms"

[Code]
// Each COM server must be registered by a regsvr32 of its own bitness: the
// 32-bit one writes the voice tokens under WOW6432Node where 32-bit SAPI
// hosts look, and the 64-bit one writes them to the native view where
// Narrator and other 64-bit hosts look.
function RegisterServer(const Dll: String; Use64: Boolean; Unregister: Boolean): Boolean;
var
  Exe, Args: String;
  ResultCode: Integer;
begin
  if Use64 then
    Exe := ExpandConstant('{sys}\regsvr32.exe')
  else if Is64BitInstallMode then
    Exe := ExpandConstant('{syswow64}\regsvr32.exe')
  else
    Exe := ExpandConstant('{sys}\regsvr32.exe');

  if Unregister then
    Args := '/s /u "' + Dll + '"'
  else
    Args := '/s "' + Dll + '"';

  Result := Exec(Exe, Args, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// The record of which firmwares were ticked. Written before registration for
// the same reason as BestspeechSAPI.iss's WriteSelectionFile -- so a user can
// see what they chose -- but unlike that file, the engine does not read this
// one back: which voices actually get registered is decided by which ROM
// files are on disk (see sapi_main.cpp DllRegisterServer), not by parsing
// this file, so the two can never drift apart.
procedure WriteSelectionFile;
var
  Contents, Firmware: String;
begin
  Firmware := '';
  if WizardIsComponentSelected('firmware\v20') then
    Firmware := 'v20';
  if WizardIsComponentSelected('firmware\v18') then
  begin
    if Firmware <> '' then
      Firmware := Firmware + ',';
    Firmware := Firmware + 'v18';
  end;

  Contents :=
    '; Which DECtalk firmware was selected in the installer.' + #13#10 +
    '; This file is a record only -- the engine does NOT read it back. It' + #13#10 +
    '; registers voices by which ROM files it actually finds beside its own' + #13#10 +
    '; DLL, so this file and the registered voices can never drift apart.' + #13#10 +
    '[Selection]' + #13#10 +
    'Firmware=' + Firmware + #13#10;

  if not SaveStringToFile(ExpandConstant('{app}\voices.ini'), Contents, False) then
    MsgBox('The list of installed firmware could not be written to ' +
           ExpandConstant('{app}\voices.ini') + '.' + #13#10 + #13#10 +
           'This does not affect which voices are registered.', mbInformation, MB_OK);
end;

// Cosmetic count for the Ready-page memo only -- actual registration is
// always decided by ROM presence (DllRegisterServer), never by this. v20 ->
// 10 voices, v18 -> 8 voices, both -> 18. MUST be kept in step BY HAND with
// sapi/src/voices.hpp's VOICES table (10 v20 + 8 v18, v18 excludes Doctor
// Dennis and Whispery Wendy) -- there is no automatic link between the two.
function PickedVoiceCount(): Integer;
begin
  Result := 0;
  if WizardIsComponentSelected('firmware\v20') then
    Result := Result + 10;
  if WizardIsComponentSelected('firmware\v18') then
    Result := Result + 8;
end;

// No firmware ticked would register no voices at all, so it is caught here
// where it can still be fixed.
function NextButtonClick(PageID: Integer): Boolean;
begin
  Result := True;
  if PageID <> wpSelectComponents then
    Exit;

  if (not WizardIsComponentSelected('firmware\v20')) and
     (not WizardIsComponentSelected('firmware\v18')) then
  begin
    MsgBox('Please tick at least one firmware version.' + #13#10 + #13#10 +
           'Without a firmware there are no ROMs to install, and DECtalk DTC-01 ' +
           'would add no voices at all.', mbError, MB_OK);
    Result := False;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
                         MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine + MemoTypeInfo + NewLine + NewLine +
            MemoComponentsInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + 'Voices that will appear in your speech settings:' + NewLine +
            Space + IntToStr(PickedVoiceCount) + ' voices';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Failed: String;
begin
  if CurStep = ssInstall then
  begin
    // ssInstall fires BEFORE [Files] copies anything, so this is the one
    // place a stale ROM set can be cleared before the new one lands.
    // [Files] only ever ADDS files -- it never removes what a component
    // copied on a previous run -- so without this, unticking firmware\v18
    // on a re-install would leave its .rom files sitting in {app}\roms
    // (and {app}\x64\roms). dtc01::available_versions() would then still
    // find a complete v18 set, and DllRegisterServer would re-register all
    // 18 voices even though the wizard (and voices.ini) say v20-only --
    // the selection and the registered voices would silently drift apart.
    // DelTree(..., True, True, True) removes the directory, its files, and
    // its subdirs, and is a documented no-op if the path doesn't exist (a
    // fresh install has no {app}\roms yet), so this is safe on every run:
    // fresh install, upgrade with the same firmware, or a firmware change.
    DelTree(ExpandConstant('{app}\roms'), True, True, True);
    DelTree(ExpandConstant('{app}\x64\roms'), True, True, True);

    // Drop any previous registration first so a firmware unticked on this
    // run -- or a rename/removal of a voice between versions -- cannot leave
    // an orphaned token pointing at this engine. There is no worker process
    // to stop first: the DECtalk core is in-proc at both bitnesses.
    RegisterServer(ExpandConstant('{app}\DectalkDtc01SAPI.dll'), False, True);
    if Is64BitInstallMode then
      RegisterServer(ExpandConstant('{app}\x64\DectalkDtc01SAPI.dll'), True, True);
  end
  else if CurStep = ssPostInstall then
  begin
    // Order matters: the ROMs have to be on disk before regsvr32 runs,
    // because DllRegisterServer decides which voices to register by which
    // ROM files it finds beside its own DLL.
    WriteSelectionFile;

    Failed := '';
    if not RegisterServer(ExpandConstant('{app}\DectalkDtc01SAPI.dll'), False, False) then
      Failed := '32-bit';

    if Is64BitInstallMode then
    begin
      if not RegisterServer(ExpandConstant('{app}\x64\DectalkDtc01SAPI.dll'), True, False) then
      begin
        if Failed <> '' then
          Failed := Failed + ' and 64-bit'
        else
          Failed := '64-bit';
      end;
    end;

    if Failed <> '' then
      MsgBox('The ' + Failed + ' speech engine could not be registered. ' +
             'The DECtalk DTC-01 voices may not appear in your applications.' + #13#10 + #13#10 +
             'Try running the installer again as an administrator.', mbError, MB_OK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    if Is64BitInstallMode then
      RegisterServer(ExpandConstant('{app}\x64\DectalkDtc01SAPI.dll'), True, True);
    RegisterServer(ExpandConstant('{app}\DectalkDtc01SAPI.dll'), False, True);
  end;
end;
