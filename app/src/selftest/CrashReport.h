#pragma once

// A crash report for the UI audit (Windows only; elsewhere a no-op). The
// audit has ended in an access violation after its last step, while the app
// shut down, and only on CI. With this installed, an unhandled exception
// prints to stderr, as "ui-audit: crash ..." lines that tests/ui_audit.cmake
// shows: the exception, whether it hit the main thread, and up to 24 stack
// frames as module+offset and, where symbols load, the function name.

namespace acestudio::ui {

// Call once, on the main thread, early in main(). Only the audit uses it:
// the app itself keeps Windows' own crash handling.
void installAuditCrashReport();

} // namespace acestudio::ui
