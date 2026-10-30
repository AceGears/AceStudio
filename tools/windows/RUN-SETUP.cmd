@echo off
rem Runs dev-setup.ps1 (this script only) without changing the PowerShell execution policy.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev-setup.ps1"
