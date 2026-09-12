@echo off
rem build_all.bat - one-shot build + stage for the DECtalk DTC-01 SAPI5 driver
rem (Task D2). Adapted from BstSpeech-sapi-master/build_all.bat.
rem
rem Builds:
rem   1. both native DTC-01 emulator cores (tools\build_native.bat)
rem   2. the SAPI project (DectalkDtc01SAPI.dll, DectalkConfig.exe,
rem      DectalkDiagnostics.exe) for both Win32 and x64
rem and stages everything into sapi\output\ in the layout the Inno installer
rem (Task D3) expects, plus the v2.0/v1.8 ROMs if their source dirs are
rem present on this machine.
rem
rem This file lives in sapi\, but every path below is built from %~dp0 (this
rem script's own directory) rather than assumed off the caller's current
rem directory -- that way `cmd /c sapi\build_all.bat` from the repo root and
rem double-clicking it from inside sapi\ both work identically.
rem
rem Implementation note: the MSVC/Inno Setup paths contain "(x86)", and
rem cmd.exe mis-parses parenthesised paths expanded inside IF/FOR blocks --
rem see tools\build_native.bat's own comment on this. The VS-detection block
rem below is lifted from the BstSpeech template (already known to work); the
rem ROM-staging step instead follows build_native.bat's CALL-subroutine style
rem to stay clear of the trap entirely.

setlocal enabledelayedexpansion

echo DECtalk DTC-01 SAPI5 build
echo.

set "SAPI_DIR=%~dp0"
if "%SAPI_DIR:~-1%"=="\" set "SAPI_DIR=%SAPI_DIR:~0,-1%"
set "ROOT=%SAPI_DIR%\.."
set "BUILD_DIR_X86=%SAPI_DIR%\build_x86"
set "BUILD_DIR_X64=%SAPI_DIR%\build_x64"
set "OUTPUT_DIR=%SAPI_DIR%\output"
set "ROMS_ROOT=E:\Text-to-Speech Repo\TTS Hosts\DECTalk\Software\DECTalk Drivers\DECTalk DTC-01\ROMs"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022 Build Tools or later.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALLDIR=%%i"
)

if not defined VSINSTALLDIR (
    echo ERROR: No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

echo Visual Studio: %VSINSTALLDIR%
echo.

rem Clear VSINSTALLDIR again once we've confirmed/printed it: leaving it set
rem makes vcvarsall.bat (called by tools\build_native.bat below) believe the
rem dev environment is already configured, so it short-circuits without
rem putting cl.exe on PATH -- the native build then fails immediately with
rem no compiler output at all. Nothing later in this script needs the var.
set "VSINSTALLDIR="

echo === Building native cores (dtc01_x86.dll / dtc01_x64.dll) ===
call "%ROOT%\tools\build_native.bat"
if errorlevel 1 (
    echo ERROR: build_native.bat failed.
    exit /b 1
)
echo.

echo === Building SAPI project: x86 ===
cmake -A Win32 -S "%SAPI_DIR%" -B "%BUILD_DIR_X86%" || exit /b 1
cmake --build "%BUILD_DIR_X86%" --config Release || exit /b 1
echo.

echo === Building SAPI project: x64 ===
cmake -A x64 -S "%SAPI_DIR%" -B "%BUILD_DIR_X64%" || exit /b 1
cmake --build "%BUILD_DIR_X64%" --config Release || exit /b 1
echo.

echo === Staging %OUTPUT_DIR% ===
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if not exist "%OUTPUT_DIR%\x64" mkdir "%OUTPUT_DIR%\x64"

copy /Y "%BUILD_DIR_X86%\Release\DectalkDtc01SAPI.dll"   "%OUTPUT_DIR%\" >nul || exit /b 1
copy /Y "%ROOT%\build\dtc01_x86.dll"                     "%OUTPUT_DIR%\" >nul || exit /b 1
copy /Y "%BUILD_DIR_X86%\Release\DectalkConfig.exe"      "%OUTPUT_DIR%\" >nul || exit /b 1
copy /Y "%BUILD_DIR_X86%\Release\DectalkDiagnostics.exe" "%OUTPUT_DIR%\" >nul || exit /b 1
copy /Y "%BUILD_DIR_X64%\Release\DectalkDtc01SAPI.dll"   "%OUTPUT_DIR%\x64\" >nul || exit /b 1
copy /Y "%ROOT%\build\dtc01_x64.dll"                     "%OUTPUT_DIR%\x64\" >nul || exit /b 1
copy /Y "%BUILD_DIR_X64%\Release\DectalkDiagnostics.exe" "%OUTPUT_DIR%\x64\" >nul || exit /b 1
rem Note: DectalkConfig.exe is only staged at top level (32-bit) -- it runs
rem fine on 64-bit Windows and only ever touches HKCU.
echo.

echo === Staging ROMs ===
call :stage_roms "v2.0" "v20"
call :stage_roms "v1.8" "v18"
echo.

echo === Building installer ===
set "ISCC="
for %%p in (
    "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
    "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    "%ProgramFiles%\Inno Setup 6\ISCC.exe"
) do (
    if not defined ISCC if exist %%p set "ISCC=%%~p"
)

if not defined ISCC (
    echo WARNING: Inno Setup 6 not found; skipping installer.
    echo          The staged files in %OUTPUT_DIR% are complete and usable.
    goto :summary
)

set "ISS=%SAPI_DIR%\installer\DectalkDtc01SAPI.iss"
if not exist "%ISS%" (
    echo installer script not present yet; skipping
    goto :summary
)

"%ISCC%" /Q "%ISS%" || exit /b 1
echo.

:summary
echo.
echo === Build finished. Staged in %OUTPUT_DIR%: ===
if exist "%OUTPUT_DIR%\DectalkDtc01SAPI.dll"        echo   DectalkDtc01SAPI.dll        (x86)
if exist "%OUTPUT_DIR%\dtc01_x86.dll"               echo   dtc01_x86.dll
if exist "%OUTPUT_DIR%\DectalkConfig.exe"           echo   DectalkConfig.exe
if exist "%OUTPUT_DIR%\DectalkDiagnostics.exe"      echo   DectalkDiagnostics.exe      (x86)
if exist "%OUTPUT_DIR%\x64\DectalkDtc01SAPI.dll"    echo   x64\DectalkDtc01SAPI.dll
if exist "%OUTPUT_DIR%\x64\dtc01_x64.dll"           echo   x64\dtc01_x64.dll
if exist "%OUTPUT_DIR%\x64\DectalkDiagnostics.exe"  echo   x64\DectalkDiagnostics.exe
if exist "%OUTPUT_DIR%\roms\v20"                    echo   roms\v20\
if exist "%OUTPUT_DIR%\roms\v18"                    echo   roms\v18\
if exist "%OUTPUT_DIR%\DectalkDtc01SAPI_Setup.exe"  echo   DectalkDtc01SAPI_Setup.exe (installer)
endlocal
exit /b 0

rem ---- :stage_roms <ROMs subdir> <dest subdir under output\roms> ------------
rem Copies both the top-level *.rom files and DSP\*.rom flat into
rem output\roms\<dest> (non-recursive -- the engine indexes the ROM dir by
rem SHA-1 without descending into subdirs, so DSP files must sit alongside
rem the main ROMs). Missing E: source dirs are a WARNING, not a build
rem failure -- the DLL/exe staging above must still succeed either way.
:stage_roms
setlocal
set "SRC=%ROMS_ROOT%\%~1"
set "DEST=%OUTPUT_DIR%\roms\%~2"
if not exist "%SRC%" (
    echo WARNING: ROM source not found: "%SRC%"
    echo          Skipping %~2 ROM staging -- ROMs must be supplied manually.
    endlocal
    exit /b 0
)
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%SRC%\*.rom" "%DEST%\" >nul
if exist "%SRC%\DSP\*.rom" copy /Y "%SRC%\DSP\*.rom" "%DEST%\" >nul
endlocal
exit /b 0
