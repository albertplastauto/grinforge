@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Install the automatic GPU clock guard: cap 2500 MHz while grinforge.exe runs, full
rem boost otherwise (games, video). Replaces the plain logon cap task, which left the
rem limit applied all the time.
rem
rem The guard is launched through wscript.exe (tools\run-hidden.vbs) rather than
rem powershell.exe directly. A scheduled PowerShell task creates a console window that
rem flashes on screen every single minute, and -WindowStyle Hidden does not prevent it -
rem that is exactly what the first version of this script did. wscript has no console,
rem so nothing is visible.
rem
rem REQUIRES ADMINISTRATOR RIGHTS.
setlocal
set GUARD=GrinForge GPU clock guard
set OLD=GrinForge GPU clock cap 2500

echo Removing the old always-on logon cap task, if present...
schtasks /Delete /TN "%OLD%" /F >nul 2>&1
echo Removing any previous guard task, if present...
schtasks /Delete /TN "%GUARD%" /F >nul 2>&1

echo Creating the per-minute guard task (hidden launcher)...
schtasks /Create /TN "%GUARD%" /TR "wscript.exe //B \"E:\grin-miner\tools\run-hidden.vbs\" \"powershell.exe -NoProfile -ExecutionPolicy Bypass -File E:\grin-miner\tools\gpu-clock-guard.ps1\"" /SC MINUTE /MO 1 /RL HIGHEST /F
if errorlevel 1 (
  echo.
  echo FAILED - run this file as administrator.
  exit /b 1
)

echo.
echo Task created. Verifying:
schtasks /Query /TN "%GUARD%"
echo.
echo It checks once a minute, shows no window, and only touches the GPU when the state
echo changes. Log:    %%ProgramData%%\grinforge-gpu-guard.log
echo Remove: schtasks /Delete /TN "%GUARD%" /F
