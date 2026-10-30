@echo off
rem Optional GPU check of the AI features; asks before every download.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ai-gpu-check.ps1"
