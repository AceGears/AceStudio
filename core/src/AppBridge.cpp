#include "acestudio/core/AppBridge.h"

#include <cctype>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace acestudio {

// From the OS account, not environment variables, so the app and
// acestudio-mcp agree however each was started.
std::string appBridgeName() {
#ifdef _WIN32
    wchar_t buf[257];
    DWORD n = 257;
    std::string user;
    if (GetUserNameW(buf, &n) && n > 1)
        for (DWORD i = 0; i + 1 < n; ++i) {
            const wchar_t c = buf[i];
            if (c < 128 && std::isalnum(static_cast<int>(c))) {
                user += static_cast<char>(std::tolower(static_cast<int>(c)));
            } else {
                char hex[8];   // other characters as hex, so names stay distinct
                std::snprintf(hex, sizeof hex, "_%04x", static_cast<unsigned>(c));
                user += hex;
            }
        }
    if (user.empty()) user = "user";
    return "acestudio-bridge-" + user;
#else
    return "acestudio-bridge-" + std::to_string(static_cast<unsigned long>(getuid()));
#endif
}

} // namespace acestudio
