@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Configure and build the whole project with the VS-bundled CMake + Ninja + MSVC and
rem the NVIDIA CUDA Toolkit. Output goes to build\cmake\.
rem Adjust VSDIR / CUDAROOT below if your toolchain lives elsewhere.
setlocal
set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community
set CUDAROOT=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.4

if not exist "%VSDIR%\Common7\Tools\VsDevCmd.bat" ( echo ERROR: VS not found & exit /b 1 )
if not exist "%CUDAROOT%\bin\nvcc.exe" (
  echo ERROR: nvcc not found at "%CUDAROOT%\bin\nvcc.exe"
  echo        adjust CUDAROOT in this script
  exit /b 1
)

call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 ( echo ERROR: VsDevCmd failed & exit /b 1 )

cd /d "%~dp0\.."
cmake -S . -B build\cmake -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CUDA_COMPILER="%CUDAROOT%\bin\nvcc.exe"
if errorlevel 1 ( echo CONFIGURE FAILED & exit /b 1 )

cmake --build build\cmake
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo BUILD OK
