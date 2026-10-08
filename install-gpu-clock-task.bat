@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Make the 2500 MHz core-clock cap survive reboots, by running gpu-lock-2500.bat at
rem every logon with the highest privileges available.
rem REQUIRES ADMINISTRATOR RIGHTS. Reversible: see the uninstall command below.
setlocal
set TASK=GrinForge GPU clock cap 2500
schtasks /Create /TN "%TASK%" /TR "E:\grin-miner\gpu-lock-2500.bat" /SC ONLOGON /RL HIGHEST /F
if errorlevel 1 (
  echo.
  echo FAILED - run this file as administrator.
  exit /b 1
)
echo.
echo Task created. Verifying:
schtasks /Query /TN "%TASK%"
echo.
echo To remove it again:
echo   schtasks /Delete /TN "%TASK%" /F
