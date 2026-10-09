@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Start the long-run supervisor detached from this console, so it survives the terminal
rem that started it (and the agent session, if one did).
rem
rem Paths are derived from %~dp0 so the project works wherever it is unpacked.
setlocal
set ROOT=%~dp0
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
start "" /b powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%ROOT%run-miner-forever.ps1"
echo Supervisor started.
echo   log:  %ROOT%logs\mining-YYYY-MM-DD.log
echo   stop: stop-miner.bat
