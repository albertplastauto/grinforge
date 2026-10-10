# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# GPU clock guard: keep the 2500 MHz boost cap applied ONLY while the miner runs.
#
# Why this exists: the cap is worth ~3-7 W (same hashrate, slightly better GPS/W), which
# is a few roubles a month. Leaving it applied permanently would cost far more than that
# in games, which want the full ~3105 MHz boost. Toggling by hand needs an elevated
# console every time, so it is done here instead - once a minute, automatically.
#
# The miner itself stays unprivileged; only this small script runs with high privileges,
# and it does nothing at all when the state has not changed.
#
# Installed by install-gpu-clock-guard.bat, removed with:
#   schtasks /Delete /TN "GrinForge GPU clock guard" /F

$ErrorActionPreference = 'SilentlyContinue'

$nvidiaSmi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
if (-not (Test-Path $nvidiaSmi)) { $nvidiaSmi = 'nvidia-smi.exe' }

$stateFile = Join-Path $env:ProgramData 'grinforge-gpu-guard.state'
$logFile   = Join-Path $env:ProgramData 'grinforge-gpu-guard.log'

# Is the miner running? Any grinforge.exe counts, whoever started it.
$minerCount = @(Get-Process -Name 'grinforge' -ErrorAction SilentlyContinue).Count
$want = if ($minerCount -gt 0) { 'capped' } else { 'unlocked' }

# The driver forgets a clock lock on reboot, but the state file does not. Without tying the
# state to the current boot the guard concludes "already capped" after every restart and never
# re-applies the lock - which is exactly what happened on 2026-10-10: state said "capped" from
# the previous evening, the machine rebooted, and the miner then ran at 2790 MHz instead of
# 2490 for as long as nobody looked.
#
# Two earlier attempts at this identifier were wrong and both were caught by testing rather
# than by reading: [math]::Floor on a DateTime silently produced 0, and
# [Environment]::TickCount64 returned nothing at all in the task's language mode, which made
# the identifier equal the CURRENT time - it then changed every minute, defeating the
# "nothing to do" shortcut entirely. LastBootUpTime is the honest source for this.
$bootId = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('yyyyMMddHHmm')
$wantState = "$want|$bootId"

$have = ''
if (Test-Path $stateFile) { $have = (Get-Content -Path $stateFile -Raw).Trim() }

# Nothing to do: do not touch the GPU on every tick, so a manual profile set by the
# operator is left alone until the mining state actually flips.
if ($wantState -eq $have) { exit 0 }

$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
if ($want -eq 'capped') {
    $out = & $nvidiaSmi --lock-gpu-clocks=300,2500 2>&1
    "$stamp  miner running  -> capping to 300..2500 MHz  | $out" | Add-Content -Path $logFile
} else {
    $out = & $nvidiaSmi --reset-gpu-clocks 2>&1
    "$stamp  miner stopped  -> resetting clocks (full boost for games/video)  | $out" | Add-Content -Path $logFile
}
Set-Content -Path $stateFile -Value $wantState
exit 0
