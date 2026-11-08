#include "acestudio/core/AppBridge.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
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

#ifdef _WIN32
namespace {

// The TOKEN_USER of a token, or empty.
std::vector<BYTE> tokenUser(HANDLE token) {
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    if (len == 0) return {};
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) return {};
    return buf;
}

std::vector<BYTE> processUser(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    std::vector<BYTE> user = tokenUser(token);
    CloseHandle(token);
    return user;
}

} // namespace
#endif

bool localPeerIsCurrentUser(std::intptr_t nativeHandle, bool peerIsServer) {
#ifdef _WIN32
    HANDLE pipe = reinterpret_cast<HANDLE>(nativeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return false;
    ULONG pid = 0;
    const BOOL got = peerIsServer ? GetNamedPipeServerProcessId(pipe, &pid) : GetNamedPipeClientProcessId(pipe, &pid);
    if (!got || pid == 0) return false;
    HANDLE peer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!peer) return false;
    const std::vector<BYTE> theirs = processUser(peer);
    CloseHandle(peer);
    const std::vector<BYTE> mine = processUser(GetCurrentProcess());
    if (theirs.empty() || mine.empty()) return false;
    const auto* a = reinterpret_cast<const TOKEN_USER*>(theirs.data());
    const auto* b = reinterpret_cast<const TOKEN_USER*>(mine.data());
    return EqualSid(a->User.Sid, b->User.Sid) != FALSE;
#else
    (void)peerIsServer;   // a Unix socket's peer credentials are the same from either end
    if (nativeHandle < 0) return false;
    const int fd = static_cast<int>(nativeHandle);
#  if defined(__linux__)
    struct ucred cred {};
    socklen_t len = static_cast<socklen_t>(sizeof cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
    return cred.uid == getuid();
#  else
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0) return false;
    return uid == getuid();
#  endif
#endif
}

} // namespace acestudio
