' GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
' Copyright (c) 2026 albertplastauto
' SPDX-License-Identifier: MIT
'
' Launch a command with NO visible window.
'
' Why this exists: Task Scheduler starts the GPU clock guard once a minute, and passing
' -WindowStyle Hidden to powershell.exe is not enough - PowerShell still creates its
' console for a moment before hiding it, so a window flashed on screen every 60 seconds.
' wscript.exe is a GUI-subsystem binary with no console of its own, and Shell.Run with
' window-style 0 starts the child hidden from the very first frame.
'
' Usage:  wscript.exe run-hidden.vbs "powershell.exe -NoProfile -File <script.ps1>"

Set shell = CreateObject("WScript.Shell")
If WScript.Arguments.Count = 0 Then
    WScript.Quit 2
End If
shell.Run WScript.Arguments(0), 0, False
