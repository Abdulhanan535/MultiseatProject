@echo off
setlocal enabledelayedexpansion

:: ================================================================
::  build.bat  -  MultiseatProject local build
::  Run from the repo root.  Must have Visual Studio 2019 or 2022.
:: ================================================================

set "OUT=build"
if not exist "%OUT%" mkdir "%OUT%"

:: ── Find vcvars64.bat ────────────────────────────────────────────
set "VCVARS="
for %%V in (2022 2019) do (
    for %%E in (Enterprise Professional Community BuildTools) do (
        set "CANDIDATE=C:\Program Files\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if exist "!CANDIDATE!" (
            set "VCVARS=!CANDIDATE!"
            goto :found_vc
        )
    )
)

echo ERROR: Could not find vcvars64.bat.
echo        Install Visual Studio 2019 or 2022 (any edition).
exit /b 1

:found_vc
echo [*] Using: %VCVARS%
call "%VCVARS%" >nul 2>&1

:: ── Build 1: mutex hook DLL ──────────────────────────────────────
echo.
echo [*] Building multiseat_mutex_hook.dll ...
pushd service\mutex_hook

cl /LD /W4 /O2 /nologo /D UNICODE /D _UNICODE ^
   mutex_hook.c ^
   /link /DLL /OUT:multiseat_mutex_hook.dll ^
   kernel32.lib

if %errorlevel% neq 0 (
    echo FAILED: mutex hook DLL
    popd & exit /b 1
)
echo [OK] multiseat_mutex_hook.dll
popd

:: ── Build 2: control panel EXE ──────────────────────────────────
echo.
echo [*] Building MultiseatCtrlPanel.exe ...
pushd ui

cl /W4 /O2 /nologo /D UNICODE /D _UNICODE ^
  main.c ^
  ..\service\session_manager.c ^
  ..\service\termsrv_patch.c ^
  ..\service\dll_injector.c ^
  /I ..\service ^
  /link ^
    user32.lib gdi32.lib comctl32.lib ^
    advapi32.lib wtsapi32.lib netapi32.lib userenv.lib ^
    winsta.lib shlwapi.lib ^
    ole32.lib oleaut32.lib wbemuuid.lib ^
  /OUT:MultiseatCtrlPanel.exe /SUBSYSTEM:WINDOWS

if %errorlevel% neq 0 (
    echo FAILED: control panel EXE
    popd & exit /b 1
)
echo [OK] MultiseatCtrlPanel.exe
popd

:: ── Copy outputs ─────────────────────────────────────────────────
echo.
echo [*] Copying to %OUT%\ ...
copy /y "service\mutex_hook\multiseat_mutex_hook.dll" "%OUT%\" >nul
copy /y "ui\MultiseatCtrlPanel.exe"                   "%OUT%\" >nul

echo.
echo ================================================================
echo  Build complete.  Drop these two files in the same folder:
echo    %OUT%\MultiseatCtrlPanel.exe
echo    %OUT%\multiseat_mutex_hook.dll
echo  Then right-click MultiseatCtrlPanel.exe and Run as administrator.
echo ================================================================
