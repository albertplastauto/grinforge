@echo off
rem Cap the RTX 4060 Ti core clock at 2500 MHz (boost ceiling, idle still allowed).
rem
rem WHY 2500: measured on this card with `grinforge --tune` (see
rem docs/performance-notes.md). Hashrate is identical under every GPU profile tried
rem (0.0538-0.0549 GPS, ~1% spread), so the clock buys no speed; but capping the boost
rem at ~2500 MHz gives the same GPS at clearly lower power, which is the best GPS/W
rem measured (~0.00214 vs ~0.00191 unlimited).
rem
rem The cap (not a hard pin) is deliberate: min=0 lets the card still downclock when
rem the miner is not running, so the desktop and idle power are unaffected. Under load
rem the card settles at ~2490 MHz.
rem
rem REQUIRES ADMINISTRATOR RIGHTS - nvidia-smi -lgc otherwise fails with
rem "Insufficient Permissions". Right-click -> Run as administrator.
rem
rem NOT PERSISTENT on its own: the driver forgets it on reboot. To make it permanent
rem run `install-gpu-clock-task.bat` once (creates a logon task with highest
rem privileges), or set the same cap in MSI Afterburner with "Apply at startup".
rem
rem Undo: gpu-unlock.bat, or  nvidia-smi --reset-gpu-clocks
setlocal
echo === GPU core clock cap 2500 MHz  %DATE% %TIME% ===
rem 300 is the card's documented minimum, not 0: GpuControl validates against the real
rem envelope 300..3200 MHz and refuses anything outside it, so the miner's own
rem --install-gpu-profile uses 300 too. Both let the card idle-downclock.
nvidia-smi --lock-gpu-clocks=300,2500
if errorlevel 1 (
  echo.
  echo FAILED. Most likely this window is not elevated: right-click the .bat and
  echo choose "Run as administrator".
  exit /b 1
)
echo.
echo Applied. Current state:
nvidia-smi --query-gpu=name,clocks.gr,clocks.max.gr,power.limit,power.draw --format=csv
