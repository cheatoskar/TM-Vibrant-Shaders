@echo off
rem Removes TM Vibrant Shaders for ManiaPlanet (its d3d11.dll from the game folder).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
pause
