@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Undo gpu-lock-2500.bat and put the GPU back under driver control.
rem REQUIRES ADMINISTRATOR RIGHTS. Not persistent, but neither is the lock.
setlocal
echo === GPU unlock  %DATE% %TIME% ===
nvidia-smi --reset-gpu-clocks
nvidia-smi --reset-memory-clocks
echo.
echo Applied. Current state:
nvidia-smi --query-gpu=name,clocks.gr,clocks.max.gr,power.limit,power.draw --format=csv
