@echo off
rem Fetch the GNU make and NASM binaries used by build_ffmpeg.cmd.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0download_ffmpeg_tools.ps1" %*
exit /b %ERRORLEVEL%
