@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Run the miner in a VISIBLE console window with its live dashboard, the way GPU miners
rem usually present themselves. The dashboard repaints in place instead of scrolling, and the
rem same lines are appended to logs\visible-<date>.log so a window still leaves a log behind.
rem
rem This is the interactive counterpart to run-miner-forever.bat, which runs headless under a
rem supervisor. Use that one for unattended multi-day runs.
setlocal
set ROOT=%~dp0
title GrinForge - GRIN Cuckatoo32 miner (0%% dev fee)

if not exist "%ROOT%wallet.txt" (
  echo.
  echo   wallet.txt not found. Copy wallet.txt.example to wallet.txt and put your
  echo   Grin address in it.
  echo.
  pause
  exit /b 1
)
set WALLET=
for /f "usebackq delims=" %%w in ("%ROOT%wallet.txt") do if not defined WALLET set WALLET=%%w
if "%WALLET%"=="" (
  echo   wallet.txt is empty. Put your Grin address in it.
  pause
  exit /b 1
)

for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"`) do set TODAY=%%d
if not exist "%ROOT%logs" mkdir "%ROOT%logs"

echo.
echo   GrinForge - GRIN Cuckatoo32, no developer fee
echo   wallet : %WALLET%
echo   log    : %ROOT%logs\visible-%TODAY%.log
echo   api    : http://127.0.0.1:4068/stat
echo   stop   : Ctrl+C in this window, or stop-miner.bat
echo.

"%ROOT%build\cmake\grinforge.exe" --pool grin.2miners.com:3030 ^
    --user %WALLET%.RIG1 --pass x --allow-address %WALLET% ^
    --temp-limit 80 --api-port 4068 --report 5 ^
    --log-file "%ROOT%logs\visible-%TODAY%.log"

echo.
echo   Miner exited with code %ERRORLEVEL%.
pause
