AceStudio - Windows build and test kit
======================================

1. Extract this zip to a folder with a short path, e.g. C:\AceStudioDev
   long paths can break the C++ build.
2. Double-click RUN-SETUP.cmd. It explains what it installs and asks first.
   - Installs Visual Studio 2022 Build Tools (C++) and Python 3.12 if missing
     (one administrator prompt), downloads Qt 6.8.3 into this folder,
     builds AceStudio, runs all tests, puts a runnable copy in dist\.
   - Takes roughly 20-60 minutes depending on download speed.
3. Optional: double-click RUN-AI-CHECK.cmd to test the AI features on your
   GPU. It asks before every download (up to ~10 GB with SDXL).
4. Send back SUMMARY.txt, AI-SUMMARY.txt and the logs\ folder (or connect
   this folder to the Claude chat) so the results can be recorded.

To remove: delete this folder, %LOCALAPPDATA%\AceStudio\ai, and (if you
want) uninstall "Visual Studio Build Tools 2022" and Python from Settings.
