@echo off
rem Install the automatic GPU clock guard: cap 2500 MHz while grinforge.exe runs,
rem full boost otherwise (games, video). Replaces the plain logon cap task, which left
rem the limit applied all the time.
rem REQUIRES ADMINISTRATOR RIGHTS.
setlocal
set GUARD=GrinForge GPU clock guard
set OLD=GrinForge GPU clock cap 2500

echo Removing the old always-on logon task, if present...
schtasks /Delete /TN "%OLD%" /F >nul 2>&1

echo Creating the per-minute guard task...
schtasks /Create /TN "%GUARD%" /TR "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File E:\grin-miner\tools\gpu-clock-guard.ps1" /SC MINUTE /MO 1 /RL HIGHEST /F
if errorlevel 1 (
  echo.
  echo FAILED - run this file as administrator.
  exit /b 1
)

echo.
echo Task created. Verifying:
schtasks /Query /TN "%GUARD%"
echo.
echo It checks once a minute and only touches the GPU when the state changes.
echo Log:     %%ProgramData%%\grinforge-gpu-guard.log
echo Remove:  schtasks /Delete /TN "%GUARD%" /F
