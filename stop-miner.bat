@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Stop both the supervisor and the miner it manages.
rem
rem The guard `-and $_.ProcessId -ne $PID` is essential: this very command line contains
rem the string run-miner-forever.ps1, so without it the PowerShell instance that does the
rem matching terminates itself. That is not hypothetical - it happened, and the kill
rem looked like a mysterious exit code.
setlocal
echo Stopping supervisor...
powershell.exe -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object { $_.CommandLine -like '*run-miner-forever.ps1*' -and $_.ProcessId -ne $PID } | ForEach-Object { Write-Host ('  killing supervisor pid ' + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force }"
echo Stopping miner...
taskkill /IM grinforge.exe /F >nul 2>&1
echo Done. The GPU clock guard lifts the clock cap within a minute.
