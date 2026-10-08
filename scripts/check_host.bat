@echo off
rem GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
rem Copyright (c) 2026 albertplastauto
rem SPDX-License-Identifier: MIT
rem
rem Compile-only syntax/semantic check of src/host/main.cpp.
rem Deliberately does NOT link, so a running grinforge.exe is neither locked nor replaced.
setlocal
set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 ( echo ERROR: VsDevCmd failed & exit /b 1 )
if not exist "%~dp0\..\build\host-check" mkdir "%~dp0\..\build\host-check"
cd /d "%~dp0\..\build\host-check"
cl /nologo /std:c++17 /EHsc /W4 /O2 ^
   /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
   /I..\..\src /I..\..\third_party\tromp-cuckoo\src\crypto /I..\..\third_party\tromp-cuckoo\src\cuckatoo ^
   /c ..\..\src\host\main.cpp /Fo:main_check.obj
if errorlevel 1 ( echo CHECK FAILED & exit /b 1 )
echo CHECK OK
