# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# Long-run supervisor for the GrinForge miner.
#
# Purpose: grind for days without supervision, and never lose the log.
#   * one append-only log per calendar day under logs\, so nothing is overwritten;
#   * if the miner dies for any reason, it is restarted after 10 s and the restart is
#     recorded with a timestamp alongside the miner's own output;
#   * the miner's own dashboard line every 60 s gives a heartbeat in the same file.
#
# The miner keeps running with its own watchdog/thermal guard; this only handles the
# "the process is gone" case, which a watchdog inside the process cannot handle.
#
# IMPORTANT: the miner's output is redirected by cmd.exe (`>> file 2>&1`), NOT piped
# through Add-Content. A PowerShell pipeline feeding Add-Content buffers until the child
# exits, so a long-running miner would leave a nearly empty log - the first version of
# this script did exactly that and was caught by the log being 79 bytes after a minute.
#
# Started detached by run-miner-forever.bat. Stop it with stop-miner.bat.

$ErrorActionPreference = 'Continue'

$exe    = 'E:\grin-miner\build\cmake\grinforge.exe'
$wallet = 'grin1replacewithyourownaddressreplacewithyourownaddressreplacewithyourow'
$logDir = 'E:\grin-miner\logs'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
Set-Content -Path (Join-Path $logDir 'supervisor.pid') -Value $PID

$argString = '--pool grin.2miners.com:3030' +
             " --user $wallet.RIG1" +
             ' --pass x' +
             " --allow-address $wallet" +
             ' --temp-limit 80' +
             ' --api-port 4068' +
             ' --report 60'

$attempt = 0
while ($true) {
    $attempt++
    $log = Join-Path $logDir ("mining-{0}.log" -f (Get-Date -Format 'yyyy-MM-dd'))
    "[{0}] supervisor: launch #{1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $attempt |
        Out-File -FilePath $log -Append -Encoding utf8

    # cmd.exe does the append redirection and streams through, so the log grows live.
    & cmd.exe /c "`"$exe`" $argString >> `"$log`" 2>&1"
    $code = $LASTEXITCODE

    "[{0}] supervisor: miner exited with code {1}; restarting in 10 s" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $code |
        Out-File -FilePath $log -Append -Encoding utf8

    Start-Sleep -Seconds 10
}
