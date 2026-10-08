@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Start the long-run supervisor detached from this console, so it survives the
rem terminal (and the agent session) that started it.
setlocal
if not exist "E:\grin-miner\logs" mkdir "E:\grin-miner\logs"
start "" /b powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "E:\grin-miner\run-miner-forever.ps1"
echo Supervisor started.
echo   log:  E:\grin-miner\logs\mining-YYYY-MM-DD.log
echo   stop: stop-miner.bat
