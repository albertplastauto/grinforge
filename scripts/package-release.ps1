# GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
# Copyright (c) 2026 albertplastauto
# SPDX-License-Identifier: MIT
#
# Build the release archive: binaries + the documents that must legally travel with them.
#
# The FAIR MINING License requires its full text to be redistributed with any binary
# built from the solver, so third_party/tromp-cuckoo/LICENSE.txt is copied into the
# package rather than merely referenced.
#
# Usage:  powershell -File scripts\package-release.ps1 -Version 0.1.0

param(
    [string]$Version = '0.1.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'build\cmake'
$dist = Join-Path $root 'build\dist'
$stage = Join-Path $dist "grinforge-$Version-windows-x64"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$binaries = @(
    'grinforge.exe',
    'solver_bench.exe',
    'solver_bench29.exe',
    'solver_bench_tiny.exe',
    'simple_ref_tiny.exe',
    'stratum_client_test.exe'
)
foreach ($name in $binaries) {
    $src = Join-Path $bin $name
    if (-not (Test-Path $src)) { throw "missing binary: $src" }
    Copy-Item $src $stage
}

# Documents: licence, attribution, the mandatory FAIR MINING text, and how to run it.
Copy-Item (Join-Path $root 'LICENSE')                      $stage
Copy-Item (Join-Path $root 'README.md')                    $stage
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'docs') | Out-Null
Copy-Item (Join-Path $root 'docs\third-party.md')          (Join-Path $stage 'docs')
Copy-Item (Join-Path $root 'docs\validated-environment.md')(Join-Path $stage 'docs')
Copy-Item (Join-Path $root 'third_party\tromp-cuckoo\LICENSE.txt') (Join-Path $stage 'FAIR-MINING-LICENSE.txt')

# Helper scripts an operator actually needs at the machine.
foreach ($name in @('gpu-lock-2500.bat', 'gpu-unlock.bat', 'install-gpu-clock-guard.bat',
                    'install-gpu-clock-task.bat', 'run-miner-forever.bat', 'run-miner-forever.ps1',
                    'stop-miner.bat', 'wallet.txt.example')) {
    Copy-Item (Join-Path $root $name) $stage
}

# install-gpu-clock-guard.bat references both of these, so leaving them out made the
# packaged guard unrunnable. Found by checking what the packaged installer actually points
# at rather than by assuming the copy list was complete.
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'tools') | Out-Null
foreach ($name in @('gpu-clock-guard.ps1', 'run-hidden.vbs')) {
    Copy-Item (Join-Path $root "tools\$name") (Join-Path $stage 'tools')
}

$zip = Join-Path $dist "grinforge-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Write-Host "package : $zip"
Write-Host "size    : $sizeMb MB"
Write-Host "sha256  : $hash"

# Machine-readable checksum file, the convention GitHub releases expect.
"$hash  grinforge-$Version-windows-x64.zip" | Out-File -FilePath "$zip.sha256" -Encoding ascii
Write-Host "sha256  : $zip.sha256"
