@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Stop the long-run supervisor and the miner it manages.
rem
rem IMPORTANT: the supervisor is stopped by the PID it wrote to logs\supervisor.pid, and
rem NOT by searching process command lines. Matching on the string 'run-miner-forever.ps1'
rem looks convenient but is actively dangerous: any shell whose command line merely
rem mentions that file matches as well, so this script would kill its own caller. That is
rem not hypothetical - it happened twice during development, once from a plain PowerShell
rem command and once through an earlier version of this very file, each time ending in a
rem mysterious exit code with no output. A PID file has none of that ambiguity.
setlocal enabledelayedexpansion
set PIDFILE=E:\grin-miner\logs\supervisor.pid

if exist "%PIDFILE%" (
  set /p SUPID=<"%PIDFILE%"
  echo Stopping supervisor pid !SUPID! ...
  taskkill /PID !SUPID! /F >nul 2>&1
  if errorlevel 1 (echo   supervisor was not running ^(stale pid file^)) else (echo   supervisor stopped)
  del "%PIDFILE%" >nul 2>&1
) else (
  echo No supervisor pid file, nothing to stop.
)

echo Stopping miner...
taskkill /IM grinforge.exe /F >nul 2>&1
echo Done. The GPU clock guard lifts the clock cap within a minute.
