// SEC-007: the app bridge checks who is at the other end of its local socket.

#include "acestudio/core/AppBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace acestudio;

TEST_CASE("App bridge name is per user", "[bridge][security]") {
    const std::string name = appBridgeName();
    CHECK(name.rfind("acestudio-bridge-", 0) == 0);
    CHECK(name.size() > std::string("acestudio-bridge-").size());
    CHECK(name == appBridgeName());
}

TEST_CASE("App bridge accepts a peer of the same user only", "[bridge][security]") {
    CHECK_FALSE(localPeerIsCurrentUser(-1, true));
    CHECK_FALSE(localPeerIsCurrentUser(-1, false));
#ifdef _WIN32
    CHECK_FALSE(localPeerIsCurrentUser(0, true));
    std::random_device rd;
    const std::wstring name = L"\\\\.\\pipe\\acestudio-test-" + std::to_wstring(rd()) + L"-" + std::to_wstring(rd());
    HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    REQUIRE(server != INVALID_HANDLE_VALUE);
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(client != INVALID_HANDLE_VALUE);
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    CHECK((connected || GetLastError() == ERROR_PIPE_CONNECTED));
    // Both ends belong to this process, so to this user.
    CHECK(localPeerIsCurrentUser(reinterpret_cast<std::intptr_t>(client), true));
    CHECK(localPeerIsCurrentUser(reinterpret_cast<std::intptr_t>(server), false));
    CloseHandle(client);
    CloseHandle(server);
#else
    int fds[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(localPeerIsCurrentUser(fds[0], true));
    CHECK(localPeerIsCurrentUser(fds[1], false));
    close(fds[0]);
    close(fds[1]);
    // Not a socket: no peer to ask about.
    int p[2] = {-1, -1};
    REQUIRE(pipe(p) == 0);
    CHECK_FALSE(localPeerIsCurrentUser(p[0], true));
    close(p[0]);
    close(p[1]);
#endif
}
