@echo off
rem Unpacks the kit next to this file (if needed) and runs setup without prompts.
cd /d "%~dp0"
if not exist "AceStudioDev\dev-setup.ps1" tar -xf AceStudioDev-kit.zip
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0AceStudioDev\dev-setup.ps1" -Yes
