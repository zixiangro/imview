@echo off
rem Windows entry point for the minimal FFmpeg build. See build_ffmpeg.ps1.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_ffmpeg.ps1" %*
exit /b %ERRORLEVEL%
