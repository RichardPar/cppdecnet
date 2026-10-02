@echo off
rem install-decnetd.cmd -- runs install-decnetd.ps1, whatever PowerShell's
rem script policy says.  Double-click it, or run it with the same options:
rem     install-decnetd.cmd [-DryRun DIR] [-Uninstall] [-Help]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-decnetd.ps1" %*
set rc=%errorlevel%
rem Keep a double-clicked window open long enough to read.
echo %cmdcmdline% | find /i "%~0" >nul && pause
exit /b %rc%
